#include "ingest/pool.hpp"

#include "ingest/peak_pyramid.hpp"
#include "ingest/slices.hpp"
#include "rt/sample.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <utility>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint32_t kRate = 48'000;

[[nodiscard]] std::shared_ptr<const rt::Sample> make_sample(std::size_t frames = 1'000,
                                                            std::uint16_t channels = 1) {
  auto sample = std::make_shared<rt::Sample>(kRate, channels, frames);
  for (std::uint16_t channel = 0; channel < channels; ++channel) {
    std::span<float> data = sample->mutable_channel(channel);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      data[frame] = static_cast<float>((frame + channel) % 100) / 100.0F;
    }
  }
  return sample;
}

[[nodiscard]] ingest::SliceSet make_slices(std::size_t count, std::size_t frames = 1'000) {
  ingest::SliceSet set;
  for (std::size_t index = 0; index < count; ++index) {
    set.slices.push_back(ingest::Slice{.start_frame = index * (frames / count),
                                       .end_frame = (index + 1) * (frames / count)});
  }
  return set;
}

[[nodiscard]] ingest::FileId add(ingest::SamplePool& pool, const char* path,
                                 std::size_t slice_count = 4) {
  auto sample = make_sample();
  ingest::PeakPyramid pyramid = ingest::PeakPyramid::build(*sample);
  return pool.add(std::move(sample), std::filesystem::path{path}, make_slices(slice_count),
                  std::move(pyramid));
}

}  // namespace

TEST_CASE("a fresh pool is empty", "[unit]") {
  const ingest::SamplePool pool;
  CHECK(pool.empty());
  CHECK(pool.size() == 0);
  CHECK(pool.total_frames() == 0);
  CHECK(pool.find(ingest::kNoFile) == nullptr);

  // A default-constructed FileId names nothing. Ids start at 1 precisely so that
  // a zero-initialised field cannot accidentally name the first file loaded --
  // which for a struct M6 serialises would be a corrupt project that looked
  // valid.
  CHECK(pool.index_of(ingest::kNoFile) == ingest::SamplePool::kNotFound);
}

TEST_CASE("the pool carries each file's own slices", "[unit]") {
  // The one-file assumption, removed: `:chop` used to mean "the chop" and now
  // means "this file's chop".
  ingest::SamplePool pool;
  const ingest::FileId first = add(pool, "/crate/break.wav", 8);
  const ingest::FileId second = add(pool, "/crate/vocal.wav", 3);

  REQUIRE(pool.size() == 2);
  REQUIRE(first != second);

  const ingest::PoolEntry* one = pool.find(first);
  const ingest::PoolEntry* two = pool.find(second);
  REQUIRE(one != nullptr);
  REQUIRE(two != nullptr);

  CHECK(one->slices.size() == 8);
  CHECK(two->slices.size() == 3);
  CHECK(one->name == "break.wav");
  CHECK(two->name == "vocal.wav");
  CHECK(one->sample != two->sample);
}

TEST_CASE("an id is stable when something before it is removed", "[unit]") {
  // THE REASON IDENTITY IS NOT AN INDEX. A pad names a file; if that name were
  // positional, unloading anything would silently re-point every pad after it at
  // the wrong material -- a bug that reads as a corrupt project rather than as a
  // bad index.
  ingest::SamplePool pool;
  const ingest::FileId first = add(pool, "/crate/a.wav");
  const ingest::FileId second = add(pool, "/crate/b.wav");
  const ingest::FileId third = add(pool, "/crate/c.wav");

  CHECK(pool.index_of(third) == 2);
  REQUIRE(pool.remove(first));

  // The id still finds the same file, and it is the same file.
  const ingest::PoolEntry* still = pool.find(third);
  REQUIRE(still != nullptr);
  CHECK(still->name == "c.wav");

  // Its POSITION moved, which is what a listing cares about and what a pad must
  // never have been holding.
  CHECK(pool.index_of(third) == 1);
  CHECK(pool.index_of(second) == 0);

  // And the removed id is absent rather than pointing at whatever slid into its
  // place.
  CHECK(pool.find(first) == nullptr);
  CHECK(pool.index_of(first) == ingest::SamplePool::kNotFound);
}

TEST_CASE("a removed id is never reissued", "[unit]") {
  // Absent rather than wrong. If ids were reused, a pad holding a stale one
  // would come back attached to a different file -- silently playing the wrong
  // sound, which is worse than playing nothing.
  ingest::SamplePool pool;
  const ingest::FileId first = add(pool, "/crate/a.wav");
  REQUIRE(pool.remove(first));

  const ingest::FileId second = add(pool, "/crate/b.wav");
  CHECK(second != first);
  CHECK(pool.find(first) == nullptr);

  // Even after many rounds.
  for (int round = 0; round < 8; ++round) {
    const ingest::FileId id = add(pool, "/crate/tmp.wav");
    CHECK(id != first);
    REQUIRE(pool.remove(id));
  }
}

