#include "tui/cli.hpp"

#include "ingest/decoder.hpp"
#include "io/audio_device.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace tui {

void print_version() {
  std::cout << "cratedig " << CRATEDIG_VERSION << " — the terminal crate-digging DAW\n";
  std::cout << "  " << ingest::ffmpeg_versions() << '\n';

  // Printed because docs/LICENSING.md scopes the LGPL requirement to distributed
  // binaries and enforces it at configure time with
  // CRATEDIG_REQUIRE_LGPL_FFMPEG. This is the runtime half: which FFmpeg is
  // actually linked should be observable, not assumed.
  std::cout << "  FFmpeg license: " << ingest::ffmpeg_license() << '\n';
  if (!ingest::ffmpeg_license().starts_with("LGPL")) {
    std::cout << "  note: this is a GPL FFmpeg build. Fine for local use — nothing is being\n"
              << "        redistributed — but a CRATEDIG binary distributed against it would\n"
              << "        be GPL rather than Apache-2.0. See docs/LICENSING.md.\n";
  }
}

int list_devices() {
  const io::AudioDevice device;
  std::cout << "audio API: " << device.api_name() << '\n';

  const std::vector<io::DeviceInfo> outputs = device.output_devices();
  std::cout << "output:\n";
  if (outputs.empty()) {
    // Not an error status: a container legitimately has none, and `cratedig
    // --list-devices` succeeding with an empty list is more useful in a script
    // than a non-zero exit.
    std::cout << "  none found\n";
  }
  for (const io::DeviceInfo& info : outputs) {
    std::cout << "  [" << info.id << "] " << info.name << "  " << info.output_channels << " ch, "
              << info.preferred_sample_rate << " Hz"
              << (info.is_default_output ? "  (default)" : "") << '\n';
  }

  // LISTED SEPARATELY, because `--input-device` takes one of these and the
  // numbers are not interchangeable: a device can capture and not play, or
  // play and not capture, and picking an id off the wrong list is a failure at
  // open() with nothing to explain it.
  const std::vector<io::DeviceInfo> inputs = device.input_devices();
  std::cout << "input:\n";
  if (inputs.empty()) {
    std::cout << "  none found\n";
  }
  for (const io::DeviceInfo& info : inputs) {
    std::cout << "  [" << info.id << "] " << info.name << "  " << info.input_channels << " ch, "
              << info.preferred_sample_rate << " Hz" << (info.is_default_input ? "  (default)" : "")
              << '\n';
  }
  return 0;
}

}  // namespace tui
