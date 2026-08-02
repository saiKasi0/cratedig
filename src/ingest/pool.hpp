#ifndef CRATEDIG_INGEST_POOL_HPP
#define CRATEDIG_INGEST_POOL_HPP

#include "ingest/peak_pyramid.hpp"
#include "ingest/slices.hpp"
#include "rt/sample.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ingest {

// The crate: every file this session has loaded, and everything derived from it.
//
// This is the assumption M5.5 removes. Until now `cratedig <file>` loaded exactly
// one file for the life of the session and every pad was a slice of it, so the
// interface could keep `sample`, `slices` and `pyramid` as three locals and a pad
// could name a slice with a single index.
//
// CONTROL THREAD ONLY. Nothing here is reachable from the audio thread and
// nothing here needs to be: `rt::PadConfig` already carries its own
// `shared_ptr<const rt::Sample>`, so a pad playing a slice of one file while its
// neighbour plays another is a fact about what was published, not a change to
// the callback. That is why this milestone is a control-side model plus a UI.
//
// SHAPED FOR M6 TO SERIALISE, deliberately, the way `rt::SequencerState` already
// is: plain owned data, no back-pointers, no handles into anything that has to
// still exist. M6's project file should consume this rather than inventing a
// second model of "what is loaded" (docs/ROADMAP.md notes the overlap).

// A file's identity, stable for the life of the session.
//
// NOT AN INDEX INTO THE POOL, and that is the whole point. A pad names a file,
// and if identity were positional then unloading anything would silently
// re-point every pad after it at the wrong material -- the kind of bug that
// looks like a corrupt project rather than like a bad index. Ids are handed out
// monotonically and never reused, so a stale one is *absent* rather than wrong.
enum class FileId : std::uint32_t {};

inline constexpr FileId kNoFile{0};

// One loaded file, and everything computed from it.
struct PoolEntry {
  FileId id = kNoFile;

  // The audio. A shared_ptr because pads hold one too -- see `remove()`.
  std::shared_ptr<const rt::Sample> sample;

  // Where it came from, and what to call it on screen. Both, because two files
  // in different directories can have the same name and the browser has to be
  // able to tell them apart while the pad grid has room only for the short one.
  std::filesystem::path path;
  std::string name;

  // This file's own chops. Each file carries its own, which is the other half of
  // the one-file assumption: `:chop` used to mean "the chop" and now means "this
  // file's chop".
  SliceSet slices;

  // For drawing it. Built at load, which is where the cost belongs.
  PeakPyramid pyramid;
};

class SamplePool {
 public:
  static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

  // Adds a file and returns its id.
  //
  // A path already in the pool is NOT decoded twice: the existing id comes back
  // and nothing is replaced. `:load` on something already loaded is a way of
  // saying "bring that up", not a request to spend three seconds re-decoding it,
  // and a second copy of the same audio would double the memory for nothing.
  //
  // The consequence, stated because it is a real limitation rather than an
  // oversight: a file edited on disk after loading is not noticed. Re-reading it
  // needs a reload verb, which nothing asks for yet.
  FileId add(std::shared_ptr<const rt::Sample> sample, std::filesystem::path path, SliceSet slices,
             PeakPyramid pyramid);

  // Drops a file from the pool.
  //
  // PADS PLAYING IT KEEP PLAYING, and voices sounding from it keep sounding.
  // Every one of them holds its own `shared_ptr` to the `rt::Sample` through its
  // `rt::PadConfig`, so removing the entry drops the pool's reference and no
  // more; the audio outlives the crate slot exactly as long as something is
  // still using it. Nothing has to be published, nothing has to be stopped, and
  // the audio thread never learns that this happened.
  //
  // Returns false if the id was not in the pool.
  bool remove(FileId id);

  [[nodiscard]] const PoolEntry* find(FileId id) const;
  [[nodiscard]] PoolEntry* find(FileId id);

  // The id of a path already loaded, or kNoFile.
  [[nodiscard]] FileId find_path(const std::filesystem::path& path) const;

  // Position in the listing, for a UI that draws them in order. kNotFound if the
  // id is gone -- and callers must treat that as ordinary rather than as an
  // error, because a pad can outlive the pool entry it names.
  [[nodiscard]] std::size_t index_of(FileId id) const;

  [[nodiscard]] const std::vector<PoolEntry>& entries() const noexcept { return m_entries; }

  [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }

  [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }

  // Total decoded audio held by the pool, in frames across all channels.
  //
  // Here because it is the number that decides whether a crate is a good idea on
  // a given machine: float audio is four bytes a frame a channel, so a
  // five-minute stereo file at 48 kHz is about 115 MB and a pool of them adds
  // up quickly. The interface can show it; nothing enforces a limit yet.
  [[nodiscard]] std::size_t total_frames() const noexcept;

 private:
  std::vector<PoolEntry> m_entries;

  // Never reset and never reused, so an id that has been removed stays absent
  // rather than coming back attached to a different file. Starts at 1 because
  // kNoFile is 0 -- a default-constructed FileId must not name the first thing
  // anybody loads.
  std::uint32_t m_next_id = 1;
};

}  // namespace ingest

#endif  // CRATEDIG_INGEST_POOL_HPP
