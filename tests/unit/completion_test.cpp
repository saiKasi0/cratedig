#include "tui/completion.hpp"

#include "tui/command.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

[[nodiscard]] std::vector<std::string> texts(const tui::CompletionSet& set) {
  std::vector<std::string> out;
  out.reserve(set.size());
  for (const tui::Completion& entry : set.entries) {
    out.push_back(entry.text);
  }
  return out;
}

[[nodiscard]] bool has(const tui::CompletionSet& set, const std::string& text) {
  const std::vector<std::string> all = texts(set);
  return std::find(all.begin(), all.end(), text) != all.end();
}

}  // namespace

TEST_CASE("a prefix offers every phrase that extends it", "[unit]") {
  const tui::CompletionSet set = tui::complete_verbs("ch");
  REQUIRE(texts(set) == std::vector<std::string>{"chop grid", "chop transient",
                                                 "chop transient beat", "chop transient bar",
                                                 "chop reset", "chop tune"});

  // In table order, not alphabetical: `grid` before `transient` before `reset`
  // is the order you would try them in, and sorting would put `reset` first.
  CHECK(set.entries.front().detail == "chop into n equal parts");
}

TEST_CASE("a phrase is offered whole, not as the missing tail", "[unit]") {
  // The property that makes `apply()` and `replace_from` unable to drift: what
  // is offered is what the line becomes, so accepting is a replacement rather
  // than an append.
  const tui::CompletionSet set = tui::complete_verbs("ch");
  REQUIRE(!set.empty());
  CHECK(set.entries.front().text == "chop grid");
  CHECK(set.apply("ch") == "chop grid");
  CHECK(set.replace_from == 0);
}

TEST_CASE("accepting the second offer replaces, not appends", "[unit]") {
  tui::CompletionSet set = tui::complete_verbs("pad ");
  REQUIRE(set.size() == 2);
  set.cursor = 1;
  CHECK(set.apply("pad ") == "pad oneshot");

  // And the first, so that the cursor is doing the choosing rather than the
  // order coincidentally agreeing with it.
  set.cursor = 0;
  CHECK(set.apply("pad ") == "pad gate");
}

TEST_CASE("a two-level phrase keeps its parent beside its children", "[unit]") {
  // `pattern` is a complete command AND the prefix of two others. Dropping it
  // as an exact match would make `:pattern 3` untypable from the menu.
  const tui::CompletionSet set = tui::complete_verbs("pattern");
  CHECK(has(set, "pattern"));
  CHECK(has(set, "pattern length"));
  CHECK(has(set, "pattern clear"));
}

TEST_CASE("an exact and only match is not offered", "[unit]") {
  // `quit` extends nothing. A menu with one row saying what is already typed is
  // a row of pixels that tells nobody anything.
  CHECK(tui::complete_verbs("quit").empty());

  // But the same word one letter short is an offer.
  CHECK(has(tui::complete_verbs("qui"), "quit"));
}

TEST_CASE("a finished phrase with an argument closes the menu", "[unit]") {
  // You are typing a number now, not choosing a verb, and a menu over the top
  // of it would hide the thing it was helping you write.
  //
  // EMERGENT, NOT ENFORCED. A guard for exactly this stood in complete_verbs()
  // until a negative control showed deleting it changed nothing on any input:
  // the prefix rule already covers it, because a line longer than a phrase
  // cannot be that phrase's prefix. Asserted here anyway -- the behaviour is
  // worth pinning whether it costs a branch or none.
  CHECK(tui::complete_verbs("chop grid 16").empty());
  CHECK(tui::complete_verbs("chop grid 1").empty());
  CHECK(tui::complete_verbs("bpm 92").empty());

  // The boundary, and it goes the other way: `chop grid` typed in full is its
  // own only match, so the same rule that hides a lone `quit` hides it. It
  // still needs an argument, and a case could be made for keeping the row up
  // as a reminder of which -- but that needs an arity column the table does not
  // have, and the parser already answers it usefully the moment you press
  // Enter ("chop grid needs a count, e.g. chop grid 16"). One rule, applied
  // everywhere, beats two rules and a new column.
  CHECK(tui::complete_verbs("chop grid").empty());

  // One letter short of it, though, is an offer -- which is what makes the rule
  // above a rule about EXACTNESS rather than about length.
  CHECK(has(tui::complete_verbs("chop gri"), "chop grid"));
}

