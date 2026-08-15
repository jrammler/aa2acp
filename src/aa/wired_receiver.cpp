#include "acp/aa/wired_receiver.hpp"

#include <aasdk/Error/Error.hpp>
#include <aasdk/USB/AccessoryModeQueryChainFactory.hpp>
#include <aasdk/USB/AccessoryModeQueryFactory.hpp>
#include <aasdk/USB/IUSBHub.hpp>
#include <aasdk/USB/USBHub.hpp>
#include <aasdk/USB/USBWrapper.hpp>

#include <boost/asio/io_service.hpp>

#include <libusb.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace acp::aa {

class WiredReceiver::Impl {
public:
  explicit Impl(EventCallback callback) : callback_(std::move(callback)) {}

  ~Impl() { stop(); }

  bool start(std::string *error) {
    std::lock_guard lock(mutex_);
    if (running_)
      return true;
    if (libusb_init(&usb_context_) != LIBUSB_SUCCESS) {
      if (error != nullptr)
        *error = "unable to initialize libusb";
      return false;
    }
    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
      if (error != nullptr)
        *error = "libusb hotplug support is unavailable";
      libusb_exit(usb_context_);
      usb_context_ = nullptr;
      return false;
    }

    usb_wrapper_ = std::make_unique<aasdk::usb::USBWrapper>(usb_context_);
    query_factory_ = std::make_unique<aasdk::usb::AccessoryModeQueryFactory>(
        *usb_wrapper_, io_service_);
    chain_factory_ =
        std::make_unique<aasdk::usb::AccessoryModeQueryChainFactory>(
            *usb_wrapper_, io_service_, *query_factory_);
    usb_hub_ = std::make_shared<aasdk::usb::USBHub>(*usb_wrapper_, io_service_,
                                                    *chain_factory_);
    const auto result = libusb_hotplug_register_callback(
        usb_context_,
        static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                          LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
        LIBUSB_HOTPLUG_NO_FLAGS, LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, &Impl::device_event,
        this, &device_callback_);
    if (result != LIBUSB_SUCCESS) {
      if (error != nullptr)
        *error = "unable to register USB disconnect callback";
      reset_locked();
      return false;
    }

    work_guard_ = std::make_unique<boost::asio::io_service::work>(io_service_);
    stopping_ = false;
    running_ = true;
    io_thread_ = std::thread([this] { io_service_.run(); });
    usb_thread_ = std::thread([this] { run_usb_events(); });
    arm_wait_for_phone();
    find_existing_aoap_device();
    emit({WiredReceiverEventType::waiting_for_phone,
          "waiting for a wired Android Auto phone"});
    return true;
  }

  void stop() {
    {
      std::lock_guard lock(mutex_);
      if (!running_)
        return;
      stopping_ = true;
      usb_hub_->cancel();
      work_guard_.reset();
      io_service_.stop();
      if (device_callback_ != LIBUSB_HOTPLUG_MATCH_ANY)
        libusb_hotplug_deregister_callback(usb_context_, device_callback_);
      device_callback_ = LIBUSB_HOTPLUG_MATCH_ANY;
    }
    if (usb_thread_.joinable())
      usb_thread_.join();
    if (io_thread_.joinable())
      io_thread_.join();
    std::lock_guard lock(mutex_);
    reset_locked();
  }

