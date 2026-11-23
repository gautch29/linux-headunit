// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/audio_control.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace headunit::audio_control {

struct Request {
  std::string route;
  Command command;
  bool operator==(const Request&) const = default;
};
struct State {
  std::string route;
  Snapshot control;
  std::string error;
  bool operator==(const State&) const = default;
};

// This codec validates one complete file. Route lifetime and strict sequence
// progression are enforced by the receiver; decoding never applies a command.
Request decode_request(std::string_view json);
std::string encode_state(const State&, std::uint64_t monotonic_ns);
// A missing request in a valid private directory returns nullopt. Invalid
// content/ownership/path throws; files are never followed through a symlink.
std::optional<Request> read_private_request(const std::string& path, unsigned owner_uid);
// Atomically replaces a private regular file without truncating its inode.
// New state is visible only as a complete owner-only file.
void write_private_state(const std::string& path, unsigned owner_uid, const State&,
                         std::uint64_t monotonic_ns);

}  // namespace headunit::audio_control
