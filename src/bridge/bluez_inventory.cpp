#include "aa2acp/bridge/bluez_inventory.hpp"

#include <dbus/dbus.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace aa2acp::bridge {
namespace {

constexpr char kBluez[] = "org.bluez";
constexpr char kAdapterPath[] = "/org/bluez/hci0";
constexpr char kAdapterInterface[] = "org.bluez.Adapter1";
constexpr char kDeviceInterface[] = "org.bluez.Device1";
constexpr char kObjectManager[] = "org.freedesktop.DBus.ObjectManager";

class Connection {
public:
  Connection() {
    DBusError error;
    dbus_error_init(&error);
    connection_ = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    if (connection_ == nullptr && error.message != nullptr)
      error_ = error.message;
    dbus_error_free(&error);
  }
  ~Connection() {
    if (connection_ != nullptr) {
      dbus_connection_close(connection_);
      dbus_connection_unref(connection_);
    }
  }
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;
  DBusConnection *get() const { return connection_; }
  const std::string &error() const { return error_; }

private:
  DBusConnection *connection_{};
  std::string error_;
};

DBusMessage *call(DBusConnection *connection, const char *path,
                  const char *interface, const char *method, int timeout_ms,
                  std::string *error) {
  DBusMessage *request =
      dbus_message_new_method_call(kBluez, path, interface, method);
  if (request == nullptr) {
    if (error != nullptr)
      *error = "unable to allocate D-Bus request";
    return nullptr;
  }
  DBusError dbus_error;
  dbus_error_init(&dbus_error);
  DBusMessage *reply = dbus_connection_send_with_reply_and_block(
      connection, request, timeout_ms, &dbus_error);
  dbus_message_unref(request);
  if (reply == nullptr && error != nullptr)
    *error = dbus_error.message == nullptr ? "D-Bus call failed"
                                           : dbus_error.message;
  dbus_error_free(&dbus_error);
  return reply;
}

std::string property_string(DBusMessageIter *properties, const char *wanted) {
  DBusMessageIter property;
  while (dbus_message_iter_get_arg_type(properties) == DBUS_TYPE_DICT_ENTRY) {
    dbus_message_iter_recurse(properties, &property);
    const char *name = nullptr;
    dbus_message_iter_get_basic(&property, &name);
    dbus_message_iter_next(&property);
    DBusMessageIter value;
    dbus_message_iter_recurse(&property, &value);
    if (name != nullptr && std::string_view(name) == wanted &&
        dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
      const char *text = nullptr;
      dbus_message_iter_get_basic(&value, &text);
      return text == nullptr ? "" : text;
    }
    dbus_message_iter_next(properties);
  }
  return {};
}

bool property_bool(DBusMessageIter *properties, const char *wanted) {
  DBusMessageIter property;
  while (dbus_message_iter_get_arg_type(properties) == DBUS_TYPE_DICT_ENTRY) {
    dbus_message_iter_recurse(properties, &property);
    const char *name = nullptr;
    dbus_message_iter_get_basic(&property, &name);
    dbus_message_iter_next(&property);
    DBusMessageIter value;
    dbus_message_iter_recurse(&property, &value);
    if (name != nullptr && std::string_view(name) == wanted &&
        dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_BOOLEAN) {
      dbus_bool_t result = false;
      dbus_message_iter_get_basic(&value, &result);
      return result;
    }
    dbus_message_iter_next(properties);
  }
  return false;
}

bool set_filter(DBusConnection *connection, const std::string &transport,
                std::string *error) {
  DBusMessage *request = dbus_message_new_method_call(
      kBluez, kAdapterPath, kAdapterInterface, "SetDiscoveryFilter");
  if (request == nullptr)
    return false;
  DBusMessageIter arguments, dictionary, entry, variant;
  const char *key = "Transport";
  const char *value = transport.c_str();
  dbus_message_iter_init_append(request, &arguments);
  dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY, "{sv}",
                                   &dictionary);
  dbus_message_iter_open_container(&dictionary, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &entry);
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                   DBUS_TYPE_STRING_AS_STRING, &variant);
  dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&dictionary, &entry);
  dbus_message_iter_close_container(&arguments, &dictionary);
  DBusError dbus_error;
  dbus_error_init(&dbus_error);
  DBusMessage *reply = dbus_connection_send_with_reply_and_block(
      connection, request, 5000, &dbus_error);
  dbus_message_unref(request);
  if (reply != nullptr)
    dbus_message_unref(reply);
  if (reply == nullptr && error != nullptr)
    *error = dbus_error.message == nullptr ? "unable to set discovery filter"
                                           : dbus_error.message;
  dbus_error_free(&dbus_error);
  return reply != nullptr;
}

