#include "aa2acp/iap2/bluez_pairing.hpp"
#include "aa2acp/bridge/bluez_inventory.hpp"
#include "aa2acp/iap2/bluetooth_worker.hpp"

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <dbus/dbus.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace aa2acp::iap2 {

std::optional<std::uint8_t> discover_spp_channel(const std::string_view mac) {
  bdaddr_t target{};
  bdaddr_t local{};
  if (str2ba(std::string(mac).c_str(), &target) != 0) {
    return std::nullopt;
  }
  sdp_session_t *session = sdp_connect(&local, &target, SDP_RETRY_IF_BUSY);
  if (session == nullptr) {
    return std::nullopt;
  }
  uuid_t spp_uuid{};
  sdp_uuid16_create(&spp_uuid, SERIAL_PORT_SVCLASS_ID);
  sdp_list_t *search_list = sdp_list_append(nullptr, &spp_uuid);
  uint32_t range = 0x0000ffff;
  sdp_list_t *attr_ids = sdp_list_append(nullptr, &range);
  sdp_list_t *results = nullptr;

  std::optional<std::uint8_t> channel;
  if (sdp_service_search_attr_req(session, search_list, SDP_ATTR_REQ_RANGE,
                                  attr_ids, &results) == 0) {
    for (sdp_list_t *entry = results; entry != nullptr && !channel.has_value();
         entry = entry->next) {
      auto *record = static_cast<sdp_record_t *>(entry->data);
      sdp_list_t *protocols = nullptr;
      if (sdp_get_access_protos(record, &protocols) == 0) {
        for (sdp_list_t *proto_list = protocols;
             proto_list != nullptr && !channel.has_value();
             proto_list = proto_list->next) {
          for (sdp_list_t *proto = static_cast<sdp_list_t *>(proto_list->data);
               proto != nullptr; proto = proto->next) {
            const auto *descriptor =
                static_cast<const sdp_data_t *>(proto->data);
            if (descriptor == nullptr || descriptor->dtd != SDP_UUID16) {
              continue;
            }
            uuid_t rfcomm{};
            sdp_uuid16_create(&rfcomm, RFCOMM_UUID);
            if (sdp_uuid_cmp(&descriptor->val.uuid, &rfcomm) != 0) {
              continue;
            }
            // The parameter following the RFCOMM UUID is the channel.
            const auto *params = descriptor->next;
            if (params != nullptr && params->dtd == SDP_UINT8) {
              channel = static_cast<std::uint8_t>(params->val.uint8);
            }
          }
        }
        sdp_list_free(protocols, nullptr);
      }
      sdp_record_free(record);
    }
    sdp_list_free(results, [](void *data) {
      sdp_record_free(static_cast<sdp_record_t *>(data));
    });
  }
  sdp_list_free(search_list, nullptr);
  sdp_list_free(attr_ids, free);
  sdp_close(session);
  return channel;
}

namespace {

constexpr char kBluez[] = "org.bluez";
constexpr char kAgentPath[] = "/com/aa2acp/agent";
constexpr char kAgentManager[] = "org.bluez.AgentManager1";
constexpr char kAgentInterface[] = "org.bluez.Agent1";
constexpr char kAdapterPath[] = "/org/bluez/hci0";
constexpr char kAdapterInterface[] = "org.bluez.Adapter1";
constexpr char kDeviceInterface[] = "org.bluez.Device1";
constexpr char kPropertiesInterface[] = "org.freedesktop.DBus.Properties";

struct CallResult {
  bool complete{};
  bool success{};
  std::string error_name;
  std::string error_message;
};

struct AgentContext {
  const PairingLogFunction *log;
  DBusMessage *pending_confirmation{};
  std::uint64_t confirmation_id{};
  std::chrono::steady_clock::time_point confirmation_deadline{};
};

void write_log(const PairingLogFunction &log, aa2acp::bridge::LogLevel level,
               const std::string &message);

void resolve_pending_confirmation(DBusConnection *connection,
                                  AgentContext *context) {
  if (context == nullptr || context->pending_confirmation == nullptr)
    return;
  bool confirmed{};
  const bool answered =
      pairing_confirmation_result(context->confirmation_id, &confirmed);
  const bool timed_out = !answered && std::chrono::steady_clock::now() >=
                                          context->confirmation_deadline;
  if (!answered && !timed_out)
    return;
  DBusMessage *reply =
      answered && confirmed
          ? dbus_message_new_method_return(context->pending_confirmation)
          : dbus_message_new_error(context->pending_confirmation,
                                   "org.bluez.Error.Rejected",
                                   "Pairing confirmation rejected");
  if (reply != nullptr) {
    dbus_connection_send(connection, reply, nullptr);
    dbus_connection_flush(connection);
    dbus_message_unref(reply);
  }
  write_log(*context->log,
            answered && confirmed ? aa2acp::bridge::LogLevel::info
                                  : aa2acp::bridge::LogLevel::warning,
            answered
                ? (confirmed
                       ? "pairing confirmation accepted from management UI"
                       : "pairing confirmation rejected from management UI")
                : "pairing confirmation timed out");
  dbus_message_unref(context->pending_confirmation);
  context->pending_confirmation = nullptr;
  if (timed_out)
    cancel_pairing_confirmation(context->confirmation_id);
}

void reject_pending_confirmation(DBusConnection *connection,
                                 AgentContext *context, const char *reason) {
  if (context == nullptr || context->pending_confirmation == nullptr)
    return;
  DBusMessage *reply = dbus_message_new_error(
      context->pending_confirmation, "org.bluez.Error.Rejected", reason);
  if (reply != nullptr) {
    dbus_connection_send(connection, reply, nullptr);
    dbus_connection_flush(connection);
    dbus_message_unref(reply);
  }
  cancel_pairing_confirmation(context->confirmation_id);
  dbus_message_unref(context->pending_confirmation);
  context->pending_confirmation = nullptr;
}

void finish_call(DBusPendingCall *pending, void *user_data) {
  auto &result = *static_cast<CallResult *>(user_data);
  DBusMessage *reply = dbus_pending_call_steal_reply(pending);
  result.complete = true;
  result.success = reply != nullptr &&
                   dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_ERROR;
  if (!result.success && reply != nullptr) {
    const char *name = dbus_message_get_error_name(reply);
    result.error_name = name == nullptr ? "D-Bus error" : name;
    DBusError error;
    dbus_error_init(&error);
    if (dbus_set_error_from_message(&error, reply)) {
      result.error_message = error.message == nullptr ? "" : error.message;
      dbus_error_free(&error);
    }
  }
  if (reply != nullptr) {
    dbus_message_unref(reply);
  }
}

bool await_call(DBusConnection *connection, DBusMessage *message,
                const int timeout_ms, std::string &error_name,
                std::string &error_message, AgentContext *context = nullptr) {
  DBusPendingCall *pending = nullptr;
  if (dbus_connection_send_with_reply(connection, message, &pending,
                                      timeout_ms) == 0 ||
      pending == nullptr) {
    error_message = "unable to submit D-Bus call";
    return false;
  }
  CallResult result;
  dbus_pending_call_set_notify(pending, finish_call, &result, nullptr);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!result.complete && std::chrono::steady_clock::now() < deadline) {
    dbus_connection_read_write_dispatch(connection, 100);
    resolve_pending_confirmation(connection, context);
  }
  dbus_pending_call_unref(pending);
  if (!result.complete) {
    error_message = "D-Bus call timed out";
    return false;
  }
  error_name = result.error_name;
  error_message = result.error_message;
  return result.success;
}

DBusMessage *new_call(const char *path, const char *interface,
                      const char *method) {
  return dbus_message_new_method_call(kBluez, path, interface, method);
}

bool call_no_arguments(DBusConnection *connection, const char *path,
                       const char *interface, const char *method,
                       const int timeout_ms, std::string &name,
                       std::string &detail, AgentContext *context = nullptr) {
  DBusMessage *message = new_call(path, interface, method);
  if (message == nullptr) {
    detail = "unable to allocate D-Bus message";
    return false;
  }
  const bool result =
      await_call(connection, message, timeout_ms, name, detail, context);
  dbus_message_unref(message);
  return result;
}

std::string agent_request_detail(DBusMessage *message) {
  DBusMessageIter arguments;
  if (!dbus_message_iter_init(message, &arguments))
    return {};

  std::ostringstream detail;
  const int first_type = dbus_message_iter_get_arg_type(&arguments);
  if (first_type == DBUS_TYPE_OBJECT_PATH) {
    const char *device = nullptr;
    dbus_message_iter_get_basic(&arguments, &device);
    detail << " for " << (device == nullptr ? "unknown device" : device);
    dbus_message_iter_next(&arguments);
  }
  const int second_type = dbus_message_iter_get_arg_type(&arguments);
  if (second_type == DBUS_TYPE_UINT32) {
    dbus_uint32_t value{};
    dbus_message_iter_get_basic(&arguments, &value);
    detail << " with value " << value;
  } else if (second_type == DBUS_TYPE_STRING) {
    const char *value = nullptr;
    dbus_message_iter_get_basic(&arguments, &value);
    detail << " for service " << (value == nullptr ? "unknown" : value);
  }
  return detail.str();
}

bool agent_handler(DBusConnection *connection, DBusMessage *message,
                   void *user_data) {
  if (!dbus_message_is_method_call(message, kAgentInterface, "Release") &&
      !dbus_message_is_method_call(message, kAgentInterface, "Cancel") &&
      !dbus_message_is_method_call(message, kAgentInterface,
                                   "RequestConfirmation") &&
      !dbus_message_is_method_call(message, kAgentInterface,
                                   "RequestAuthorization") &&
      !dbus_message_is_method_call(message, kAgentInterface,
                                   "AuthorizeService") &&
      !dbus_message_is_method_call(message, kAgentInterface,
                                   "DisplayPasskey") &&
      !dbus_message_is_method_call(message, kAgentInterface,
                                   "RequestPinCode") &&
      !dbus_message_is_method_call(message, kAgentInterface,
                                   "RequestPasskey")) {
    return false;
  }
  const auto *context = static_cast<AgentContext *>(user_data);
  const char *member = dbus_message_get_member(message);
  if (context != nullptr && context->log != nullptr) {
    write_log(*context->log, aa2acp::bridge::LogLevel::info,
              "pairing agent request " +
                  std::string(member == nullptr ? "unknown" : member) +
                  agent_request_detail(message));
  }
  auto *mutable_context = const_cast<AgentContext *>(context);
  if ((dbus_message_is_method_call(message, kAgentInterface, "Cancel") ||
       dbus_message_is_method_call(message, kAgentInterface, "Release")) &&
      mutable_context != nullptr) {
    reject_pending_confirmation(connection, mutable_context,
                                "Pairing confirmation cancelled");
  }
  if (dbus_message_is_method_call(message, kAgentInterface,
                                  "RequestConfirmation") &&
      mutable_context != nullptr &&
      mutable_context->pending_confirmation != nullptr) {
    DBusMessage *rejection = dbus_message_new_error(
        message, "org.bluez.Error.Rejected",
        "Another pairing confirmation is already pending");
    if (rejection != nullptr) {
      dbus_connection_send(connection, rejection, nullptr);
      dbus_connection_flush(connection);
      dbus_message_unref(rejection);
    }
    write_log(*mutable_context->log, aa2acp::bridge::LogLevel::warning,
              "rejected concurrent pairing confirmation request");
    return true;
  }
  if (dbus_message_is_method_call(message, kAgentInterface,
                                  "RequestConfirmation") &&
      mutable_context != nullptr &&
      mutable_context->pending_confirmation == nullptr) {
    DBusMessageIter arguments;
    dbus_message_iter_init(message, &arguments);
    const char *device = nullptr;
    dbus_message_iter_get_basic(&arguments, &device);
    dbus_message_iter_next(&arguments);
    dbus_uint32_t passkey{};
    dbus_message_iter_get_basic(&arguments, &passkey);
    std::uint64_t id{};
    if (request_pairing_confirmation(device == nullptr ? "" : device, passkey,
                                     &id)) {
      mutable_context->pending_confirmation = dbus_message_ref(message);
      mutable_context->confirmation_id = id;
      mutable_context->confirmation_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(50);
      write_log(*mutable_context->log, aa2acp::bridge::LogLevel::info,
                "pairing confirmation is awaiting management UI approval");
      return true;
    }
    write_log(*mutable_context->log, aa2acp::bridge::LogLevel::error,
              "unable to request management UI pairing confirmation");
    DBusMessage *rejection = dbus_message_new_error(
        message, "org.bluez.Error.Rejected",
        "Unable to request management UI pairing confirmation");
    if (rejection != nullptr) {
      dbus_connection_send(connection, rejection, nullptr);
      dbus_connection_flush(connection);
      dbus_message_unref(rejection);
    }
    return true;
  }
  if (dbus_message_is_method_call(message, kAgentInterface, "RequestPinCode") ||
      dbus_message_is_method_call(message, kAgentInterface, "RequestPasskey")) {
    DBusMessage *rejection = dbus_message_new_error(
        message, "org.bluez.Error.Rejected",
        "AA2ACP only supports Numeric Comparison pairing");
    if (rejection != nullptr) {
      dbus_connection_send(connection, rejection, nullptr);
      dbus_connection_flush(connection);
      dbus_message_unref(rejection);
    }
    if (context != nullptr && context->log != nullptr) {
      write_log(*context->log, aa2acp::bridge::LogLevel::warning,
                "rejected unsupported PIN/passkey pairing request");
    }
    return true;
  }
  DBusMessage *reply = dbus_message_new_method_return(message);
  if (reply == nullptr) {
    return true;
  }
  dbus_connection_send(connection, reply, nullptr);
  dbus_connection_flush(connection);
  dbus_message_unref(reply);
  return true;
}

DBusHandlerResult agent_vtable_handler(DBusConnection *connection,
                                       DBusMessage *message, void *data) {
  return agent_handler(connection, message, data)
             ? DBUS_HANDLER_RESULT_HANDLED
             : DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

DBusObjectPathVTable kAgentVTable{
    nullptr, agent_vtable_handler, nullptr, nullptr, nullptr, nullptr};

std::string device_path(const std::string_view mac) {
  std::string path = "/org/bluez/hci0/dev_";
  for (const char character : mac) {
    path.push_back(character == ':' ? '_' : character);
  }
  return path;
}

bool call_register_agent(DBusConnection *connection, const char *method,
                         std::string &name, std::string &detail) {
  DBusMessage *message = new_call("/org/bluez", kAgentManager, method);
  if (message == nullptr) {
    detail = "unable to allocate D-Bus message";
    return false;
  }
  const char *agent_path = kAgentPath;
  // Numeric Comparison is widely used by CarPlay head units.  DisplayYesNo
  // allows BlueZ to select it, while the agent can accept confirmation without
  // requiring a separate display or input device.
  const char *capability = "DisplayYesNo";
  dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &agent_path,
                           DBUS_TYPE_STRING, &capability, DBUS_TYPE_INVALID);
  const bool result = await_call(connection, message, 5000, name, detail);
  dbus_message_unref(message);
  return result;
}

bool call_unregister_agent(DBusConnection *connection, std::string &name,
                           std::string &detail) {
  DBusMessage *message =
      new_call("/org/bluez", kAgentManager, "UnregisterAgent");
  if (message == nullptr) {
    detail = "unable to allocate D-Bus message";
    return false;
  }
  const char *agent_path = kAgentPath;
  dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &agent_path,
                           DBUS_TYPE_INVALID);
  const bool result = await_call(connection, message, 5000, name, detail);
  dbus_message_unref(message);
  return result;
}

