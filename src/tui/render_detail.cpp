#include "tui/render_detail.hpp"

#include "tui/theme.hpp"
#include "tui/ui_state.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tui::detail {

std::string with_precision(double value, int digits) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(digits) << value;
  return out.str();
}

std::string format_time(double seconds, double span_seconds) {
  if (seconds < 0.0) {
    seconds = 0.0;
  }
  if (span_seconds >= 120.0) {
    const auto minutes = static_cast<int>(seconds / 60.0);
    return std::to_string(minutes) + ":" + (seconds - (minutes * 60.0) < 10.0 ? "0" : "") +
           with_precision(seconds - (minutes * 60.0), 0);
  }
  if (span_seconds >= 10.0) {
    return with_precision(seconds, 1) + "s";
  }
  if (span_seconds >= 1.0) {
    return with_precision(seconds, 2) + "s";
  }
  return with_precision(seconds * 1000.0, 1) + "ms";
}

float db_to_linear(float decibels) {
  if (!std::isfinite(decibels)) {
    return 1.0F;
  }
  if (decibels == 0.0F) {
    return 1.0F;  // exactly unity, not almost
  }
  return static_cast<float>(std::pow(10.0, static_cast<double>(decibels) / 20.0));
}

float linear_to_db(float linear) {
  if (!std::isfinite(linear) || linear <= 0.0F) {
    return -60.0F;
  }
  if (linear == 1.0F) {
    return 0.0F;  // exactly unity, so a nudge from unity starts from 0.0 and not -0.0000001
  }
  return static_cast<float>(20.0 * std::log10(static_cast<double>(linear)));
}

std::string format_dbfs(float linear) {
  if (linear <= 0.0F) {
    return "-inf";
  }
  return with_precision(20.0 * std::log10(static_cast<double>(linear)), 1);
}

std::size_t utf8_cells(std::string_view text) {
  std::size_t cells = 0;
  for (const char byte : text) {
    if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U) {
      ++cells;
    }
  }
  return cells;
}

std::pair<std::string, std::string> utf8_split(std::string_view text, std::size_t cells) {
  std::size_t seen = 0;
  std::size_t offset = 0;
  while (offset < text.size()) {
    if ((static_cast<unsigned char>(text[offset]) & 0xC0U) != 0x80U) {
      if (seen == cells) {
        break;
      }
      ++seen;
    }
    ++offset;
  }
  // Walk to the end of the character we stopped on.
  while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U) {
    ++offset;
  }
  return {std::string{text.substr(0, offset)}, std::string{text.substr(offset)}};
}

std::pair<std::string, std::string> utf8_take_one(std::string_view text) {
  return utf8_split(text, 1);
}

std::string splice_at(std::string_view text, std::size_t column, std::string_view glyph) {
  if (column >= utf8_cells(text)) {
    return std::string{text};
  }
  auto [before, rest] = utf8_split(text, column);
  auto [under, after] = utf8_take_one(rest);
  static_cast<void>(under);  // replaced, not blended -- as the mockups draw it
  return before + std::string{glyph} + after;
}

std::string paint_at(std::string_view row, std::size_t column, std::string_view text) {
  const std::size_t cells = utf8_cells(row);
  if (column >= cells) {
    return std::string{row};
  }
  auto [before, rest] = utf8_split(row, column);

  // Clipped to what is left of the row, so the result is exactly as wide as it
  // arrived. A caller that writes past the end gets a short write rather than a
  // wider row, because the width is the panel's and not this function's to
  // change.
  std::string glyph{text};
  std::size_t glyph_cells = utf8_cells(glyph);
  if (glyph_cells > cells - column) {
    glyph = utf8_split(glyph, cells - column).first;
    glyph_cells = utf8_cells(glyph);
  }
  return before + glyph + utf8_split(rest, glyph_cells).second;
}

// The Tab menu, drawn under the prompt as the COMMAND mockup has it.
//
// UNDER rather than over, which is where the mockup puts it and is also the
// only place it can go: the mode line is the last row of every screen's vbox,
// so anything above it would have to displace a panel, and a menu that resized
// the waveform every time you pressed Tab would be a menu nobody pressed twice.
namespace {

// How many entries fit, given the rows the caller can spare.
//
// THE MENU SHRINKS RATHER THAN VANISHING. The first version dropped it whole
// when the screen would have gone below its floor, which at the design size of
// 30 rows meant a bare `:` -- thirty-three verbs wanting eleven rows against a
// budget of ten -- silently showed nothing at all. Fewer rows and an honest
// "n more" is the answer; all-or-nothing is not.
[[nodiscard]] std::size_t visible_entries(const CompletionState& completion, std::size_t max_rows) {
  if (!completion.showing() || max_rows < 3) {
    return 0;  // under three there is no room for a rule, one entry and a caption
  }
  // Two go to the rule and the caption.
  const std::size_t spare = max_rows - 2;
  std::size_t visible = std::min({kMaxCompletionRows, completion.entries.size(), spare});

  // The "n more" line needs a row of its own, and taking it from the entries is
  // what keeps the total inside the budget rather than one over it.
  if (completion.entries.size() > visible && visible + 1 > spare) {
    --visible;
  }
  return visible;
}

}  // namespace

