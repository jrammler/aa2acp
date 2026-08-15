#include "acp/iap2/bluez_pairing.hpp"

#include <dbus/dbus.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace acp::iap2 {
namespace {

constexpr char kBluez[] = "org.bluez";
constexpr char kAgentPath[] = "/com/acp_aabridge/agent";
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

void finish_call(DBusPendingCall* pending, void* user_data) {
    auto& result = *static_cast<CallResult*>(user_data);
    DBusMessage* reply = dbus_pending_call_steal_reply(pending);
    result.complete = true;
    result.success = reply != nullptr && dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_ERROR;
    if (!result.success && reply != nullptr) {
        const char* name = dbus_message_get_error_name(reply);
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

bool await_call(DBusConnection* connection, DBusMessage* message, const int timeout_ms,
                std::string& error_name, std::string& error_message) {
    DBusPendingCall* pending = nullptr;
    if (dbus_connection_send_with_reply(connection, message, &pending, timeout_ms) == 0 || pending == nullptr) {
        error_message = "unable to submit D-Bus call";
        return false;
    }
    CallResult result;
    dbus_pending_call_set_notify(pending, finish_call, &result, nullptr);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!result.complete && std::chrono::steady_clock::now() < deadline) {
        dbus_connection_read_write_dispatch(connection, 100);
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

DBusMessage* new_call(const char* path, const char* interface, const char* method) {
    return dbus_message_new_method_call(kBluez, path, interface, method);
}

bool call_no_arguments(DBusConnection* connection, const char* path, const char* interface,
                       const char* method, const int timeout_ms, std::string& name, std::string& detail) {
    DBusMessage* message = new_call(path, interface, method);
    if (message == nullptr) {
        detail = "unable to allocate D-Bus message";
        return false;
    }
    const bool result = await_call(connection, message, timeout_ms, name, detail);
    dbus_message_unref(message);
    return result;
}

bool agent_handler(DBusConnection* connection, DBusMessage* message, void*) {
    if (!dbus_message_is_method_call(message, kAgentInterface, "Release") &&
        !dbus_message_is_method_call(message, kAgentInterface, "Cancel") &&
        !dbus_message_is_method_call(message, kAgentInterface, "RequestConfirmation") &&
        !dbus_message_is_method_call(message, kAgentInterface, "RequestAuthorization") &&
        !dbus_message_is_method_call(message, kAgentInterface, "AuthorizeService") &&
        !dbus_message_is_method_call(message, kAgentInterface, "DisplayPasskey") &&
        !dbus_message_is_method_call(message, kAgentInterface, "RequestPinCode") &&
        !dbus_message_is_method_call(message, kAgentInterface, "RequestPasskey")) {
        return false;
    }
    DBusMessage* reply = dbus_message_new_method_return(message);
    if (reply == nullptr) {
        return true;
    }
    if (dbus_message_is_method_call(message, kAgentInterface, "RequestPinCode")) {
        const char* pin = "0000";
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &pin, DBUS_TYPE_INVALID);
    } else if (dbus_message_is_method_call(message, kAgentInterface, "RequestPasskey")) {
        const dbus_uint32_t passkey = 0;
        dbus_message_append_args(reply, DBUS_TYPE_UINT32, &passkey, DBUS_TYPE_INVALID);
    }
    dbus_connection_send(connection, reply, nullptr);
    dbus_connection_flush(connection);
    dbus_message_unref(reply);
    return true;
}

DBusHandlerResult agent_vtable_handler(DBusConnection* connection, DBusMessage* message, void* data) {
    return agent_handler(connection, message, data) ? DBUS_HANDLER_RESULT_HANDLED
                                                     : DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

DBusObjectPathVTable kAgentVTable{nullptr, agent_vtable_handler, nullptr, nullptr, nullptr, nullptr};

std::string device_path(const std::string_view mac) {
    std::string path = "/org/bluez/hci0/dev_";
    for (const char character : mac) {
        path.push_back(character == ':' ? '_' : character);
    }
    return path;
}

bool call_register_agent(DBusConnection* connection, const char* method, std::string& name, std::string& detail) {
    DBusMessage* message = new_call("/org/bluez", kAgentManager, method);
    if (message == nullptr) {
        detail = "unable to allocate D-Bus message";
        return false;
    }
    const char* agent_path = kAgentPath;
    const char* capability = "NoInputNoOutput";
    dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &agent_path, DBUS_TYPE_STRING, &capability,
                             DBUS_TYPE_INVALID);
    const bool result = await_call(connection, message, 5000, name, detail);
    dbus_message_unref(message);
    return result;
}

bool call_request_default_agent(DBusConnection* connection, std::string& name, std::string& detail) {
    DBusMessage* message = new_call("/org/bluez", kAgentManager, "RequestDefaultAgent");
    if (message == nullptr) {
        detail = "unable to allocate D-Bus message";
        return false;
    }
    const char* agent_path = kAgentPath;
    dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &agent_path, DBUS_TYPE_INVALID);
    const bool result = await_call(connection, message, 5000, name, detail);
    dbus_message_unref(message);
    return result;
}

bool call_set_trusted(DBusConnection* connection, const std::string& path, std::string& name, std::string& detail) {
    DBusMessage* message = new_call(path.c_str(), kPropertiesInterface, "Set");
    if (message == nullptr) {
        detail = "unable to allocate D-Bus message";
        return false;
    }
    DBusMessageIter arguments;
    DBusMessageIter variant;
    const char* interface = kDeviceInterface;
    const char* property = "Trusted";
    const dbus_bool_t trusted = true;
    dbus_message_iter_init_append(message, &arguments);
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING, &interface);
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING, &property);
    dbus_message_iter_open_container(&arguments, DBUS_TYPE_VARIANT, DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &trusted);
    dbus_message_iter_close_container(&arguments, &variant);
    const bool result = await_call(connection, message, 5000, name, detail);
    dbus_message_unref(message);
    return result;
}

