#include "tui/render.hpp"
#include "tui/render_detail.hpp"
#include "tui/theme.hpp"
#include "tui/ui_state.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// BROWSE: finding a record, and hearing it before it joins the crate.
//
// A fourth pure function of the same UiState, beside render(), render_edit() and
// render_mix().
//
// A DEPARTURE THE MOCKUPS DO NOT COVER: they draw no browser at all.
// docs/design/README.md records it with the others. It is a SCREEN rather than a
// panel because a path, a listing and the crate beside it do not fit in the
// right-hand column PERFORM already spends on the sample and the pattern --
// shrinking it far enough would leave a file browser that cannot show a
// filename, which is the one thing it exists to do.

namespace tui {
namespace {

using ftxui::Element;
using ftxui::Elements;

using detail::paint_at;
using detail::utf8_cells;
using detail::with_precision;

// The crate panel's width. Enough for a name, a slice count and the marker that
// says which file is showing; the listing gets everything else, because a path
// is the longer of the two and the one that cannot be abbreviated usefully.
constexpr std::size_t kCrateWidth = 34;

// Below this the two panels stop being two panels and the crate is dropped
// rather than squeezed -- the listing is what BROWSE is for.
constexpr std::size_t kBothPanelsMinColumns = 78;

[[nodiscard]] std::string pad_to(std::string text, std::size_t cells) {
  while (utf8_cells(text) < cells) {
    text.push_back(' ');
  }
  return text;
}

// Clips from the LEFT for a path and from the right for a name.
//
// A path's identity is at its end -- `.../breaks/amen.wav` says more than
// `/Users/someone/Music/Sam...` -- while a filename's is at its start.
[[nodiscard]] std::string clip_path(std::string_view text, std::size_t cells) {
  const std::size_t width = utf8_cells(text);
  if (width <= cells || cells < 2) {
    return std::string{text};
  }
  auto [dropped, kept] = detail::utf8_split(text, width - (cells - 1));
  static_cast<void>(dropped);
  return "…" + kept;
}

[[nodiscard]] std::string clip_name(std::string_view text, std::size_t cells) {
  if (utf8_cells(text) <= cells || cells < 2) {
    return std::string{text};
  }
  auto [kept, dropped] = detail::utf8_split(text, cells - 1);
  static_cast<void>(dropped);
  return kept + "…";
}

// Bytes, in the unit a person reads. Not a precise figure: it is here to tell a
// loop from an album, which two significant digits answer.
[[nodiscard]] std::string format_bytes(std::uint64_t bytes) {
  if (bytes < 1'024) {
    return std::to_string(bytes) + " B";
  }
  const auto kilobytes = static_cast<double>(bytes) / 1'024.0;
  if (kilobytes < 1'024.0) {
    return with_precision(kilobytes, 0) + " KB";
  }
  const double megabytes = kilobytes / 1'024.0;
  return with_precision(megabytes, megabytes < 10.0 ? 1 : 0) + " MB";
}

// How many listing rows fit, and which of them to draw.
//
// The window follows the cursor rather than the cursor being clamped to the
// window: a listing is scrolled by moving through it, and a browser that stopped
// at the bottom of the first page would be one nobody could reach the end of.
[[nodiscard]] std::size_t window_start(const BrowserState& browser, std::size_t rows) {
  if (rows == 0 || browser.entries.size() <= rows) {
    return 0;
  }
  std::size_t first = std::min(browser.first_visible, browser.entries.size() - rows);
  if (browser.cursor < first) {
    first = browser.cursor;
  } else if (browser.cursor >= first + rows) {
    first = browser.cursor - rows + 1;
  }
  return first;
}

[[nodiscard]] Element listing_panel(const UiState& state, std::size_t width, std::size_t rows) {
  const BrowserState& browser = state.browser;
  const std::size_t inner = width > 2 ? width - 2 : 1;

  Elements lines;
  if (browser.entries.empty()) {
    // WHY it is empty, because an unreadable directory and an empty one look
    // identical otherwise -- and one of them is a mistake to fix.
    lines.push_back(
        ftxui::text(pad_to(
            "  " + (browser.note.empty() ? std::string{"nothing here to load"} : browser.note),
            inner)) |
        ftxui::color(theme::muted()));
  }

  const std::size_t first = window_start(browser, rows);
  for (std::size_t index = first; index < browser.entries.size() && lines.size() < rows; ++index) {
    const BrowserEntry& entry = browser.entries[index];
    const bool selected = index == browser.cursor;

    // Size and status on the right, name on the left, and the name is what gets
    // clipped: a listing that dropped the size to fit a long name would be one
    // where the columns move about as you scroll.
    const std::string right = entry.is_directory
                                  ? std::string{"dir"}
                                  : format_bytes(entry.bytes) + (entry.loaded ? " · loaded" : "");
    const std::size_t name_room = inner > right.size() + 4 ? inner - right.size() - 4 : 1;

    std::string row(inner, ' ');
    row = paint_at(row, 1, selected ? "▸" : " ");
    row =
        paint_at(row, 3, clip_name(entry.is_directory ? entry.name + "/" : entry.name, name_room));
    row = paint_at(row, inner > right.size() + 1 ? inner - right.size() - 1 : 0, right);

    Element line = ftxui::text(row);
    if (selected) {
      line = line | ftxui::inverted;
    } else if (entry.is_directory) {
      line = line | ftxui::color(theme::label());
    } else if (entry.loaded) {
      line = line | ftxui::color(theme::muted());
    }
    lines.push_back(line);
  }

  while (lines.size() < rows) {
    lines.push_back(ftxui::text(std::string(inner, ' ')));
  }

  Element panel = ftxui::vbox(std::move(lines)) | ftxui::border;
  return panel | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, static_cast<int>(width));
}

[[nodiscard]] Element crate_panel(const UiState& state, std::size_t rows) {
  constexpr std::size_t kInner = kCrateWidth - 2;
  Elements lines;

  if (state.files.empty()) {
    lines.push_back(ftxui::text(pad_to("  the crate is empty", kInner)) |
                    ftxui::color(theme::muted()));
  }

  for (std::size_t index = 0; index < state.files.size() && lines.size() < rows; ++index) {
    const UiState::FileEntry& file = state.files[index];
    const bool showing = file.id == state.current_file;

    const std::string count = std::to_string(file.slices) + (file.slices == 1 ? " cut" : " cuts");
    std::string row(kInner, ' ');
    row = paint_at(row, 1, std::to_string(index + 1));
    row = paint_at(row, 3,
                   clip_name(file.name, kInner > count.size() + 6 ? kInner - count.size() - 6 : 1));
    row = paint_at(row, kInner > count.size() + 1 ? kInner - count.size() - 1 : 0, count);

    Element line = ftxui::text(row);
    // The one being shown is marked, because `:file N` and the wave panel are
    // the same question asked twice.
    lines.push_back(showing ? line | ftxui::color(theme::accent()) | ftxui::bold : line);
  }

  while (lines.size() < rows) {
    lines.push_back(ftxui::text(std::string(kInner, ' ')));
  }

  Element panel = ftxui::vbox(std::move(lines)) | ftxui::border;
  return panel | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, static_cast<int>(kCrateWidth));
}

}  // namespace

