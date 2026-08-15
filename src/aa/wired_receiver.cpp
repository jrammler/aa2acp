#include "acp/aa/wired_receiver.hpp"

#include <aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <aasdk/Channel/MediaSink/Video/VideoMediaSinkService.hpp>
#include <aasdk/Error/Error.hpp>
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Messenger/MessageInStream.hpp>
#include <aasdk/Messenger/MessageOutStream.hpp>
#include <aasdk/Messenger/Messenger.hpp>
#include <aasdk/Transport/SSLWrapper.hpp>
#include <aasdk/Transport/USBTransport.hpp>
#include <aasdk/USB/AOAPDevice.hpp>
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

class ControlSession final
    : public aasdk::channel::control::IControlServiceChannelEventHandler,
      public aasdk::channel::mediasink::video::
          IVideoMediaSinkServiceEventHandler,
      public std::enable_shared_from_this<ControlSession> {
public:
  using Callback = WiredReceiver::EventCallback;

  ControlSession(boost::asio::io_service &io_service,
                 aasdk::usb::IUSBWrapper &usb_wrapper,
                 aasdk::usb::DeviceHandle handle, Callback callback)
      : io_service_(io_service), strand_(io_service), usb_wrapper_(usb_wrapper),
        handle_(std::move(handle)), callback_(std::move(callback)) {}

  void start() {
    try {
      auto device =
          aasdk::usb::AOAPDevice::create(usb_wrapper_, io_service_, handle_);
      transport_ = std::make_shared<aasdk::transport::USBTransport>(
          io_service_, std::move(device));
      auto ssl = std::make_shared<aasdk::transport::SSLWrapper>();
      cryptor_ = std::make_shared<aasdk::messenger::Cryptor>(std::move(ssl));
      cryptor_->init();
      messenger_ = std::make_shared<aasdk::messenger::Messenger>(
          io_service_,
          std::make_shared<aasdk::messenger::MessageInStream>(
              io_service_, transport_, cryptor_),
          std::make_shared<aasdk::messenger::MessageOutStream>(
              io_service_, transport_, cryptor_));
      control_ =
          std::make_shared<aasdk::channel::control::ControlServiceChannel>(
              strand_, messenger_);
      video_ = std::make_shared<
          aasdk::channel::mediasink::video::VideoMediaSinkService>(
          strand_, messenger_, aasdk::messenger::ChannelId::MEDIA_SINK_VIDEO);
      send_version_request();
      receive_next();
      receive_video_next();
    } catch (const aasdk::error::Error &error) {
      fail(error.what());
    }
  }

  void stop() {
    if (messenger_)
      messenger_->stop();
    if (transport_)
      transport_->stop();
    if (cryptor_)
      cryptor_->deinit();
  }

  void onVersionResponse(uint16_t, uint16_t,
                         aap_protobuf::shared::MessageStatus status) override {
    if (status != aap_protobuf::shared::STATUS_SUCCESS) {
      fail("Android Auto version negotiation was rejected");
      return;
    }
    try {
      cryptor_->doHandshake();
      send_handshake();
      receive_next();
    } catch (const aasdk::error::Error &error) {
      fail(error.what());
    }
  }

  void onHandshake(const aasdk::common::DataConstBuffer &payload) override {
    try {
      cryptor_->writeHandshakeBuffer(payload);
      if (cryptor_->doHandshake()) {
        aap_protobuf::service::control::message::AuthResponse response;
        response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
        send([this, response](aasdk::channel::SendPromise::Pointer promise) {
          control_->sendAuthComplete(response, std::move(promise));
        });
      } else {
        send_handshake();
      }
      receive_next();
    } catch (const aasdk::error::Error &error) {
      fail(error.what());
    }
  }

  void onServiceDiscoveryRequest(
      const aap_protobuf::service::control::message::ServiceDiscoveryRequest &)
      override {
    aap_protobuf::service::control::message::ServiceDiscoveryResponse response;
    response.mutable_channels()->Reserve(8);
    response.set_driver_position(
        aap_protobuf::service::control::message::DRIVER_POSITION_LEFT);
    response.set_display_name("ACP-AA Bridge");
    response.set_probe_for_support(false);
    auto *ping = response.mutable_connection_configuration()
                     ->mutable_ping_configuration();
    ping->set_timeout_ms(5000);
    ping->set_interval_ms(1500);
    ping->set_high_latency_threshold_ms(500);
    ping->set_tracked_ping_count(5);
    auto *head_unit = response.mutable_headunit_info();
    head_unit->set_make("ACP");
    head_unit->set_model("Android Auto to CarPlay Bridge");
    head_unit->set_year("2026");
    head_unit->set_vehicle_id("acp-aa-bridge");
    head_unit->set_head_unit_make("ACP");
    head_unit->set_head_unit_model("Pi Bridge");
    head_unit->set_head_unit_software_build("1");
    head_unit->set_head_unit_software_version("0.1");

    auto *video_service = response.add_channels();
    video_service->set_id(
        static_cast<int>(aasdk::messenger::ChannelId::MEDIA_SINK_VIDEO));
    auto *media_sink = video_service->mutable_media_sink_service();
    media_sink->set_available_type(aap_protobuf::service::media::shared::
                                       message::MEDIA_CODEC_VIDEO_H264_BP);
    media_sink->set_available_while_in_call(true);
    auto *video_config = media_sink->add_video_configs();
    video_config->set_codec_resolution(
        aap_protobuf::service::media::sink::message::VIDEO_1280x720);
    video_config->set_frame_rate(
        aap_protobuf::service::media::sink::message::VIDEO_FPS_30);
    video_config->set_density(180);

    const auto add_audio_service =
        [&response](
            aasdk::messenger::ChannelId channel,
            aap_protobuf::service::media::sink::message::AudioStreamType stream,
            uint32_t sample_rate, uint32_t channels) {
          auto *service = response.add_channels();
          service->set_id(static_cast<int>(channel));
          auto *media_sink = service->mutable_media_sink_service();
          media_sink->set_available_type(aap_protobuf::service::media::shared::
                                             message::MEDIA_CODEC_AUDIO_PCM);
          media_sink->set_audio_type(stream);
          media_sink->set_available_while_in_call(true);
          auto *audio_config = media_sink->add_audio_configs();
          audio_config->set_sampling_rate(sample_rate);
          audio_config->set_number_of_bits(16);
          audio_config->set_number_of_channels(channels);
        };
    add_audio_service(
        aasdk::messenger::ChannelId::MEDIA_SINK_MEDIA_AUDIO,
        aap_protobuf::service::media::sink::message::AUDIO_STREAM_MEDIA, 48000,
        2);
    add_audio_service(
        aasdk::messenger::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO,
        aap_protobuf::service::media::sink::message::AUDIO_STREAM_GUIDANCE,
        16000, 1);
    add_audio_service(
        aasdk::messenger::ChannelId::MEDIA_SINK_SYSTEM_AUDIO,
        aap_protobuf::service::media::sink::message::AUDIO_STREAM_SYSTEM_AUDIO,
        16000, 1);

    auto *microphone_service = response.add_channels();
    microphone_service->set_id(
        static_cast<int>(aasdk::messenger::ChannelId::MEDIA_SOURCE_MICROPHONE));
    auto *microphone = microphone_service->mutable_media_source_service();
    microphone->set_available_type(
        aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
    auto *microphone_config = microphone->mutable_audio_config();
    microphone_config->set_sampling_rate(16000);
    microphone_config->set_number_of_bits(16);
    microphone_config->set_number_of_channels(1);

    auto *sensor_service = response.add_channels();
    sensor_service->set_id(
        static_cast<int>(aasdk::messenger::ChannelId::SENSOR));
    sensor_service->mutable_sensor_source_service()
        ->add_sensors()
        ->set_sensor_type(aap_protobuf::service::sensorsource::message::
                              SENSOR_DRIVING_STATUS_DATA);

    auto *input_service = response.add_channels();
    input_service->set_id(
        static_cast<int>(aasdk::messenger::ChannelId::INPUT_SOURCE));
    auto *touchscreen =
        input_service->mutable_input_source_service()->add_touchscreen();
    touchscreen->set_width(1280);
    touchscreen->set_height(720);
    touchscreen->set_type(
        aap_protobuf::service::inputsource::message::CAPACITIVE);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        [self = shared_from_this(), count = response.channels_size()] {
          self->callback_({WiredReceiverEventType::control_session_ready,
                           "sent Android Auto service discovery (" +
                               std::to_string(count) + " channels)"});
        },
        [self = shared_from_this()](const auto &error) {
          self->fail(error.what());
        });
    control_->sendServiceDiscoveryResponse(response, std::move(promise));
    receive_next();
  }

  void onAudioFocusRequest(
      const aap_protobuf::service::control::message::AudioFocusRequest &request)
      override {
    aap_protobuf::service::control::message::AudioFocusNotification response;
    response.set_focus_state(
        request.audio_focus_type() ==
                aap_protobuf::service::control::message::AUDIO_FOCUS_RELEASE
            ? aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_LOSS
            : aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_GAIN);
    response.set_unsolicited(false);
    send([this, response](aasdk::channel::SendPromise::Pointer promise) {
      control_->sendAudioFocusResponse(response, std::move(promise));
    });
    receive_next();
  }
  void onByeByeRequest(
      const aap_protobuf::service::control::message::ByeByeRequest &) override {
    callback_({WiredReceiverEventType::disconnected,
               "phone ended the Android Auto control session"});
    stop();
  }
  void onByeByeResponse(
      const aap_protobuf::service::control::message::ByeByeResponse &)
      override {
    callback_({WiredReceiverEventType::disconnected,
               "phone acknowledged Android Auto control-session shutdown"});
    stop();
  }
  void onBatteryStatusNotification(
      const aap_protobuf::service::control::message::BatteryStatusNotification
          &) override {
    receive_next();
  }
  void onNavigationFocusRequest(
      const aap_protobuf::service::control::message::NavFocusRequestNotification
          &) override {
    receive_next();
  }
  void onVoiceSessionRequest(
      const aap_protobuf::service::control::message::VoiceSessionNotification &)
      override {
    receive_next();
  }
  void onPingRequest(
      const aap_protobuf::service::control::message::PingRequest &) override {
    receive_next();
  }
  void onPingResponse(
      const aap_protobuf::service::control::message::PingResponse &) override {
    receive_next();
  }
  void onChannelError(const aasdk::error::Error &error) override {
    fail(error.what());
  }

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest &)
      override {
    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    send_video([this, response](aasdk::channel::SendPromise::Pointer promise) {
      video_->sendChannelOpenResponse(response, std::move(promise));
    });
    receive_video_next();
  }

  void onMediaChannelSetupRequest(
      const aap_protobuf::service::media::shared::message::Setup &request)
      override {
    if (request.type() != aap_protobuf::service::media::shared::message::
                              MEDIA_CODEC_VIDEO_H264_BP) {
      fail("phone requested an unsupported Android Auto video codec");
      return;
    }
    aap_protobuf::service::media::shared::message::Config response;
    response.set_status(
        aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    response.set_max_unacked(1);
    response.add_configuration_indices(0);
    send_video([this, response](aasdk::channel::SendPromise::Pointer promise) {
      video_->sendChannelSetupResponse(response, std::move(promise));
    });
    receive_video_next();
  }

  void onMediaChannelStartIndication(
      const aap_protobuf::service::media::shared::message::Start &indication)
      override {
    video_session_id_ = indication.session_id();
    receive_video_next();
  }

  void onMediaChannelStopIndication(
      const aap_protobuf::service::media::shared::message::Stop &) override {
    video_session_id_ = 0;
    receive_video_next();
  }

  void onMediaWithTimestampIndication(
      aasdk::messenger::Timestamp::ValueType,
      const aasdk::common::DataConstBuffer &) override {
    acknowledge_video_frame();
  }

  void onMediaIndication(const aasdk::common::DataConstBuffer &) override {
    acknowledge_video_frame();
  }

  void onVideoFocusRequest(const aap_protobuf::service::media::video::message::
                               VideoFocusRequestNotification &) override {
    aap_protobuf::service::media::video::message::VideoFocusNotification
        response;
    response.set_focus(
        aap_protobuf::service::media::video::message::VIDEO_FOCUS_PROJECTED);
    response.set_unsolicited(false);
    send_video([this, response](aasdk::channel::SendPromise::Pointer promise) {
      video_->sendVideoFocusIndication(response, std::move(promise));
    });
    receive_video_next();
  }

private:
  void send_version_request() {
    send([this](auto promise) {
      control_->sendVersionRequest(std::move(promise));
    });
  }
  void send_handshake() {
    send([this](auto promise) {
      control_->sendHandshake(cryptor_->readHandshakeBuffer(),
                              std::move(promise));
    });
  }
  template <typename Sender> void send(Sender sender) {
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {}, [self = shared_from_this()](
                             const auto &error) { self->fail(error.what()); });
    sender(std::move(promise));
  }
  template <typename Sender> void send_video(Sender sender) {
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {}, [self = shared_from_this()](
                             const auto &error) { self->fail(error.what()); });
    sender(std::move(promise));
  }
  void receive_next() { control_->receive(shared_from_this()); }
  void receive_video_next() { video_->receive(shared_from_this()); }
  void acknowledge_video_frame() {
    aap_protobuf::service::media::source::message::Ack acknowledgement;
    acknowledgement.set_session_id(video_session_id_);
    acknowledgement.set_ack(1);
    send_video(
        [this, acknowledgement](aasdk::channel::SendPromise::Pointer promise) {
          video_->sendMediaAckIndication(acknowledgement, std::move(promise));
        });
    receive_video_next();
  }
  void fail(const std::string &detail) {
    callback_({WiredReceiverEventType::error,
               "Android Auto control session: " + detail});
  }

  boost::asio::io_service &io_service_;
  boost::asio::io_service::strand strand_;
  aasdk::usb::IUSBWrapper &usb_wrapper_;
  aasdk::usb::DeviceHandle handle_;
  Callback callback_;
  aasdk::transport::ITransport::Pointer transport_;
  aasdk::messenger::ICryptor::Pointer cryptor_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  aasdk::channel::control::IControlServiceChannel::Pointer control_;
  aasdk::channel::mediasink::video::IVideoMediaSinkService::Pointer video_;
  int32_t video_session_id_ = 0;
};

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
          start_control_session();
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
      self->control_session_.reset();
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
        });
  }

  void emit(WiredReceiverEvent event) const {
    if (callback_)
      callback_(event);
  }

  void start_control_session() {
    control_session_ = std::make_shared<ControlSession>(
        io_service_, *usb_wrapper_, active_handle_, callback_);
    control_session_->start();
  }

  void reset_locked() {
    if (control_session_)
      control_session_->stop();
    control_session_.reset();
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
  std::shared_ptr<ControlSession> control_session_;
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