TEST_CASE("an empty line offers everything", "[unit]") {
  // What a person pressing Tab at a bare `:` is asking for -- "what is there?"
  const tui::CompletionSet set = tui::complete_verbs("");
  CHECK(set.size() > 20);
  CHECK(has(set, "chop grid"));
  CHECK(has(set, "quit"));
}

TEST_CASE("leading blanks do not change what is offered", "[unit]") {
  const tui::CompletionSet set = tui::complete_verbs("  ch");
  CHECK(has(set, "chop grid"));

  // And the replacement starts after them, so accepting does not eat the space
  // the person typed.
  CHECK(set.replace_from == 2);
  CHECK(set.apply("  ch") == "  chop grid");
}

TEST_CASE("nothing matches an unknown word", "[unit]") {
  CHECK(tui::complete_verbs("zzz").empty());
  CHECK(tui::complete_verbs("chop zzz").empty());
}

TEST_CASE("matching is case-sensitive", "[unit]") {
  // The parser is. A prompt that offered `chop grid` for `CHOP` would be
  // promising a tolerance that disappears the moment you press Enter.
  CHECK(tui::complete_verbs("CH").empty());
}

// -- paths -------------------------------------------------------------------

TEST_CASE("a path-taking verb with an argument is a path", "[unit]") {
  const tui::PathContext context = tui::path_being_typed("load /crate/br");
  CHECK(context.is_path);
  CHECK(context.partial == "/crate/br");
  CHECK(context.replace_from == 5);
}

TEST_CASE("a verb that takes no path is not one", "[unit]") {
  CHECK(!tui::path_being_typed("chop grid 16").is_path);
  CHECK(!tui::path_being_typed("bpm 92").is_path);

  // Nor is the path verb before its space: `loa` is still being spelled, and
  // reading a directory for it would be I/O for a word that is not a word yet.
  CHECK(!tui::path_being_typed("load").is_path);
  CHECK(!tui::path_being_typed("loa").is_path);
}

TEST_CASE("a path with nothing typed after the verb is still a path", "[unit]") {
  // `:load ` and then Tab means "what is here?", the same as a bare `:` does.
  const tui::PathContext context = tui::path_being_typed("load ");
  CHECK(context.is_path);
  CHECK(context.partial.empty());
  CHECK(context.replace_from == 5);
}

TEST_CASE("a path keeps its spaces", "[unit]") {
  // THE ONE THAT WOULD HAVE BEEN A BUG. Splitting on whitespace makes
  // `~/Field Recordings/` uncompletable, in exactly the directories people keep
  // field recordings in.
  const tui::PathContext context = tui::path_being_typed("load /crate/Field Rec");
  CHECK(context.is_path);
  CHECK(context.partial == "/crate/Field Rec");
}

TEST_CASE("path candidates are filtered by what is typed and sorted", "[unit]") {
  const tui::PathContext context = tui::path_being_typed("load /crate/b");
  const std::vector<std::string> names{"/crate/vocal.wav", "/crate/break.wav", "/crate/amen.wav",
                                       "/crate/bass.wav"};

  const tui::CompletionSet set = tui::complete_paths(context, names);
  CHECK(texts(set) == std::vector<std::string>{"/crate/bass.wav", "/crate/break.wav"});

  // Sorted, unlike the verbs: a directory has no hand-order and the
  // filesystem's own is arbitrary, so an unsorted menu would differ per machine.
  CHECK(set.apply("load /crate/b") == "load /crate/bass.wav");
}

