#ifndef CRATEDIG_TUI_COMPLETION_HPP
#define CRATEDIG_TUI_COMPLETION_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tui {

// What the `:` prompt can finish for you, and what each thing does.
//
// A PURE FUNCTION OF THE TYPED LINE, deliberately, the way parse_command() is.
// The menu is the only part of the prompt anybody sees, but the part worth
// testing is the choosing -- which phrases match `sl`, where the replacement
// starts, what a second Tab does. None of that needs a terminal, so none of it
// lives in app.cpp.
//
// THE ONE THING THIS FILE DOES NOT DO IS TOUCH THE FILESYSTEM. Path completion
// needs a directory listing, and a directory listing is I/O; it stays on the
// control thread in app.cpp beside read_directory(), which already does exactly
// this. What lives here is the pure half: deciding that a path IS what is being
// typed (path_being_typed) and turning a list of names into entries
// (complete_paths). Same split as BROWSE: the renderer never reads a disk.

// One offer: what typing it would produce, and what it does.
struct Completion {
  // The WHOLE phrase, not the missing tail -- "chop grid" rather than "op grid"
  // when `ch` is typed. Accepting it replaces from `replace_from` rather than
  // appending, so the two cannot drift apart the way an append-the-tail scheme
  // does the moment a match is not a prefix.
  std::string text;

  // The right-hand column: what it does, in the fewest words that distinguish
  // it from its neighbours. Empty for paths, which describe themselves.
  std::string detail;
};

struct CompletionSet {
  std::vector<Completion> entries;

  // Which one is selected. Always in range when `entries` is non-empty.
  std::size_t cursor = 0;

  // Where in the typed line the selected text goes. Everything from here to the
  // end of the line is replaced.
  //
  // Carried rather than recomputed because the caller cannot derive it: for a
  // verb it is 0, for `chop gr` it is 0 as well (the whole phrase is offered),
  // and for `load /crate/br` it is the index just past `load `. One number
  // instead of three rules at the call site.
  std::size_t replace_from = 0;

  [[nodiscard]] bool empty() const noexcept { return entries.empty(); }

  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }

  // The line that accepting the selection would produce, given the line typed.
  //
  // Here rather than in the caller because the replacement rule and
  // `replace_from` have to agree, and a rule kept next to the number it uses
  // cannot disagree with it.
  [[nodiscard]] std::string apply(std::string_view typed) const;
};

// Verb and sub-verb completion for a typed line.
//
// Matching is by PREFIX and case-sensitively, because every verb here is
// lowercase and a prompt that quietly accepted `CHOP` would be claiming a
// tolerance the parser does not have.
//
// Returns an empty set for a line that is already a complete phrase with an
// argument after it (`chop grid 16`): there is nothing left to offer, and a
// menu that stayed open over a finished command would hide the thing it was
// helping you write.
[[nodiscard]] CompletionSet complete_verbs(std::string_view typed);

// The partial path being typed, if one is -- `load /crate/br` gives `/crate/br`.
//
// std::nullopt-shaped as an empty optional would be, but expressed as a bool
// plus an out-string so callers can ask and use in one step. `false` means the
// line is not a path-taking verb followed by an argument, which is the common
// case and the one that must not cost a directory read.
struct PathContext {
  bool is_path = false;

  // What has been typed of the path. May be empty (`load ` with nothing after).
  std::string partial;

  // Where `partial` starts in the typed line -- the `replace_from` for the set
  // the caller builds.
  std::size_t replace_from = 0;
};

[[nodiscard]] PathContext path_being_typed(std::string_view typed);

// Turns candidate names into a set, keeping those that extend `context.partial`.
//
// `names` are full paths as they would be typed. The caller has already read
// the directory; this is the pure half that filters and orders them, so the
// ordering is testable without a filesystem.
[[nodiscard]] CompletionSet complete_paths(const PathContext& context,
                                           const std::vector<std::string>& names);

}  // namespace tui

#endif  // CRATEDIG_TUI_COMPLETION_HPP