TEST_CASE("loading the same path twice does not decode it twice", "[unit]") {
  ingest::SamplePool pool;
  const ingest::FileId first = add(pool, "/crate/break.wav", 8);

  // A second add of the same path returns the SAME id and leaves the entry
  // alone -- including its slices, which is the part that would hurt: `:load` on
  // something already open must not throw away the chop you made of it.
  auto other = make_sample();
  ingest::PeakPyramid pyramid = ingest::PeakPyramid::build(*other);
  const ingest::FileId again = pool.add(std::move(other), std::filesystem::path{"/crate/break.wav"},
                                        make_slices(2), std::move(pyramid));

  CHECK(again == first);
  CHECK(pool.size() == 1);
  const ingest::PoolEntry* entry = pool.find(first);
  REQUIRE(entry != nullptr);
  CHECK(entry->slices.size() == 8);
}

TEST_CASE("removing a file leaves anything still playing it alone", "[unit]") {
  // The property that makes `remove()` safe to call while the stream is running,
  // and the reason nothing has to be published or stopped: a pad holds its own
  // shared_ptr to the rt::Sample through its rt::PadConfig, so dropping the
  // pool's entry drops the pool's reference and no more.
  ingest::SamplePool pool;
  auto sample = make_sample();
  const auto* raw = sample.get();
  ingest::PeakPyramid pyramid = ingest::PeakPyramid::build(*sample);

  // What a pad would be holding.
  std::shared_ptr<const rt::Sample> held = sample;

  const ingest::FileId id = pool.add(std::move(sample), std::filesystem::path{"/crate/a.wav"},
                                     make_slices(4), std::move(pyramid));
  REQUIRE(id != ingest::kNoFile);

  REQUIRE(pool.remove(id));
  CHECK(pool.empty());

  // The audio is still there, still the same object, still readable.
  REQUIRE(held != nullptr);
  CHECK(held.get() == raw);
  CHECK(held->num_frames() == 1'000);
  CHECK(held.use_count() == 1);  // the pool really did let go
}

TEST_CASE("a null sample is refused rather than stored", "[unit]") {
  // A pool entry with no audio would be a file that draws, lists and cannot be
  // played -- and every consumer would need a null check the type system was
  // supposed to remove.
  ingest::SamplePool pool;
  CHECK(pool.add(nullptr, std::filesystem::path{"/crate/a.wav"}, ingest::SliceSet{},
                 ingest::PeakPyramid{}) == ingest::kNoFile);
  CHECK(pool.empty());
}

TEST_CASE("the pool reports how much audio it is holding", "[unit]") {
  // Frames across all channels, because that is the number that decides whether
  // a crate fits in memory: four bytes each, so a five-minute stereo file at
  // 48 kHz is about 115 MB.
  ingest::SamplePool pool;
  CHECK(pool.total_frames() == 0);

  // THE ADD IS OUTSIDE THE MACRO, and that is not style. Catch2's REQUIRE can
  // evaluate its expression a second time to build the failure message, so a
  // `std::move` inside one reads a moved-from object on exactly the path where
  // something has already gone wrong. clang-tidy's bugprone-use-after-move found
  // it; it had been here since the pool landed.
  auto mono = make_sample(1'000, 1);
  ingest::PeakPyramid mono_pyramid = ingest::PeakPyramid::build(*mono);
  const ingest::FileId mono_id = pool.add(std::move(mono), std::filesystem::path{"/a.wav"},
                                          ingest::SliceSet{}, std::move(mono_pyramid));
  REQUIRE(mono_id != ingest::kNoFile);
  CHECK(pool.total_frames() == 1'000);

  auto stereo = make_sample(500, 2);
  ingest::PeakPyramid stereo_pyramid = ingest::PeakPyramid::build(*stereo);
  const ingest::FileId stereo_id = pool.add(std::move(stereo), std::filesystem::path{"/b.wav"},
                                            ingest::SliceSet{}, std::move(stereo_pyramid));
  REQUIRE(stereo_id != ingest::kNoFile);
  CHECK(pool.total_frames() == 2'000);  // 1000 mono + 500 x 2
}

TEST_CASE("a path is matched as written, not resolved", "[unit]") {
  ingest::SamplePool pool;
  const ingest::FileId id = add(pool, "/crate/a.wav");

  CHECK(pool.find_path(std::filesystem::path{"/crate/a.wav"}) == id);
  CHECK(pool.find_path(std::filesystem::path{"/crate/b.wav"}) == ingest::kNoFile);

  // Two spellings of one path load twice. Deliberate: canonicalising needs the
  // file to still exist, and a pool entry outliving its file on disk is normal
  // once the audio is decoded and in memory. Loading twice costs memory and
  // confuses nobody; a lookup that failed on a renamed file would confuse
  // everybody.
  CHECK(pool.find_path(std::filesystem::path{"/crate/./a.wav"}) == ingest::kNoFile);
}