TEST_CASE("a path completion replaces only the path", "[unit]") {
  const tui::PathContext context = tui::path_being_typed("load /crate/b");
  const tui::CompletionSet set = tui::complete_paths(context, {"/crate/break.wav"});
  REQUIRE(set.size() == 1);

  // The verb survives. An append-the-tail scheme would give
  // `load /crate/b/crate/break.wav` here, which is the bug replace_from exists
  // to make impossible.
  CHECK(set.apply("load /crate/b") == "load /crate/break.wav");
}

TEST_CASE("candidates for a verb that takes no path are refused", "[unit]") {
  // complete_paths() is given a context, not a line, and a context that says
  // "not a path" must produce nothing even when handed names -- otherwise a
  // caller that forgot to check would silently offer files for `:bpm`.
  const tui::PathContext context = tui::path_being_typed("bpm 9");
  CHECK(tui::complete_paths(context, {"/crate/break.wav"}).empty());
}

// -- the table against the parser --------------------------------------------

TEST_CASE("every phrase offered is a phrase the parser understands", "[unit]") {
  // THE CHECK THAT MAKES THE DUPLICATION SAFE. The completion table is a second
  // spelling of parse_command()'s verbs and nothing keeps them in step, so this
  // walks every entry through the real parser and fails if one has been renamed
  // on one side only.
  //
  // Each phrase is completed with an argument that makes it valid where one is
  // needed -- the point is that the VERB is understood, not that a bare phrase
  // runs.
  struct Case {
    const char* phrase;
    const char* suffix;
  };

  static constexpr Case kCases[]{
      {"chop grid", " 16"},
      {"chop transient beat", ""},
      {"chop transient bar", ""},
      {"chop transient", ""},
      {"chop reset", ""},
      {"chop tune", ""},
      {"slot assign", " 1 1"},
      {"edit", " 1"},
      {"env", " 1 r 250"},
      {"pitch", " 1 +7"},
      {"reverse", " 1"},
      {"browse", ""},
      {"load", " /tmp/a.wav"},
      {"files", ""},
      {"file", " 1"},
      {"unload", " 1"},
      {"pad gate", ""},
      {"pad oneshot", ""},
      {"stop", ""},
      {"mix", ""},
      {"gain", " 1 -6"},
      {"pan", " 1 -50"},
      {"mute", " 1 on"},
      {"solo", " 1 on"},
      {"bus", " 1 a"},
      {"eq", " 1 1 off"},
      {"comp", " 1 off"},
      {"limit", " on"},
      {"bpm", " 92"},
      {"swing", " 55"},
      {"pattern", " 3"},
      {"pattern length", " 16"},
      {"pattern clear", ""},
      {"song", " 1 2"},
      {"song clear", ""},
      {"metro", " on"},
      {"perform", ""},
      {"quit", ""},
  };

  for (const Case& one : kCases) {
    const std::string line = std::string{one.phrase} + one.suffix;
    const tui::Command command = tui::parse_command(line);
    INFO("phrase: " << line);
    CHECK(command.kind != tui::CommandKind::kError);
    CHECK(command.kind != tui::CommandKind::kNone);
  }

  // And the other direction: every phrase in the table is covered above, so a
  // phrase added without a case here fails rather than going unchecked.
  const tui::CompletionSet all = tui::complete_verbs("");
  CHECK(all.size() == std::size(kCases));
  for (const tui::Completion& entry : all.entries) {
    const bool covered = std::any_of(std::begin(kCases), std::end(kCases),
                                     [&](const Case& one) { return entry.text == one.phrase; });
    INFO("phrase in the table with no parser case: " << entry.text);
    CHECK(covered);
  }
}

TEST_CASE("every phrase has a detail and neither is empty", "[unit]") {
  for (const tui::Completion& entry : tui::complete_verbs("").entries) {
    INFO("phrase: " << entry.text);
    CHECK(!entry.text.empty());
    CHECK(!entry.detail.empty());
  }
}
