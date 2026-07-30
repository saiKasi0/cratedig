#include "ingest/decoder.hpp"
#include "ingest/resampler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace {

// A complete 16-bit PCM WAV, assembled byte by byte in the test.
//
// This is the test that actually proves the decoder correct rather than merely
// non-crashing: every expected float is known exactly in advance, because every
// input byte was chosen here. It needs no fixture, no network and no ffmpeg CLI,
// so unlike the starter-pack tests below it can never skip.
//
// 8 frames, stereo, 44.1 kHz. Left channel counts up, right counts down, so a
// channel swap or an interleaving mistake is immediately visible rather than
// plausible.
constexpr std::array<std::int16_t, 8> kLeft{0,      4'096,  8'192,   16'384,
                                            -4'096, -8'192, -16'384, 32'767};
constexpr std::array<std::int16_t, 8> kRight{-32'768, -16'384, -8'192, -4'096,
                                             4'096,   8'192,   16'384, 0};
constexpr std::uint32_t kWavRate = 44'100;

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void put_tag(std::vector<std::uint8_t>& out, const char* tag) {
  for (std::size_t i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>(tag[i]));
  }
}

std::vector<std::uint8_t> build_wav() {
  constexpr std::uint16_t kChannels = 2;
  constexpr std::uint16_t kBitsPerSample = 16;
  const auto frames = static_cast<std::uint32_t>(kLeft.size());
  const std::uint32_t data_bytes = frames * kChannels * (kBitsPerSample / 8U);

  std::vector<std::uint8_t> wav;
  put_tag(wav, "RIFF");
  put_u32(wav, 36U + data_bytes);  // everything after this field
  put_tag(wav, "WAVE");

  put_tag(wav, "fmt ");
  put_u32(wav, 16);  // PCM fmt chunk size
  put_u16(wav, 1);   // WAVE_FORMAT_PCM
  put_u16(wav, kChannels);
  put_u32(wav, kWavRate);
  put_u32(wav, kWavRate * kChannels * (kBitsPerSample / 8U));  // byte rate
  put_u16(wav, kChannels * (kBitsPerSample / 8U));             // block align
  put_u16(wav, kBitsPerSample);

  put_tag(wav, "data");
  put_u32(wav, data_bytes);
  for (std::size_t frame = 0; frame < kLeft.size(); ++frame) {
    put_u16(wav, static_cast<std::uint16_t>(kLeft[frame]));
    put_u16(wav, static_cast<std::uint16_t>(kRight[frame]));
  }
  return wav;
}

// Writes the bytes to a uniquely named temp file and removes it on scope exit.
class TempFile {
 public:
  TempFile(const std::string& name, const std::vector<std::uint8_t>& bytes)
      : m_path(std::filesystem::temp_directory_path() / name) {
    std::ofstream out{m_path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
  }

  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(m_path, ignored);
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&&) = delete;
  TempFile& operator=(TempFile&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

 private:
  std::filesystem::path m_path;
};

// FFmpeg's pcm_s16le decoder divides by 32768, so every expected value is exact
// in binary and can be compared without a tolerance.
constexpr float to_float(std::int16_t value) {
  return static_cast<float>(value) / 32'768.0F;
}

}  // namespace

TEST_CASE("Decoder reads a 16-bit PCM WAV exactly", "[unit]") {
  const TempFile file{"cratedig_decode_exact.wav", build_wav()};

  // target_sample_rate == 0 keeps the native rate, so nothing but the decoder is
  // under test here.
  const ingest::SampleLoad load = ingest::load_sample(file.path(), 0);
  INFO("decode error: " << ingest::describe(load.error) << " (" << load.detail << ")");
  REQUIRE(load.ok());

  const rt::Sample& sample = *load.sample;
  CHECK(sample.sample_rate() == kWavRate);
  CHECK(sample.num_channels() == 2);
  REQUIRE(sample.num_frames() == kLeft.size());

  for (std::size_t frame = 0; frame < kLeft.size(); ++frame) {
    CHECK(sample.channel(0)[frame] == to_float(kLeft[frame]));
    CHECK(sample.channel(1)[frame] == to_float(kRight[frame]));
  }
}

TEST_CASE("Decoder leaves a sample already at the engine rate bit-exact", "[unit]") {
  // Asking for exactly the file's own rate must not run it through the
  // converter. If it did, a sinc filter at unity ratio would alter every sample
  // and the engine's "loaded at 48k, plays untouched" property would be a
  // fiction.
  const TempFile file{"cratedig_decode_passthrough.wav", build_wav()};

  const ingest::SampleLoad native = ingest::load_sample(file.path(), 0);
  const ingest::SampleLoad requested = ingest::load_sample(file.path(), kWavRate);
  REQUIRE(native.ok());
  REQUIRE(requested.ok());

  REQUIRE(requested.sample->num_frames() == native.sample->num_frames());
  for (std::uint16_t channel = 0; channel < 2; ++channel) {
    for (std::size_t frame = 0; frame < native.sample->num_frames(); ++frame) {
      CHECK(requested.sample->channel(channel)[frame] == native.sample->channel(channel)[frame]);
    }
  }
}

