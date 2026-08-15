#include "acp/bridge/h264_normalizer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <memory>

namespace acp::bridge {

struct H264Normalizer::Context {
  AVBSFContext *filter{};
};

namespace {

std::string ffmpeg_error(const int result) {
  char text[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(result, text, sizeof(text));
  return text;
}

} // namespace

H264Normalizer::H264Normalizer() : context_(new Context) {
  const auto *filter = av_bsf_get_by_name("h264_metadata");
  if (filter == nullptr || av_bsf_alloc(filter, &context_->filter) < 0)
    return;
  context_->filter->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
  context_->filter->par_in->codec_id = AV_CODEC_ID_H264;
  context_->filter->time_base_in = AVRational{1, 30};
  if (av_opt_set_int(context_->filter->priv_data, "colour_primaries", 2, 0) <
          0 ||
      av_opt_set_int(context_->filter->priv_data, "transfer_characteristics", 2,
                     0) < 0 ||
      av_opt_set_int(context_->filter->priv_data, "matrix_coefficients", 2, 0) <
          0 ||
      av_bsf_init(context_->filter) < 0) {
    av_bsf_free(&context_->filter);
  }
}

H264Normalizer::~H264Normalizer() {
  if (context_ != nullptr) {
    av_bsf_free(&context_->filter);
    delete context_;
  }
}

std::vector<std::vector<std::uint8_t>>
H264Normalizer::normalize(const std::span<const std::uint8_t> access_unit,
                          std::string *error) {
  if (context_ == nullptr || context_->filter == nullptr) {
    if (error != nullptr)
      *error = "unable to initialize FFmpeg h264_metadata filter";
    return {};
  }
  AVPacket *packet = av_packet_alloc();
  if (packet == nullptr ||
      av_new_packet(packet, static_cast<int>(access_unit.size())) < 0) {
    av_packet_free(&packet);
    if (error != nullptr)
      *error = "unable to allocate H.264 normalization packet";
    return {};
  }
  std::copy(access_unit.begin(), access_unit.end(), packet->data);
  const auto send_result = av_bsf_send_packet(context_->filter, packet);
  av_packet_free(&packet);
  if (send_result < 0) {
    if (error != nullptr)
      *error = "unable to normalize Android Auto H.264: " +
               ffmpeg_error(send_result);
    return {};
  }
  std::vector<std::vector<std::uint8_t>> output;
  for (;;) {
    packet = av_packet_alloc();
    if (packet == nullptr) {
      if (error != nullptr)
        *error = "unable to allocate normalized H.264 output packet";
      return {};
    }
    const auto receive_result = av_bsf_receive_packet(context_->filter, packet);
    if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
      av_packet_free(&packet);
      break;
    }
    if (receive_result < 0) {
      av_packet_free(&packet);
      if (error != nullptr)
        *error = "unable to receive normalized Android Auto H.264: " +
                 ffmpeg_error(receive_result);
      return {};
    }
    output.emplace_back(packet->data, packet->data + packet->size);
    av_packet_free(&packet);
  }
  return output;
}

} // namespace acp::bridge
