#include "ingest/pool.hpp"

#include <algorithm>
#include <utility>

namespace ingest {

FileId SamplePool::add(std::shared_ptr<const rt::Sample> sample, std::filesystem::path path,
                       SliceSet slices, PeakPyramid pyramid) {
  if (sample == nullptr) {
    return kNoFile;
  }

  // Already loaded: hand back what is there rather than decoding a second copy.
  if (const FileId existing = find_path(path); existing != kNoFile) {
    return existing;
  }

  PoolEntry entry;
  entry.id = static_cast<FileId>(m_next_id++);
  entry.sample = std::move(sample);

  // The display name before the path is moved from, which is the ordering bug
  // this line exists to not have: taking `filename()` off a moved-from path
  // gives an empty string and a crate full of unnamed files.
  entry.name = path.filename().string();
  entry.path = std::move(path);

  entry.slices = std::move(slices);
  entry.pyramid = std::move(pyramid);

  const FileId id = entry.id;
  m_entries.push_back(std::move(entry));
  return id;
}

bool SamplePool::remove(FileId id) {
  const auto found = std::find_if(m_entries.begin(), m_entries.end(),
                                  [id](const PoolEntry& entry) { return entry.id == id; });
  if (found == m_entries.end()) {
    return false;
  }
  m_entries.erase(found);
  return true;
}

const PoolEntry* SamplePool::find(FileId id) const {
  const auto found = std::find_if(m_entries.begin(), m_entries.end(),
                                  [id](const PoolEntry& entry) { return entry.id == id; });
  return found == m_entries.end() ? nullptr : &*found;
}

PoolEntry* SamplePool::find(FileId id) {
  const auto found = std::find_if(m_entries.begin(), m_entries.end(),
                                  [id](const PoolEntry& entry) { return entry.id == id; });
  return found == m_entries.end() ? nullptr : &*found;
}

FileId SamplePool::find_path(const std::filesystem::path& path) const {
  // Compared as written rather than canonicalised. Resolving symlinks and `..`
  // would need the file to still exist, and a pool entry outliving its file on
  // disk is normal -- the audio is already decoded and in memory. Two spellings
  // of one path therefore load twice, which costs memory and confuses nobody;
  // canonicalising would make `find_path` fail on a file that had been renamed
  // since, which confuses everybody.
  const auto found = std::find_if(m_entries.begin(), m_entries.end(),
                                  [&path](const PoolEntry& entry) { return entry.path == path; });
  return found == m_entries.end() ? kNoFile : found->id;
}

std::size_t SamplePool::index_of(FileId id) const {
  for (std::size_t index = 0; index < m_entries.size(); ++index) {
    if (m_entries[index].id == id) {
      return index;
    }
  }
  return kNotFound;
}

std::size_t SamplePool::total_frames() const noexcept {
  std::size_t total = 0;
  for (const PoolEntry& entry : m_entries) {
    if (entry.sample != nullptr) {
      total += entry.sample->num_frames() * entry.sample->num_channels();
    }
  }
  return total;
}

}  // namespace ingest