bool call_request_default_agent(DBusConnection *connection, std::string &name,
                                std::string &detail) {
  DBusMessage *message =
      new_call("/org/bluez", kAgentManager, "RequestDefaultAgent");
  if (message == nullptr) {
    detail = "unable to allocate D-Bus message";
    return false;
  }
  const char *agent_path = kAgentPath;
  dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &agent_path,
                           DBUS_TYPE_INVALID);
  const bool result = await_call(connection, message, 5000, name, detail);
  dbus_message_unref(message);
  return result;
}

bool call_set_trusted(DBusConnection *connection, const std::string &path,
                      std::string &name, std::string &detail) {
  DBusMessage *message = new_call(path.c_str(), kPropertiesInterface, "Set");
  if (message == nullptr) {
    detail = "unable to allocate D-Bus message";
    return false;
  }
  DBusMessageIter arguments;
  DBusMessageIter variant;
  const char *interface = kDeviceInterface;
  const char *property = "Trusted";
  const dbus_bool_t trusted = true;
  dbus_message_iter_init_append(message, &arguments);
  dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING, &interface);
  dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING, &property);
  dbus_message_iter_open_container(&arguments, DBUS_TYPE_VARIANT,
                                   DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
  dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &trusted);
  dbus_message_iter_close_container(&arguments, &variant);
  const bool result = await_call(connection, message, 5000, name, detail);
  dbus_message_unref(message);
  return result;
}