std::size_t completion_rows(const CompletionState& completion, std::size_t max_rows) {
  const std::size_t visible = visible_entries(completion, max_rows);
  if (visible == 0) {
    return 0;
  }
  // The rule, the entries, the "n more" line when there is one, and the caption.
  // Derived from the SAME visible_entries() the menu draws, so the two cannot
  // disagree -- a height that under-reported would clip the caption off the
  // bottom of the terminal, which is what this whole wrapper exists to prevent.
  return 1 + visible + (completion.entries.size() > visible ? 1 : 0) + 1;
}

ftxui::Element completion_menu(const CompletionState& completion, std::size_t columns,
                               std::size_t max_rows) {
  // The widest phrase, so the details line up in a column. Measured over the
  // WHOLE set rather than the visible window: a column that moved as you cycled
  // would be a column, moving.
  std::size_t widest = 0;
  for (const Completion& entry : completion.entries) {
    widest = std::max(widest, utf8_cells(entry.text));
  }

  const std::size_t rows = visible_entries(completion, max_rows);

  // Scroll to keep the selection visible, exactly as BROWSE's listing does.
  std::size_t first = 0;
  if (completion.cursor >= rows) {
    first = completion.cursor - rows + 1;
  }

  ftxui::Elements lines;
  std::string rule;
  for (std::size_t cell = 0; cell < columns; ++cell) {
    rule += "\u2500";
  }
  lines.push_back(ftxui::text(rule) | ftxui::color(theme::muted()));

  for (std::size_t index = first; index < completion.entries.size() && lines.size() <= rows;
       ++index) {
    const Completion& entry = completion.entries[index];
    const bool selected = index == completion.cursor;

    // THE MARKER IS OUTSIDE WHAT GETS CLIPPED. Clipping the assembled row --
    // prefix and all -- is what the first version did, and a left-clipped path
    // then ate the very glyph that says which row is selected.
    const std::string prefix = std::string{"  "} + (selected ? "\u258c " : "  ");
    const std::size_t room = columns > utf8_cells(prefix) + 1 ? columns - utf8_cells(prefix) : 1;

    std::string body = entry.text;
    if (entry.detail.empty()) {
      // A PATH, and a path clips from the LEFT -- the rule BROWSE's header
      // already uses, because `.../breaks/amen.wav` says more than
      // `/Users/someone/Music/Sam...`. No padding either: aligning to the widest
      // entry is for the detail column, and one path among paths has none.
      if (utf8_cells(body) > room) {
        body = "\u2026" + utf8_split(body, utf8_cells(body) - (room - 1)).second;
      }
    } else {
      while (utf8_cells(body) < widest + 2) {
        body.push_back(' ');
      }
      body += entry.detail;
      if (utf8_cells(body) > room) {
        body = utf8_split(body, room).first;  // a verb's identity is its first word
      }
    }

    ftxui::Element line = ftxui::text(prefix + body);
    lines.push_back(selected ? line | ftxui::color(theme::accent()) | ftxui::bold
                             : line | ftxui::color(theme::muted()));
  }

  // How many did not fit, said rather than left to be discovered -- a menu that
  // silently showed eight of thirty-three would read as "there are eight".
  if (completion.entries.size() > rows) {
    lines.push_back(
        ftxui::text("    " + std::to_string(completion.entries.size() - rows) + " more") |
        ftxui::color(theme::muted()));
  }

  // TAB AND SHIFT-TAB, not j and k, and that is a departure the mockup forced
  // rather than one chosen. Its caption row reads `j k select`; `j` and `k` are
  // letters you are in the middle of typing. No verb here contains either, so
  // verbs alone would have survived it -- but paths are completed with the same
  // menu, and `kits/`, `kick.wav` and `jungle/` are exactly the names a crate is
  // full of. A menu you cannot use while typing the thing it is completing is
  // not a menu.
  lines.push_back(
      ftxui::text("  tab next \u00b7 shift-tab back \u00b7 enter run \u00b7 esc close") |
      ftxui::color(theme::muted()));

  return ftxui::vbox(std::move(lines));
}