private:
  void arm_wait_for_phone() {
    auto promise = aasdk::usb::IUSBHub::Promise::defer(io_service_);
    promise->then(
        [this](aasdk::usb::DeviceHandle handle) {
          if (stopping_)
            return;
          auto *device = libusb_get_device(handle.get());
          active_bus_ = libusb_get_bus_number(device);
          active_address_ = libusb_get_device_address(device);
          active_handle_ = std::move(handle);
          active_ = true;
          emit({WiredReceiverEventType::aoap_transport_ready,
                "AOAP transport ready on USB " + std::to_string(active_bus_) +
                    ":" + std::to_string(active_address_)});
        },
        [this](const aasdk::error::Error &error) {
          if (!stopping_)
            emit({WiredReceiverEventType::error,
                  "Android Auto USB discovery failed: " +
                      std::string(error.what())});
        });
    usb_hub_->start(std::move(promise));
  }

  void run_usb_events() {
    while (!stopping_) {
      timeval timeout{0, 100 * 1000};
      libusb_handle_events_timeout_completed(usb_context_, &timeout, nullptr);
    }
  }

  static int device_event(libusb_context *, libusb_device *device,
                          libusb_hotplug_event event, void *user_data) {
    auto *self = static_cast<Impl *>(user_data);
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
      self->handle_device_arrival(device);
      return 0;
    }
    if (event != LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT || !self->active_ ||
        self->stopping_ || libusb_get_bus_number(device) != self->active_bus_ ||
        libusb_get_device_address(device) != self->active_address_)
      return 0;
    self->io_service_.post([self] {
      self->active_ = false;
      self->active_handle_.reset();
      self->emit({WiredReceiverEventType::disconnected,
                  "wired Android Auto phone disconnected"});
      self->arm_wait_for_phone();
      self->emit({WiredReceiverEventType::waiting_for_phone,
                  "waiting for a wired Android Auto phone"});
    });
    return 0;
  }

  void find_existing_aoap_device() {
    libusb_device **devices{};
    const auto count = libusb_get_device_list(usb_context_, &devices);
    if (count < 0)
      return;
    for (ssize_t index = 0; index < count; ++index)
      handle_device_arrival(devices[index]);
    libusb_free_device_list(devices, 1);
  }

  void handle_device_arrival(libusb_device *device) {
    if (stopping_ || active_)
      return;
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS ||
        descriptor.idVendor != 0x18D1 ||
        (descriptor.idProduct != 0x2D00 && descriptor.idProduct != 0x2D01))
      return;
    libusb_device_handle *raw_handle{};
    if (libusb_open(device, &raw_handle) != LIBUSB_SUCCESS ||
        raw_handle == nullptr) {
      emit({WiredReceiverEventType::error,
            "unable to open the Android Auto AOAP USB device"});
      return;
    }
    const auto bus = libusb_get_bus_number(device);
    const auto address = libusb_get_device_address(device);
    aasdk::usb::DeviceHandle handle(raw_handle, &libusb_close);
    io_service_.post(
        [this, handle = std::move(handle), bus, address]() mutable {
          if (stopping_ || active_)
            return;
          active_bus_ = bus;
          active_address_ = address;
          active_handle_ = std::move(handle);
          active_ = true;
          emit({WiredReceiverEventType::aoap_transport_ready,
                "AOAP transport ready on USB " + std::to_string(bus) + ":" +
                    std::to_string(address)});
        });
  }

  void emit(WiredReceiverEvent event) const {
    if (callback_)
      callback_(event);
  }

  void reset_locked() {
    active_handle_.reset();
    active_ = false;
    usb_hub_.reset();
    chain_factory_.reset();
    query_factory_.reset();
    usb_wrapper_.reset();
    if (usb_context_ != nullptr)
      libusb_exit(usb_context_);
    usb_context_ = nullptr;
    io_service_.reset();
    running_ = false;
  }

  EventCallback callback_;
  std::mutex mutex_;
  std::atomic_bool stopping_{false};
  bool running_{};
  boost::asio::io_service io_service_;
  std::unique_ptr<boost::asio::io_service::work> work_guard_;
  libusb_context *usb_context_{};
  libusb_hotplug_callback_handle device_callback_{LIBUSB_HOTPLUG_MATCH_ANY};
  std::unique_ptr<aasdk::usb::USBWrapper> usb_wrapper_;
  std::unique_ptr<aasdk::usb::AccessoryModeQueryFactory> query_factory_;
  std::unique_ptr<aasdk::usb::AccessoryModeQueryChainFactory> chain_factory_;
  std::shared_ptr<aasdk::usb::USBHub> usb_hub_;
  aasdk::usb::DeviceHandle active_handle_;
  std::atomic_bool active_{false};
  std::atomic_uint8_t active_bus_{};
  std::atomic_uint8_t active_address_{};
  std::thread io_thread_;
  std::thread usb_thread_;
};

WiredReceiver::WiredReceiver(EventCallback callback)
    : impl_(std::make_unique<Impl>(std::move(callback))) {}

WiredReceiver::~WiredReceiver() = default;

bool WiredReceiver::start(std::string *error) { return impl_->start(error); }

void WiredReceiver::stop() { impl_->stop(); }

} // namespace acp::aa
