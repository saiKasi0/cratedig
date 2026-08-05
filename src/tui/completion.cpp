#include "tui/completion.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tui {
namespace {

// Every phrase the prompt can finish, and what it does.
//
// CONSTEXPR, per the coding standard, and hand-ordered rather than sorted: the
// order here is the order on screen, and it groups by what you are doing --
// chopping, then pads, then the crate, then the mixer, then the sequencer --
// rather than by spelling. A menu sorted alphabetically puts `bpm` above `bus`
// above `chop` and teaches nobody anything.
//
// THIS TABLE IS A SECOND SPELLING OF parse_command()'s VERBS and there is no
// mechanism keeping them in step. A test walks every entry through
// parse_command() and fails if one does not parse, which is the check that
// makes the duplication safe; see completion_test.cpp. The alternative -- one
// table driving both -- would put help text in the parser, and the parser is
// -fno-exceptions RT-adjacent code that has no business carrying prose.
struct Phrase {
  std::string_view text;
  std::string_view detail;
};

// THE SIZE IS DEDUCED, and that is not a style choice. Written as
// `std::array<Phrase, 34>` over 33 entries, the thirty-fourth is
// default-constructed with an EMPTY text -- and an empty string is a prefix of
// every line, so the "this phrase is already finished" guard below fired on
// every keystroke and the menu was empty always. It cost a segfault in the
// first test run to find. A deduced size cannot be wrong.
constexpr std::array kPhrases{
    // Chopping.
    Phrase{"chop grid", "chop into n equal parts"},
    Phrase{"chop transient", "detect hits and cut there"},
    Phrase{"chop transient beat", "one cut a beat, at the session tempo"},
    Phrase{"chop transient bar", "one cut a bar — riffs whole"},
    Phrase{"chop reset", "back to one slice"},
    Phrase{"chop tune", "adjust the chop while watching it"},
    Phrase{"slot assign", "bind slices to pads"},
    Phrase{"edit", "open EDIT on a slice"},
    Phrase{"env", "set a pad's attack, decay, sustain or release"},
    Phrase{"pitch", "play a pad faster or slower — +7 semitones or 1.5x"},
    Phrase{"reverse", "play a pad backwards"},

    // The crate.
    Phrase{"browse", "find a file to load"},
    Phrase{"load", "add a file to the crate"},
    Phrase{"files", "list what is loaded"},
    Phrase{"file", "show one of them"},
    Phrase{"unload", "drop one from the crate"},

    // Pads.
    Phrase{"pad gate", "pads sound while held"},
    Phrase{"pad oneshot", "pads play to the end"},
    Phrase{"stop", "silence a pad, or everything"},

    // The mixer.
    Phrase{"mix", "open MIX"},
    Phrase{"gain", "set a strip's level in dB"},
    Phrase{"pan", "place a strip left or right"},
    Phrase{"mute", "silence a strip"},
    Phrase{"solo", "hear one strip alone"},
    Phrase{"bus", "route a strip to a bus"},
    Phrase{"eq", "set a band, or turn the EQ off"},
    Phrase{"comp", "set the compressor, or turn it off"},
    Phrase{"limit", "the master limiter and its ceiling"},

    // The sequencer.
    Phrase{"bpm", "set the tempo"},
    Phrase{"bpm detect", "read the tempo off the file"},
    Phrase{"tape", "scale the tempo — 1.05 is a nudge faster"},
    Phrase{"swing", "delay every other step"},
    Phrase{"pattern", "select a pattern by number"},
    Phrase{"pattern length", "how many steps it has"},
    Phrase{"pattern clear", "empty it"},
    Phrase{"song", "play patterns in an order"},
    Phrase{"song clear", "back to one pattern repeating"},
    Phrase{"metro", "the metronome, on or off"},

    // Playing a pattern in.
    Phrase{"rec", "arm — play the pattern in instead of typing it"},
    Phrase{"rec quant", "how hard to round what you play: 16, 8, 4, 2"},
    Phrase{"rec replace", "a take clears the pattern first"},
    Phrase{"rec overdub", "a take adds to the pattern"},
    Phrase{"rec undo", "put the pattern back as it was"},

    // Getting about.
    Phrase{"perform", "back to PERFORM"},
    Phrase{"quit", "leave cratedig"},
};

// A phrase must never be empty: an empty string is a prefix of every line, so
// one would swallow the whole menu. Checked here rather than trusted.
static_assert(std::none_of(kPhrases.begin(), kPhrases.end(),
                           [](const Phrase& phrase) { return phrase.text.empty(); }),
              "an empty phrase would match every line and empty the menu");

// Verbs whose argument is a path. Short enough to scan linearly and unlikely to
// grow: a verb that takes a path is a verb that reaches the filesystem, and
// there are only ever a few of those.
constexpr std::array<std::string_view, 2> kPathVerbs{"load", "browse"};

[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// The typed line with leading blanks removed, and how many were removed.
//
// Leading space is not an error at a prompt -- `: chop` is what a slow typist
// produces -- and completing it must offer the same thing `:chop` does, from
// the right place.
[[nodiscard]] std::size_t first_word_start(std::string_view typed) {
  std::size_t start = 0;
  while (start < typed.size() && typed[start] == ' ') {
    ++start;
  }
  return start;
}

}  // namespace

std::string CompletionSet::apply(std::string_view typed) const {
  if (entries.empty() || cursor >= entries.size()) {
    return std::string{typed};
  }
  const std::size_t from = std::min(replace_from, typed.size());
  return std::string{typed.substr(0, from)} + entries[cursor].text;
}

CompletionSet complete_verbs(std::string_view typed) {
  CompletionSet set;
  const std::size_t start = first_word_start(typed);
  const std::string_view line = typed.substr(start);
  set.replace_from = start;

  // A FINISHED PHRASE FALLS OUT OF THE PREFIX RULE rather than needing a guard
  // of its own. `chop grid 16` offers nothing because no phrase in the table
  // starts with it -- a line longer than a phrase cannot be that phrase's
  // prefix. An explicit "is this already finished" loop stood here first; a
  // negative control deleting it changed no test on any input, which is what
  // dead code looks like when you check. The property is still asserted in
  // completion_test.cpp, because it is worth knowing and it is now emergent
  // rather than enforced.
  for (const Phrase& phrase : kPhrases) {
    if (starts_with(phrase.text, line)) {
      set.entries.push_back(
          Completion{.text = std::string{phrase.text}, .detail = std::string{phrase.detail}});
    }
  }

  // AN EXACT AND ONLY MATCH IS NOT AN OFFER. `:quit` typed in full has one
  // match, itself, and a menu showing the thing already on the line is a row of
  // pixels saying nothing. Two-level phrases are the exception and the reason
  // this is not simply "drop exact matches": `pattern` is complete on its own
  // AND the prefix of `pattern length`, so it stays while its children are
  // there to be shown beside it.
  if (set.entries.size() == 1 && set.entries.front().text == line) {
    set.entries.clear();
  }
  return set;
}

PathContext path_being_typed(std::string_view typed) {
  PathContext context;
  const std::size_t start = first_word_start(typed);
  const std::string_view line = typed.substr(start);

  for (const std::string_view verb : kPathVerbs) {
    if (!starts_with(line, verb) || line.size() <= verb.size() || line[verb.size()] != ' ') {
      continue;
    }
    // Everything after the space, VERBATIM -- spaces included. A path with a
    // space in it is ordinary on both platforms this runs on, and splitting on
    // whitespace here would make `~/Field Recordings/` uncompletable in exactly
    // the directories people keep field recordings in. parse_command() takes
    // the rest of the line for the same reason.
    context.is_path = true;
    context.replace_from = start + verb.size() + 1;
    context.partial = std::string{line.substr(verb.size() + 1)};
    return context;
  }
  return context;
}

CompletionSet complete_paths(const PathContext& context, const std::vector<std::string>& names) {
  CompletionSet set;
  set.replace_from = context.replace_from;
  if (!context.is_path) {
    return set;
  }

  for (const std::string& name : names) {
    if (starts_with(name, context.partial)) {
      // No detail: a path says what it is. The right-hand column is for verbs,
      // whose names are short enough to be ambiguous.
      set.entries.push_back(Completion{.text = name, .detail = {}});
    }
  }

  // Sorted, unlike the verbs. A directory has no meaningful hand-order and the
  // filesystem's own is arbitrary, so a listing that changed order between two
  // machines would make the menu unsnapshotable -- the same reason BROWSE sorts.
  std::sort(set.entries.begin(), set.entries.end(),
            [](const Completion& left, const Completion& right) { return left.text < right.text; });

  if (set.entries.size() == 1 && set.entries.front().text == context.partial) {
    set.entries.clear();  // already typed in full; see complete_verbs()
  }
  return set;
}

}  // namespace tui
