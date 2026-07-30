// Decode tests against the real CC0 starter pack.
//
// These are the only decode tests allowed to skip. Everything about decoder
// *correctness* is pinned by the byte-array WAV in decode_test.cpp, which needs
// no files and always runs; what these add is real-world material — actual FLAC
// frames, actual stereo, a rate that is not the engine's — which cannot be
// assembled by hand.
//
// Fixtures are fetched, never committed (docs/TESTING.md), so a fresh clone has
// none until scripts/fetch_starter_pack.sh runs. When a file is missing these
// SKIP with a reason rather than fail, and rather than passing vacuously; the
// `fixture` label runs them deliberately.

#include "ingest/decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace {

// The pack lives beside the sources, not beside the build tree, so this is
// resolved from a compile definition rather than from the working directory —
// ctest runs tests from the build directory.
std::filesystem::path fixture(const std::string& name) {
  return std::filesystem::path{CRATEDIG_STARTER_PACK_DIR} / name;
}

// Returns true when the fixture is present; otherwise records why it skipped.
bool have(const std::filesystem::path& path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored);
}

}  // namespace

TEST_CASE("Starter pack: the kick decodes", "[fixture]") {
  const std::filesystem::path path = fixture("drum_heavy_kick.flac");
  if (!have(path)) {
    SKIP("starter pack not fetched — run scripts/fetch_starter_pack.sh");
  }

  const ingest::SampleLoad load = ingest::load_sample(path, 0);
  INFO("decode error: " << ingest::describe(load.error) << " (" << load.detail << ")");
  REQUIRE(load.ok());

  // Properties recorded in MANIFEST.toml. Asserting them here means a fixture
  // that was silently replaced by a different file fails loudly, on top of the
  // checksum check in scripts/verify_fixtures.py.
  CHECK(load.sample->sample_rate() == 44'100);
  CHECK(load.sample->num_channels() == 1);
  CHECK(load.sample->num_frames() > 10'000);  // ~0.27 s at 44.1 kHz

  // A kick that decoded to silence would satisfy everything above.
  bool audible = false;
  for (const float value : load.sample->channel(0)) {
    if (value > 0.1F || value < -0.1F) {
      audible = true;
      break;
    }
  }
  CHECK(audible);
}

TEST_CASE("Starter pack: stereo files keep both channels", "[fixture]") {
  const std::filesystem::path path = fixture("ambi_choir.flac");
  if (!have(path)) {
    SKIP("starter pack not fetched — run scripts/fetch_starter_pack.sh");
  }

  const ingest::SampleLoad load = ingest::load_sample(path, 0);
  REQUIRE(load.ok());
  REQUIRE(load.sample->num_channels() == 2);

  // Both channels carry signal, and they are not the same signal — which is what
  // catches a decoder that duplicated channel 0 into both planes.
  std::size_t differences = 0;
  for (std::size_t frame = 0; frame < load.sample->num_frames(); ++frame) {
    if (load.sample->channel(0)[frame] != load.sample->channel(1)[frame]) {
      ++differences;
    }
  }
  CHECK(differences > 0);
}

TEST_CASE("Starter pack: WAV and FLAC of the same audio decode identically", "[fixture]") {
  // kick_44k.wav is transcoded from drum_heavy_kick.flac losslessly, so this is
  // the check that makes the derived fixtures trustworthy without hash equality:
  // two different containers and two different decoders must produce the same
  // samples. It also covers the WAV row of the coverage table.
  const std::filesystem::path flac = fixture("drum_heavy_kick.flac");
  const std::filesystem::path wav = fixture("kick_44k.wav");
  if (!have(flac) || !have(wav)) {
    SKIP("starter pack not fetched — run scripts/fetch_starter_pack.sh");
  }

  const ingest::SampleLoad from_flac = ingest::load_sample(flac, 0);
  const ingest::SampleLoad from_wav = ingest::load_sample(wav, 0);
  REQUIRE(from_flac.ok());
  REQUIRE(from_wav.ok());

  CHECK(from_wav.sample->sample_rate() == from_flac.sample->sample_rate());
  REQUIRE(from_wav.sample->num_channels() == from_flac.sample->num_channels());
  REQUIRE(from_wav.sample->num_frames() == from_flac.sample->num_frames());

  for (std::uint16_t channel = 0; channel < from_flac.sample->num_channels(); ++channel) {
    for (std::size_t frame = 0; frame < from_flac.sample->num_frames(); ++frame) {
      REQUIRE(from_wav.sample->channel(channel)[frame] ==
              from_flac.sample->channel(channel)[frame]);
    }
  }
}

TEST_CASE("Starter pack: 48 kHz material skips the resampler", "[fixture]") {
  // The other half of the sample-rate coverage. Loading a 48 kHz file onto a
  // 48 kHz engine must take the unity-ratio short circuit and come back
  // untouched, while the 44.1 kHz files above exercise the conversion path.
  const std::filesystem::path path = fixture("kick_48k.wav");
  if (!have(path)) {
    SKIP("starter pack not fetched — run scripts/fetch_starter_pack.sh");
  }

  const ingest::SampleLoad native = ingest::load_sample(path, 0);
  const ingest::SampleLoad engine_rate = ingest::load_sample(path, 48'000);
  REQUIRE(native.ok());
  REQUIRE(engine_rate.ok());

  CHECK(native.sample->sample_rate() == 48'000);
  REQUIRE(engine_rate.sample->num_frames() == native.sample->num_frames());
  for (std::size_t frame = 0; frame < native.sample->num_frames(); ++frame) {
    REQUIRE(engine_rate.sample->channel(0)[frame] == native.sample->channel(0)[frame]);
  }
}

TEST_CASE("Starter pack: 44.1 kHz material converts to the engine rate", "[fixture]") {
  const std::filesystem::path path = fixture("drum_heavy_kick.flac");
  if (!have(path)) {
    SKIP("starter pack not fetched — run scripts/fetch_starter_pack.sh");
  }

  const ingest::SampleLoad native = ingest::load_sample(path, 0);
  const ingest::SampleLoad converted = ingest::load_sample(path, 48'000);
  REQUIRE(native.ok());
  REQUIRE(converted.ok());

  CHECK(converted.sample->sample_rate() == 48'000);

  // 48000/44100 longer, within the converter's transient.
  const auto expected =
      static_cast<double>(native.sample->num_frames()) * 48'000.0 / 44'100.0;
  const auto actual = static_cast<double>(converted.sample->num_frames());
  CHECK(actual > expected - 32.0);
  CHECK(actual < expected + 32.0);
}