TEST_CASE("Decoder resamples to the engine rate", "[unit]") {
  const TempFile file{"cratedig_decode_resample.wav", build_wav()};

  const ingest::SampleLoad load = ingest::load_sample(file.path(), 48'000);
  INFO("decode error: " << ingest::describe(load.error) << " (" << load.detail << ")");
  REQUIRE(load.ok());

  CHECK(load.sample->sample_rate() == 48'000);
  CHECK(load.sample->num_channels() == 2);

  // 8 frames at 44.1 kHz is ~8.7 frames at 48 kHz. The converter decides the
  // exact count, so this asserts the ratio was applied, not a specific policy
  // about rounding.
  CHECK(load.sample->num_frames() >= 8);
  CHECK(load.sample->num_frames() <= 12);
}

TEST_CASE("Decoder reports why a load failed", "[unit]") {
  SECTION("missing file") {
    const ingest::SampleLoad load =
        ingest::load_sample(std::filesystem::temp_directory_path() / "cratedig_nope.wav", 48'000);
    CHECK_FALSE(load.ok());
    CHECK(load.error == ingest::DecodeError::kFileNotFound);
    CHECK(load.sample == nullptr);
  }

  SECTION("not audio at all") {
    const std::vector<std::uint8_t> junk(512, 0xA5U);
    const TempFile file{"cratedig_decode_junk.wav", junk};

    const ingest::SampleLoad load = ingest::load_sample(file.path(), 48'000);
    CHECK_FALSE(load.ok());
    CHECK(load.sample == nullptr);
    // FFmpeg's own message is carried through, because "Invalid data found when
    // processing input" is what a user needs to see, not an enum name.
    CHECK_FALSE(load.detail.empty());
  }

  SECTION("truncated header") {
    std::vector<std::uint8_t> wav = build_wav();
    wav.resize(8);
    const TempFile file{"cratedig_decode_truncated.wav", wav};

    const ingest::SampleLoad load = ingest::load_sample(file.path(), 48'000);
    CHECK_FALSE(load.ok());
  }
}

TEST_CASE("Decoder describes every error code", "[unit]") {
  // A switch that silently falls through to "unknown error" for a newly added
  // code would show up here rather than in front of a user.
  const ingest::DecodeError codes[] = {
      ingest::DecodeError::kNone,         ingest::DecodeError::kFileNotFound,
      ingest::DecodeError::kOpenFailed,   ingest::DecodeError::kNoAudioStream,
      ingest::DecodeError::kNoDecoder,    ingest::DecodeError::kDecoderOpenFailed,
      ingest::DecodeError::kDecodeFailed, ingest::DecodeError::kResamplerFailed,
      ingest::DecodeError::kEmptyStream,  ingest::DecodeError::kUnsupportedChannelCount,
  };
  for (const ingest::DecodeError code : codes) {
    CHECK_FALSE(ingest::describe(code).empty());
    CHECK(ingest::describe(code) != "unknown error");
  }
}

TEST_CASE("FFmpeg build details are reportable", "[unit]") {
  // docs/LICENSING.md requires the linked build's license to be observable at
  // runtime rather than assumed. `cratedig --version` prints these.
  CHECK_FALSE(ingest::ffmpeg_license().empty());
  CHECK_FALSE(ingest::ffmpeg_configuration().empty());
  CHECK_FALSE(ingest::ffmpeg_versions().empty());
}

TEST_CASE("Resampler returns the input unchanged at a unity ratio", "[unit]") {
  ingest::PlanarAudio input{{0.25F, -0.5F, 0.75F, -1.0F}, {1.0F, 0.5F, -0.25F, 0.125F}};

  const ingest::ResampleResult result = ingest::resample(input, 48'000, 48'000);
  REQUIRE(result.ok());
  CHECK(result.channels == input);
}

TEST_CASE("Resampler changes length by the rate ratio", "[unit]") {
  ingest::PlanarAudio input(1, std::vector<float>(44'100, 0.0F));
  for (std::size_t frame = 0; frame < input[0].size(); ++frame) {
    input[0][frame] = static_cast<float>(frame % 100) / 100.0F;
  }

  const ingest::ResampleResult result = ingest::resample(input, 44'100, 48'000);
  REQUIRE(result.ok());
  REQUIRE(result.channels.size() == 1);

  // One second in, one second out, within the converter's transient.
  CHECK(result.channels[0].size() > 47'900);
  CHECK(result.channels[0].size() < 48'100);
}

TEST_CASE("Resampler rejects nonsense rather than dividing by zero", "[unit]") {
  const ingest::PlanarAudio input{{0.1F, 0.2F}};

  CHECK(ingest::resample(input, 0, 48'000).status == ingest::ResampleStatus::kBadRate);
  CHECK(ingest::resample(input, 48'000, 0).status == ingest::ResampleStatus::kBadRate);
  CHECK(ingest::resample({}, 44'100, 48'000).status == ingest::ResampleStatus::kNoInput);
}