Element render_browse(const UiState& state, std::size_t terminal_columns,
                      std::size_t terminal_rows) {
  const BrowserState& browser = state.browser;

  // Header, two panels, mode line -- and a blank line under the header, the
  // shape every other screen here uses.
  const std::size_t chrome = 5;
  const std::size_t rows = terminal_rows > chrome + 2 ? terminal_rows - chrome : 3;

  const bool both = terminal_columns >= kBothPanelsMinColumns;
  const std::size_t listing_width = both ? terminal_columns - kCrateWidth - 1 : terminal_columns;

  Elements panels;
  panels.push_back(listing_panel(state, listing_width, rows));
  if (both) {
    panels.push_back(ftxui::text(" "));
    panels.push_back(crate_panel(state, rows));
  }

  Element header = ftxui::hbox({
      ftxui::text("cratedig ") | ftxui::color(theme::bright()),
      ftxui::text(state.version + "  ") | ftxui::color(theme::muted()),
      ftxui::text("browse") | ftxui::color(theme::label()),
      ftxui::filler(),
      ftxui::text(clip_path(browser.path, terminal_columns / 2)) | ftxui::color(theme::muted()),
  });

  // One fact, because the crate panel is right there saying the other one. The
  // MIX screen learned the same thing: a mode line repeating what is two lines
  // above it costs a hint tier and tells nobody anything.
  std::vector<std::string> facts;
  facts.push_back(std::to_string(browser.entries.size()) + " here");

  static constexpr std::array<std::string_view, 3> kHints{
      "jk move · l open · h up · space audition · enter load · esc back",
      "jk move · l open · h up · space hear · enter load",
      "jk · l/h · enter",
  };

  Element line = detail::mode_line(state, terminal_columns, "  browse    ", facts, kHints, 24);

  return ftxui::vbox({
      header,
      ftxui::text(""),
      ftxui::hbox(std::move(panels)),
      ftxui::filler(),
      line,
  });
}

}  // namespace tui