void write_log(const PairingLogFunction& log, const std::string& message) {
    if (log) {
        log(message);
    } else {
        std::cout << "Bluetooth: " << message << '\n';
    }
}

}  // namespace

bool ensure_bluez_pairing(const std::string_view mac, const int timeout_seconds, const PairingLogFunction& log) {
    DBusError error;
    dbus_error_init(&error);
    DBusConnection* connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (connection == nullptr) {
        write_log(log, std::string("cannot connect to system D-Bus: ") +
                           (error.message == nullptr ? "unknown error" : error.message));
        dbus_error_free(&error);
        return false;
    }
    if (!dbus_connection_register_object_path(connection, kAgentPath, &kAgentVTable, nullptr)) {
        write_log(log, "cannot register BlueZ pairing agent");
        return false;
    }

    std::string name;
    std::string detail;
    if (!call_register_agent(connection, "RegisterAgent", name, detail)) {
        write_log(log, "RegisterAgent failed: " + name + " " + detail);
        dbus_connection_unregister_object_path(connection, kAgentPath);
        return false;
    }
    if (!call_request_default_agent(connection, name, detail)) {
        write_log(log, "RequestDefaultAgent failed: " + name + " " + detail);
    }

    if (!call_no_arguments(connection, kAdapterPath, kAdapterInterface, "StartDiscovery", 5000, name, detail)) {
        write_log(log, "StartDiscovery failed: " + name + " " + detail);
    } else {
        write_log(log, "scanning for " + std::string(mac));
    }
    const auto path = device_path(mac);
    // After a bond is removed, BlueZ removes its Device1 object too. test head unit can
    // take several inquiry intervals to reappear, so five seconds is not
    // enough for a real first-pairing attempt.
    const auto discovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < discovery_deadline) {
        dbus_connection_read_write_dispatch(connection, 200);
    }
    write_log(log, "pairing " + std::string(mac) + " using NoInputNoOutput (Just Works)");
    const int pair_timeout = timeout_seconds > 0 ? timeout_seconds * 1000 : 60000;
    const bool paired = call_no_arguments(connection, path.c_str(), kDeviceInterface, "Pair", pair_timeout, name, detail);
    const auto pair_error_name = name;
    const auto pair_error_detail = detail;
    std::string cleanup_name;
    std::string cleanup_detail;
    call_no_arguments(connection, kAdapterPath, kAdapterInterface, "StopDiscovery", 5000, cleanup_name, cleanup_detail);
    const bool already_paired = pair_error_name == "org.bluez.Error.AlreadyExists" ||
                                pair_error_name == "org.bluez.Error.AlreadyPaired";
    if (!paired && !already_paired) {
        write_log(log, "Pair failed: " + pair_error_name + " " + pair_error_detail);
        dbus_connection_unregister_object_path(connection, kAgentPath);
        return false;
    }
    if (already_paired) {
        write_log(log, "device was already paired");
    } else {
        write_log(log, "pairing succeeded");
    }
    if (!call_set_trusted(connection, path, name, detail)) {
        write_log(log, "setting Trusted failed: " + name + " " + detail);
        dbus_connection_unregister_object_path(connection, kAgentPath);
        return false;
    }
    write_log(log, "device is trusted");
    dbus_connection_unregister_object_path(connection, kAgentPath);
    return true;
}

}  // namespace acp::iap2