void log_message(const std::function<void(const std::string &)> &log,
                 const std::string &message) {
  if (log)
    log(message);
}

} // namespace

std::vector<BluetoothDevice> list_bluez_devices(std::string *error) {
  Connection connection;
  if (connection.get() == nullptr) {
    if (error != nullptr)
      *error = connection.error();
    return {};
  }
  DBusMessage *reply = call(connection.get(), "/", kObjectManager,
                            "GetManagedObjects", 5000, error);
  if (reply == nullptr)
    return {};
  std::vector<BluetoothDevice> devices;
  DBusMessageIter root, objects;
  if (!dbus_message_iter_init(reply, &root) ||
      dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
    dbus_message_unref(reply);
    if (error != nullptr)
      *error = "BlueZ returned an invalid ObjectManager response";
    return {};
  }
  dbus_message_iter_recurse(&root, &objects);
  while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter object, interfaces;
    dbus_message_iter_recurse(&objects, &object);
    dbus_message_iter_next(&object);
    dbus_message_iter_recurse(&object, &interfaces);
    while (dbus_message_iter_get_arg_type(&interfaces) ==
           DBUS_TYPE_DICT_ENTRY) {
      DBusMessageIter interface, properties;
      dbus_message_iter_recurse(&interfaces, &interface);
      const char *interface_name = nullptr;
      dbus_message_iter_get_basic(&interface, &interface_name);
      dbus_message_iter_next(&interface);
      if (interface_name != nullptr &&
          std::string_view(interface_name) == kDeviceInterface) {
        dbus_message_iter_recurse(&interface, &properties);
        BluetoothDevice device;
        device.address = property_string(&properties, "Address");
        device.name = property_string(&properties, "Alias");
        if (device.name.empty())
          device.name = property_string(&properties, "Name");
        device.paired = property_bool(&properties, "Paired");
        device.connected = property_bool(&properties, "Connected");
        if (!device.address.empty())
          devices.push_back(std::move(device));
      }
      dbus_message_iter_next(&interfaces);
    }
    dbus_message_iter_next(&objects);
  }
  dbus_message_unref(reply);
  std::sort(devices.begin(), devices.end(),
            [](const auto &left, const auto &right) {
              if (left.paired != right.paired)
                return left.paired > right.paired;
              return left.address < right.address;
            });
  return devices;
}

bool bluez_device_is_paired(const std::string_view address) {
  const auto devices = list_bluez_devices();
  return std::any_of(devices.begin(), devices.end(),
                     [address](const BluetoothDevice &device) {
                       return device.address == address && device.paired;
                     });
}

bool discover_bluez_devices(
    const std::string &transport, int seconds,
    const std::function<void(const std::string &)> &log) {
  Connection connection;
  if (connection.get() == nullptr) {
    log_message(log, "cannot connect to system D-Bus: " + connection.error());
    return false;
  }
  std::string error;
  if (!set_filter(connection.get(), transport, &error)) {
    log_message(log, "unable to select " + transport + " discovery: " + error);
    return false;
  }
  DBusMessage *reply = call(connection.get(), kAdapterPath, kAdapterInterface,
                            "StartDiscovery", 5000, &error);
  if (reply == nullptr) {
    log_message(log, "unable to start " + transport + " discovery: " + error);
    return false;
  }
  dbus_message_unref(reply);
  log_message(log, "scanning " + transport + " devices for " +
                       std::to_string(seconds) + " seconds");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline)
    dbus_connection_read_write_dispatch(connection.get(), 250);
  reply = call(connection.get(), kAdapterPath, kAdapterInterface,
               "StopDiscovery", 5000, &error);
  if (reply == nullptr) {
    log_message(log, "unable to stop discovery: " + error);
    return false;
  }
  dbus_message_unref(reply);
  return true;
}

} // namespace aa2acp::bridge