ftxui::Element mode_line(const UiState& state, std::size_t columns, std::string_view prefix,
                         const std::vector<std::string>& facts,
                         std::span<const std::string_view> hint_tiers, std::size_t min_fact_cells) {
  // The prompt takes the WHOLE line when it is up. A `:` line sharing space
  // with a keymap is a prompt you cannot read, and the facts it would be
  // competing with are all still one keystroke away.
  //
  // The block is a cursor. FTXUI can place a real terminal cursor, but doing so
  // would put a blinking, terminal-dependent artefact into the PTY snapshot --
  // this draws the same information deterministically.
  if (state.command_active) {
    // The `:` is PINNED and the text scrolls under it, as in every other line
    // editor that has ever had one. Letting the colon scroll off would take the
    // one glyph that says which mode you are in.
    //
    // Three cells go elsewhere: the leading space, the colon, and the cursor.
    std::string typed = state.command_text;
    const std::size_t room = columns > 3 ? columns - 3 : 1;
    if (utf8_cells(typed) > room) {
      // Keep the END visible. What is being typed matters more than what was
      // typed a moment ago, and truncating the tail instead looks exactly like
      // a prompt that has stopped accepting input.
      typed = utf8_split(typed, utf8_cells(typed) - room).second;
    }
    ftxui::Elements prompt{
        ftxui::text(" "),
        ftxui::text(":") | ftxui::color(theme::accent()),
        ftxui::text(typed) | ftxui::color(theme::bright()),
        ftxui::text("█") | ftxui::color(theme::accent()),
        ftxui::filler(),
    };

    // A NOTE ON THE PROMPT'S OWN LINE, right-aligned.
    //
    // The message branch below is unreachable while the prompt is up -- this one
    // returns first -- so anything set_message() said during a command was
    // simply invisible. That went unnoticed until Tab wanted to answer "no
    // command starts with that", and a Tab that says nothing is exactly the
    // confusion that cost M5 a wrong finding about Tab in the first place.
    //
    // Right-aligned rather than on a row of its own: a row would have to come
    // out of the screen's budget the way the menu does, and this is one short
    // sentence that fits in the space the typed line is not using.
    if (!state.message.empty()) {
      const std::size_t used = utf8_cells(typed) + 3;
      const std::size_t spare = columns > used + 2 ? columns - used - 2 : 0;
      if (utf8_cells(state.message) <= spare) {
        prompt.push_back(ftxui::text(state.message + " ") |
                         ftxui::color(state.message_is_error ? theme::accent() : theme::muted()));
      }
    }
    return ftxui::hbox(std::move(prompt));
  }

  // What the last command said, in place of everything else. See UiState for
  // why it wins over the counters and the keymap both.
  if (!state.message.empty()) {
    const std::size_t room =
        columns > utf8_cells(prefix) + 1 ? columns - utf8_cells(prefix) - 1 : 1;
    std::string text = state.message;
    if (utf8_cells(text) > room) {
      text = utf8_split(text, room).first;
    }
    return ftxui::hbox({
        ftxui::text(std::string{prefix}) | ftxui::color(theme::label()) | ftxui::bold,
        ftxui::text(text) |
            ftxui::color(state.message_is_error ? theme::accent() : theme::bright()) |
            (state.message_is_error ? ftxui::bold : ftxui::nothing),
        ftxui::filler(),
    });
  }

  // Cells, not bytes: the separator is a two-byte character one column wide, and
  // budgeting in bytes silently loses a column per fact.
  std::string_view hints = hint_tiers.empty() ? std::string_view{} : hint_tiers.back();
  for (const std::string_view tier : hint_tiers) {
    if (utf8_cells(prefix) + utf8_cells(tier) + min_fact_cells <= columns) {
      hints = tier;
      break;
    }
  }

  const std::size_t reserved = utf8_cells(prefix) + utf8_cells(hints);
  std::size_t budget = columns > reserved ? columns - reserved : 0;

  std::string shown;
  for (const std::string& fact : facts) {
    // The +1 keeps at least one space between the last fact and the hint. A
    // mode line that exactly fills its width reads as two run-together words.
    const std::size_t extra = utf8_cells(fact) + (shown.empty() ? 0 : 3) + 1;
    if (extra > budget) {
      break;
    }
    budget -= extra;
    shown += shown.empty() ? fact : " · " + fact;
  }

  return ftxui::hbox({
      ftxui::text(std::string{prefix}) | ftxui::color(theme::label()) | ftxui::bold,
      ftxui::text(shown) | ftxui::color(theme::muted()),
      ftxui::filler(),
      ftxui::text(std::string{hints}) | ftxui::color(theme::structure()),
  });
}

}  // namespace tui::detail