bool call_set_le_discovery_filter(DBusConnection *connection, std::string &name,
                                  std::string &detail) {
  DBusMessage *message =
      new_call(kAdapterPath, kAdapterInterface, "SetDiscoveryFilter");
  if (message == nullptr) {
    detail = "unable to allocate D-Bus message";
    return false;
  }
  DBusMessageIter arguments;
  DBusMessageIter dictionary;
  DBusMessageIter entry;
  DBusMessageIter variant;
  const char *key = "Transport";
  const char *transport = "le";
  dbus_message_iter_init_append(message, &arguments);
  dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY, "{sv}",
                                   &dictionary);
  dbus_message_iter_open_container(&dictionary, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &entry);
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                   DBUS_TYPE_STRING_AS_STRING, &variant);
  dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &transport);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&dictionary, &entry);
  dbus_message_iter_close_container(&arguments, &dictionary);
  const bool result = await_call(connection, message, 5000, name, detail);
  dbus_message_unref(message);
  return result;
}

void write_log(const PairingLogFunction &log,
               const aa2acp::bridge::LogLevel level,
               const std::string &message) {
  if (log) {
    log(level, message);
  } else {
    aa2acp::bridge::log(level) << "Bluetooth: " << message << '\n';
  }
}

bool same_address(const std::string_view left, const std::string_view right) {
  if (left.size() != right.size())
    return false;
  return std::equal(left.begin(), left.end(), right.begin(),
                    [](const char a, const char b) {
                      return std::toupper(static_cast<unsigned char>(a)) ==
                             std::toupper(static_cast<unsigned char>(b));
                    });
}

