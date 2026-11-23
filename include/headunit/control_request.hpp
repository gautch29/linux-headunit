// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <string_view>
#include <optional>

namespace headunit {

// Numeric-only receiver-thread evidence. Fingerprints distinguish bounded unknown
// protocol labels; they are not semantic IDs or cryptographic identities. A hash
// collision can merge counts. Never retain methods, URLs, parameters or bodies.
// Results are the owning boundary's status observations, never applied state or
// presentation. Main reports after socket write; command/event report dispatch.
struct ProtocolCoverageKey {
  std::optional<unsigned> category;
  std::uint64_t fingerprint = 0;
};
std::uint64_t protocol_fingerprint(std::string_view first, std::string_view second = {});
enum class ControlRoute {
  info,
  pair_setup,
  pair_verify,
  auth_setup,
  setup,
  record,
  teardown,
  options,
  command,
  feedback,
  invalid_framing,
  flush
};
ProtocolCoverageKey control_route_key(std::string_view method, std::string_view target);
const char* control_route_name(unsigned category);
class ProtocolCoverage {
 public:
  struct Counts {
    std::uint64_t received = 0, results = 0, ok = 0, bad_request = 0, unsupported = 0, other = 0;
    bool operator==(const Counts&) const = default;
  };
  struct Unknown {
    std::uint64_t fingerprint;
    std::array<Counts, 2> channels;
  };
  static constexpr std::size_t max_known = 16, max_unknown = 32;
  void observe(ProtocolCoverageKey, unsigned channel) noexcept;
  void result(ProtocolCoverageKey, unsigned channel, unsigned status) noexcept;
  const auto& totals() const noexcept { return totals_; }
  const auto& known() const noexcept { return known_; }
  const auto& unknown() const noexcept { return unknown_; }
  std::uint64_t evictions() const noexcept { return evictions_; }
  std::uint64_t errors() const noexcept { return errors_; }

 private:
  Counts* entry(ProtocolCoverageKey, unsigned channel, bool create) noexcept;
  std::array<Counts, 2> totals_{};
  std::array<std::array<Counts, 2>, max_known> known_{};
  std::array<std::optional<Unknown>, max_unknown> unknown_{};
  std::size_t next_unknown_ = 0;
  std::uint64_t evictions_ = 0, errors_ = 0;
};

enum class ControlParseStatus { incomplete, complete, invalid };

// Framing only: authentication, decryption, routing and responses belong to
// the receiver session. A complete request may be followed by another one in
// the input buffer; consumed_bytes identifies exactly the first request.
struct ControlRequestHead {
  std::string method, target, protocol, cseq, hkp;
  std::size_t header_bytes = 0;
  std::size_t body_bytes = 0;
  std::size_t consumed_bytes = 0;
};
struct ControlParseResult {
  ControlParseStatus status = ControlParseStatus::incomplete;
  ControlRequestHead request;
};
struct ControlHeaders {
  std::size_t body_bytes = 0;
  std::string cseq, hkp;
};
// Main and event channels share exact request-line/header framing. No chunked
// transfer, folded/malformed fields, duplicate length/CSeq/HKP or control bytes.
bool valid_control_start_line(std::string_view line);
bool valid_control_request_line(std::string_view method, std::string_view target,
                                std::string_view protocol);
std::optional<ControlHeaders> parse_control_headers(std::string_view header_lines,
                                                    std::size_t maximum_body_bytes);

ControlParseResult parse_control_request(std::string_view buffer,
                                         std::size_t maximum_bytes = 65536);

// RTSP FLUSH resume point from an `RTP-Info: seq=N;rtptime=T` header. Absent
// means "flush now"; a present but malformed or duplicated header is invalid.
struct RtpInfo {
  std::uint16_t seq = 0;
  std::uint32_t rtptime = 0;
};
struct RtpInfoParse {
  bool present = false, valid = true;
  std::optional<RtpInfo> info;
};
RtpInfoParse parse_rtp_info(std::string_view header_lines);

}  // namespace headunit
