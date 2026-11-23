// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace headunit::theme::script {
// One bounded, coalescing disk owner. VM callbacks copy records and read cached
// outcomes; open/read/write/fsync run only on this worker. Retirement drains the
// last accepted write before the owner exits.
class PreferenceFile {
 public:
  struct Snapshot {
    bool loaded = false;
    std::optional<std::string> data;
    std::string error;
    std::uint64_t completed = 0;
  };
  explicit PreferenceFile(std::string path, std::size_t maximum_bytes = 102);
  ~PreferenceFile();
  PreferenceFile(const PreferenceFile&) = delete;
  PreferenceFile& operator=(const PreferenceFile&) = delete;
  Snapshot snapshot() const;
  std::uint64_t save(std::string record);
  // Blocking commissioning/test checkpoint. Never call from a VM/control tick.
  void wait();

 private:
  void run();
  std::string path_;
  std::size_t maximum_bytes_;
  mutable std::mutex mutex_;
  std::condition_variable wake_, idle_;
  Snapshot state_;
  std::optional<std::pair<std::uint64_t, std::string>> pending_;
  std::uint64_t sequence_ = 0;
  bool stopping_ = false, busy_ = true;
  std::thread worker_;
};
}  // namespace headunit::theme::script