bool device_visible(const std::string_view mac) {
  std::string error;
  const auto devices = aa2acp::bridge::list_bluez_devices(&error);
  return std::any_of(devices.begin(), devices.end(), [mac](const auto &device) {
    return same_address(device.address, mac);
  });
}

} // namespace

bool ensure_bluez_pairing(const std::string_view mac, const int timeout_seconds,
                          const PairingLogFunction &log) {
  DBusError error;
  dbus_error_init(&error);
  DBusConnection *connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
  if (connection == nullptr) {
    write_log(log, aa2acp::bridge::LogLevel::error,
              std::string("cannot connect to system D-Bus: ") +
                  (error.message == nullptr ? "unknown error" : error.message));
    dbus_error_free(&error);
    return false;
  }
  const auto path = device_path(mac);
  if (aa2acp::bridge::bluez_device_is_paired(mac)) {
    write_log(log, aa2acp::bridge::LogLevel::info,
              "reusing existing BlueZ bond for " + std::string(mac));
    std::string name;
    std::string detail;
    if (!call_set_trusted(connection, path, name, detail)) {
      write_log(log, aa2acp::bridge::LogLevel::warning,
                "setting Trusted failed: " + name + " " + detail);
      return false;
    }
    return true;
  }
  AgentContext agent_context{&log};
  if (!dbus_connection_register_object_path(connection, kAgentPath,
                                            &kAgentVTable, &agent_context)) {
    write_log(log, aa2acp::bridge::LogLevel::error,
              "cannot register BlueZ pairing agent");
    return false;
  }

  std::string name;
  std::string detail;
  if (!call_register_agent(connection, "RegisterAgent", name, detail)) {
    write_log(log, aa2acp::bridge::LogLevel::error,
              "RegisterAgent failed: " + name + " " + detail);
    dbus_connection_unregister_object_path(connection, kAgentPath);
    return false;
  }
  const auto cleanup_agent = [&] {
    reject_pending_confirmation(connection, &agent_context,
                                "Pairing session ended");
    std::string cleanup_name;
    std::string cleanup_detail;
    if (!call_unregister_agent(connection, cleanup_name, cleanup_detail) &&
        cleanup_name != "org.bluez.Error.DoesNotExist") {
      write_log(log, aa2acp::bridge::LogLevel::warning,
                "UnregisterAgent failed: " + cleanup_name + " " +
                    cleanup_detail);
    }
    dbus_connection_unregister_object_path(connection, kAgentPath);
  };
  if (!call_request_default_agent(connection, name, detail)) {
    write_log(log, aa2acp::bridge::LogLevel::warning,
              "RequestDefaultAgent failed: " + name + " " + detail);
  }

  if (!call_set_le_discovery_filter(connection, name, detail)) {
    write_log(log, aa2acp::bridge::LogLevel::warning,
              "SetDiscoveryFilter(le) failed: " + name + " " + detail);
  }
  if (!call_no_arguments(connection, kAdapterPath, kAdapterInterface,
                         "StartDiscovery", 5000, name, detail)) {
    write_log(log, aa2acp::bridge::LogLevel::warning,
              "StartDiscovery failed: " + name + " " + detail);
  } else {
    write_log(log, aa2acp::bridge::LogLevel::info,
              "scanning for " + std::string(mac));
  }
  // After a bond is removed, BlueZ removes its Device1 object too. A head unit
  // can take several inquiry intervals to reappear, so five seconds is not
  // enough for a real first-pairing attempt.
  // The overall CarPlay phase permits a minute, but discovery should remain a
  // bounded user-visible operation rather than consuming that whole budget.
  const auto discovery_seconds = std::clamp(timeout_seconds, 12, 30);
  const auto discovery_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(discovery_seconds);
  bool found = false;
  while (std::chrono::steady_clock::now() < discovery_deadline) {
    dbus_connection_read_write_dispatch(connection, 200);
    if (device_visible(mac)) {
      found = true;
      write_log(log, aa2acp::bridge::LogLevel::info,
                "found " + std::string(mac) + " during discovery");
      break;
    }
  }
  if (!found) {
    write_log(log, aa2acp::bridge::LogLevel::error,
              "discovery timeout reached for " + std::string(mac));
    std::string cleanup_name;
    std::string cleanup_detail;
    call_no_arguments(connection, kAdapterPath, kAdapterInterface,
                      "StopDiscovery", 5000, cleanup_name, cleanup_detail);
    cleanup_agent();
    write_log(log, aa2acp::bridge::LogLevel::error,
              "Bluetooth device " + std::string(mac) +
                  " was not found; pairing was not attempted");
    return false;
  }
  write_log(log, aa2acp::bridge::LogLevel::info,
            "pairing " + std::string(mac) +
                " using DisplayYesNo (management confirmation required)");
  const int pair_timeout = timeout_seconds > 0 ? timeout_seconds * 1000 : 60000;
  // Device1 objects can disappear and be recreated while discovery updates a
  // freshly removed bond. Retry the method on the stable address-derived path
  // rather than treating that transient UnknownObject as a pairing failure.
  bool paired = false;
  for (int attempt = 0; attempt < 3 && !paired; ++attempt) {
    paired =
        call_no_arguments(connection, path.c_str(), kDeviceInterface, "Pair",
                          pair_timeout, name, detail, &agent_context);
    if (!paired && name == "org.freedesktop.DBus.Error.UnknownObject" &&
        attempt < 2) {
      write_log(log, aa2acp::bridge::LogLevel::warning,
                "device object changed during discovery; retrying pair");
      const auto retry_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (std::chrono::steady_clock::now() < retry_deadline)
        dbus_connection_read_write_dispatch(connection, 100);
    }
  }
  const auto pair_error_name = name;
  const auto pair_error_detail = detail;
  std::string cleanup_name;
  std::string cleanup_detail;
  call_no_arguments(connection, kAdapterPath, kAdapterInterface,
                    "StopDiscovery", 5000, cleanup_name, cleanup_detail);
  const bool already_paired =
      pair_error_name == "org.bluez.Error.AlreadyExists" ||
      pair_error_name == "org.bluez.Error.AlreadyPaired";
  if (!paired && !already_paired) {
    write_log(log, aa2acp::bridge::LogLevel::error,
              "Pair failed: " + pair_error_name + " " + pair_error_detail);
    cleanup_agent();
    return false;
  }
  if (already_paired) {
    write_log(log, aa2acp::bridge::LogLevel::info, "device was already paired");
  } else {
    write_log(log, aa2acp::bridge::LogLevel::info, "pairing succeeded");
  }
  if (!call_set_trusted(connection, path, name, detail)) {
    write_log(log, aa2acp::bridge::LogLevel::warning,
              "setting Trusted failed: " + name + " " + detail);
    cleanup_agent();
    return false;
  }
  write_log(log, aa2acp::bridge::LogLevel::info, "device is trusted");
  cleanup_agent();
  return true;
}

} // namespace aa2acp::iap2
