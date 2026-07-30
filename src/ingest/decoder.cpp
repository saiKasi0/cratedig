#include "ingest/decoder.hpp"

#include "ingest/resampler.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ingest {
namespace {

// A file with more channels than this is not something a sampler should be
// quietly loading; it is far more likely a mislabelled stream.
constexpr int kMaxChannels = 32;

// RAII for FFmpeg's C handles. Every one of these has a bespoke two-step
// free function, and every early return below would otherwise have to remember
// all of them in the right order.
struct FormatContextDeleter {
  void operator()(AVFormatContext* context) const noexcept { avformat_close_input(&context); }
};

struct CodecContextDeleter {
  void operator()(AVCodecContext* context) const noexcept { avcodec_free_context(&context); }
};

struct PacketDeleter {
  void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};

struct FrameDeleter {
  void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};

struct SwrDeleter {
  void operator()(SwrContext* context) const noexcept { swr_free(&context); }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;

std::string av_error_string(int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
    return "unknown FFmpeg error " + std::to_string(code);
  }
  return std::string{buffer.data()};
}

SampleLoad failure(DecodeError error, std::string detail = {}) {
  return SampleLoad{.sample = nullptr, .error = error, .detail = std::move(detail)};
}

// Appends one decoded frame, converted to planar float, onto `channels`.
bool append_frame(SwrContext* swr, const AVFrame& frame, PlanarAudio& channels) {
  const auto channel_count = channels.size();
  const auto frame_samples = static_cast<std::size_t>(frame.nb_samples);
  if (frame_samples == 0) {
    return true;
  }

  // swr_convert writes into caller-provided planes. Growing each channel first
  // and pointing the planes at the new tail avoids a scratch buffer and a copy.
  const std::size_t offset = channels.front().size();
  std::vector<std::uint8_t*> planes(channel_count, nullptr);
  for (std::size_t channel = 0; channel < channel_count; ++channel) {
    channels[channel].resize(offset + frame_samples, 0.0F);
    planes[channel] = reinterpret_cast<std::uint8_t*>(channels[channel].data() + offset);
  }

  const int written =
      swr_convert(swr, planes.data(), frame.nb_samples,
                  const_cast<const std::uint8_t**>(frame.extended_data), frame.nb_samples);
  if (written < 0) {
    return false;
  }

  // The converter may produce fewer frames than it was offered; trim rather than
  // leaving the tail of zeroes we just resized in.
  const auto produced = static_cast<std::size_t>(written);
  if (produced != frame_samples) {
    for (std::vector<float>& channel : channels) {
      channel.resize(offset + produced);
    }
  }
  return true;
}

}  // namespace

std::string_view describe(DecodeError error) noexcept {
  switch (error) {
    case DecodeError::kNone:
      return "no error";
    case DecodeError::kFileNotFound:
      return "file not found";
    case DecodeError::kOpenFailed:
      return "could not open the file";
    case DecodeError::kNoAudioStream:
      return "the file contains no audio stream";
    case DecodeError::kNoDecoder:
      return "no decoder is available for this codec";
    case DecodeError::kDecoderOpenFailed:
      return "the decoder could not be opened";
    case DecodeError::kDecodeFailed:
      return "decoding failed";
    case DecodeError::kResamplerFailed:
      return "sample-rate conversion failed";
    case DecodeError::kEmptyStream:
      return "the audio stream contains no samples";
    case DecodeError::kUnsupportedChannelCount:
      return "unsupported channel count";
  }
  return "unknown error";
}

SampleLoad load_sample(const std::filesystem::path& path, std::uint32_t target_sample_rate) {
  std::error_code exists_error;
  if (!std::filesystem::exists(path, exists_error)) {
    // Checked before handing the path to FFmpeg purely so the common mistake
    // gets the obvious message instead of a demuxer's guess at what the bytes
    // were supposed to be.
    return failure(DecodeError::kFileNotFound, path.string());
  }

  AVFormatContext* raw_format = nullptr;
  int status = avformat_open_input(&raw_format, path.string().c_str(), nullptr, nullptr);
  if (status < 0) {
    return failure(DecodeError::kOpenFailed, av_error_string(status));
  }
  FormatContextPtr format{raw_format};

  status = avformat_find_stream_info(format.get(), nullptr);
  if (status < 0) {
    return failure(DecodeError::kNoAudioStream, av_error_string(status));
  }

  const AVCodec* codec = nullptr;
  const int stream_index = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (stream_index < 0) {
    return failure(DecodeError::kNoAudioStream, av_error_string(stream_index));
  }
  if (codec == nullptr) {
    return failure(DecodeError::kNoDecoder);
  }

  CodecContextPtr decoder{avcodec_alloc_context3(codec)};
  if (decoder == nullptr) {
    return failure(DecodeError::kDecoderOpenFailed, "out of memory");
  }

  const AVStream* stream = format->streams[stream_index];
  status = avcodec_parameters_to_context(decoder.get(), stream->codecpar);
  if (status < 0) {
    return failure(DecodeError::kDecoderOpenFailed, av_error_string(status));
  }
  status = avcodec_open2(decoder.get(), codec, nullptr);
  if (status < 0) {
    return failure(DecodeError::kDecoderOpenFailed, av_error_string(status));
  }

  const int channel_count = decoder->ch_layout.nb_channels;
  if (channel_count <= 0 || channel_count > kMaxChannels) {
    return failure(DecodeError::kUnsupportedChannelCount, std::to_string(channel_count));
  }
  const auto native_rate = static_cast<std::uint32_t>(decoder->sample_rate);
  if (native_rate == 0) {
    return failure(DecodeError::kDecodeFailed, "the stream declares a sample rate of zero");
  }

  // swresample converts FORMAT ONLY -- input rate in, same rate out. Rate
  // conversion is libsamplerate's job (see ingest/resampler.hpp for why), so
  // asking swr to do it here would mean resampling twice.
  SwrContext* raw_swr = nullptr;
  status = swr_alloc_set_opts2(&raw_swr, &decoder->ch_layout, AV_SAMPLE_FMT_FLTP,
                               decoder->sample_rate, &decoder->ch_layout, decoder->sample_fmt,
                               decoder->sample_rate, 0, nullptr);
  if (status < 0 || raw_swr == nullptr) {
    return failure(DecodeError::kDecodeFailed, av_error_string(status));
  }
  SwrPtr swr{raw_swr};
  status = swr_init(swr.get());
  if (status < 0) {
    return failure(DecodeError::kDecodeFailed, av_error_string(status));
  }

  PacketPtr packet{av_packet_alloc()};
  FramePtr frame{av_frame_alloc()};
  if (packet == nullptr || frame == nullptr) {
    return failure(DecodeError::kDecodeFailed, "out of memory");
  }

  PlanarAudio channels(static_cast<std::size_t>(channel_count));

  // Decode, then flush. The send/receive API buffers internally, so a decoder
  // that is never sent a null packet silently drops its tail -- which shows up
  // as a sample that is a few milliseconds short, not as an error.
  bool reading = true;
  while (reading) {
    status = av_read_frame(format.get(), packet.get());
    const bool end_of_file = (status == AVERROR_EOF);
    if (status < 0 && !end_of_file) {
      return failure(DecodeError::kDecodeFailed, av_error_string(status));
    }

    if (end_of_file) {
      reading = false;
      status = avcodec_send_packet(decoder.get(), nullptr);  // flush
    } else if (packet->stream_index != stream_index) {
      av_packet_unref(packet.get());
      continue;
    } else {
      status = avcodec_send_packet(decoder.get(), packet.get());
    }
    av_packet_unref(packet.get());

    if (status < 0 && status != AVERROR(EAGAIN) && status != AVERROR_EOF) {
      return failure(DecodeError::kDecodeFailed, av_error_string(status));
    }

    while (true) {
      status = avcodec_receive_frame(decoder.get(), frame.get());
      if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
        break;
      }
      if (status < 0) {
        return failure(DecodeError::kDecodeFailed, av_error_string(status));
      }
      if (!append_frame(swr.get(), *frame, channels)) {
        return failure(DecodeError::kDecodeFailed, "sample format conversion failed");
      }
      av_frame_unref(frame.get());
    }
  }

  if (channels.front().empty()) {
    return failure(DecodeError::kEmptyStream);
  }

  const std::uint32_t output_rate = target_sample_rate == 0 ? native_rate : target_sample_rate;
  ResampleResult resampled = resample(channels, native_rate, output_rate);
  if (!resampled.ok()) {
    return failure(DecodeError::kResamplerFailed,
                   "libsamplerate error " + std::to_string(resampled.library_error));
  }

  const std::size_t frames = resampled.channels.front().size();
  auto sample =
      std::make_shared<rt::Sample>(output_rate, static_cast<std::uint16_t>(channel_count), frames);
  for (std::size_t channel = 0; channel < resampled.channels.size(); ++channel) {
    const std::vector<float>& source = resampled.channels[channel];
    std::span<float> destination = sample->mutable_channel(static_cast<std::uint16_t>(channel));
    std::copy(source.begin(), source.end(), destination.begin());
  }

  return SampleLoad{.sample = std::move(sample), .error = DecodeError::kNone, .detail = {}};
}

std::string_view ffmpeg_license() noexcept {
  return avutil_license();
}

std::string_view ffmpeg_configuration() noexcept {
  return avutil_configuration();
}

std::string ffmpeg_versions() {
  const auto format = [](unsigned int version) {
    return std::to_string(AV_VERSION_MAJOR(version)) + "." +
           std::to_string(AV_VERSION_MINOR(version)) + "." +
           std::to_string(AV_VERSION_MICRO(version));
  };
  return "avformat " + format(avformat_version()) + ", avcodec " + format(avcodec_version()) +
         ", avutil " + format(avutil_version()) + ", swresample " + format(swresample_version());
}

}  // namespace ingest
