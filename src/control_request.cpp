// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/control_request.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <sstream>
#include <utility>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace headunit {
namespace {
std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1);
}
bool token(std::string_view text) {
  if (text.empty()) return false;
  return std::all_of(text.begin(), text.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
  });
}
}  // namespace
std::uint64_t protocol_fingerprint(std::string_view first, std::string_view second) {
  if (first.size() > 2048 || second.size() > 2048)
    throw std::invalid_argument("oversized protocol fingerprint input");
  std::uint64_t hash = 14695981039346656037ull;
  const auto byte = [&](unsigned char value) {
    hash = (hash ^ value) * 1099511628211ull;
  };
  for (const auto part : {first, second}) {
    // Length domains prevent ambiguous method/target concatenation.
    const auto length = static_cast<std::uint64_t>(part.size());
    for (unsigned i = 0; i < 8; ++i)
      byte(static_cast<unsigned char>(length >> (8 * i)));
    for (const unsigned char value : part)
      byte(value);
  }
  return hash;
}
ProtocolCoverageKey control_route_key(std::string_view method, std::string_view target) {
  const auto known = [](ControlRoute route) {
    return ProtocolCoverageKey{static_cast<unsigned>(route), 0};
  };
  if (method == "GET" && target == "/info") return known(ControlRoute::info);
  if (method == "POST") {
    if (target == "/pair-setup") return known(ControlRoute::pair_setup);
    if (target == "/pair-verify") return known(ControlRoute::pair_verify);
    if (target == "/auth-setup") return known(ControlRoute::auth_setup);
    if (target == "/command") return known(ControlRoute::command);
    if (target == "/feedback") return known(ControlRoute::feedback);
  }
  if (method == "SETUP") return known(ControlRoute::setup);
  if (method == "RECORD") return known(ControlRoute::record);
  if (method == "TEARDOWN") return known(ControlRoute::teardown);
  if (method == "OPTIONS") return known(ControlRoute::options);
  if (method == "FLUSH") return known(ControlRoute::flush);
  return {{}, protocol_fingerprint(method, target)};
}
const char* control_route_name(unsigned category) {
  constexpr std::array names{"info",    "pair-setup", "pair-verify",     "auth-setup",
                             "SETUP",   "RECORD",     "TEARDOWN",        "OPTIONS",
                             "command", "feedback",   "invalid-framing", "FLUSH"};
  return category < names.size() ? names[category] : "unknown-category";
}
namespace {
void increment(std::uint64_t& value) noexcept {
  if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}
}  // namespace
ProtocolCoverage::Counts* ProtocolCoverage::entry(ProtocolCoverageKey key, unsigned channel,
                                                  bool create) noexcept {
  if (channel >= 2 || (key.category && *key.category >= max_known)) {
    increment(errors_);
    return nullptr;
  }
  if (key.category) return &known_[*key.category][channel];
  for (auto& item : unknown_)
    if (item && item->fingerprint == key.fingerprint) return &item->channels[channel];
  if (!create) {
    increment(errors_);
    return nullptr;
  }
  auto& item = unknown_[next_unknown_];
  if (item) increment(evictions_);
  item = Unknown{key.fingerprint, {}};
  next_unknown_ = (next_unknown_ + 1) % max_unknown;
  return &item->channels[channel];
}
void ProtocolCoverage::observe(ProtocolCoverageKey key, unsigned channel) noexcept {
  if (auto* counts = entry(key, channel, true)) {
    increment(counts->received);
    increment(totals_[channel].received);
  }
}
void ProtocolCoverage::result(ProtocolCoverageKey key, unsigned channel, unsigned status) noexcept {
  if (auto* counts = entry(key, channel, false)) {
    if (counts->results == counts->received) {
      increment(errors_);
      return;
    }
    const auto respond = [status](Counts& count) {
      increment(count.results);
      if (status >= 200 && status < 300)
        increment(count.ok);
      else if (status == 400)
        increment(count.bad_request);
      else if (status == 501)
        increment(count.unsupported);
      else
        increment(count.other);
    };
    respond(*counts);
    respond(totals_[channel]);
  }
}

bool valid_control_start_line(std::string_view line) {
  return !line.empty() && line.size() <= 8192 &&
         std::all_of(line.begin(), line.end(), [](unsigned char c) { return c >= 32 && c != 127; });
}
bool valid_control_request_line(std::string_view method, std::string_view target,
                                std::string_view protocol) {
  return method.size() <= 32 && token(method) && !target.empty() && target.size() <= 2048 &&
         std::all_of(target.begin(), target.end(),
                     [](unsigned char c) { return c > 32 && c < 127; }) &&
         (protocol == "RTSP/1.0" || protocol == "HTTP/1.1");
}

std::optional<ControlHeaders> parse_control_headers(std::string_view block, std::size_t maximum) {
  if (block.size() > 8192) return {};
  ControlHeaders headers;
  bool has_length = false, has_cseq = false, has_hkp = false;
  unsigned count = 0;
  std::istringstream lines{std::string(block)};
  std::string line;
  while (std::getline(lines, line)) {
    if (++count > 128) return {};
    if (!lines.eof()) {
      if (line.empty() || line.back() != '\r') return {};
      line.pop_back();
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos || !token(std::string_view(line).substr(0, colon))) return {};
    if (std::any_of(line.begin() + colon + 1, line.end(),
                    [](unsigned char c) { return (c < 32 && c != '\t') || c == 127; }))
      return {};
    auto key = line.substr(0, colon);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto value = trim(line.substr(colon + 1));
    if (key == "transfer-encoding") return {};  // Cannot delimit this unsupported wire format.
    if (key == "content-length") {
      const auto [last, error] =
          std::from_chars(value.data(), value.data() + value.size(), headers.body_bytes);
      if (has_length || error != std::errc{} || last != value.data() + value.size() ||
          headers.body_bytes > maximum)
        return {};
      has_length = true;
    } else if (key == "cseq") {
      std::uint32_t sequence = 0;
      const auto [last, error] =
          std::from_chars(value.data(), value.data() + value.size(), sequence);
      if (has_cseq || value.size() > 10 || error != std::errc{} ||
          last != value.data() + value.size())
        return {};
      headers.cseq = value;
      has_cseq = true;
    } else if (key == "x-apple-hkp") {
      if (has_hkp || value.empty() || value.size() > 3 ||
          !std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return c >= '0' && c <= '9'; }))
        return {};
      headers.hkp = value;
      has_hkp = true;
    }
  }
  return headers;
}

ControlParseResult parse_control_request(std::string_view buffer, std::size_t maximum_bytes) {
  constexpr std::size_t maximum_header_bytes = 8192;
  const auto end = buffer.find("\r\n\r\n");
  if (end == std::string_view::npos)
    return {buffer.size() <= std::min(maximum_header_bytes, maximum_bytes)
                ? ControlParseStatus::incomplete
                : ControlParseStatus::invalid,
            {}};
  if (end > maximum_header_bytes || end + 4 > maximum_bytes)
    return {ControlParseStatus::invalid, {}};

  ControlRequestHead request;
  const auto first_end = buffer.find("\r\n");
  const auto first_line = buffer.substr(0, first_end);
  if (!valid_control_start_line(first_line)) return {ControlParseStatus::invalid, {}};
  std::istringstream request_line{std::string(first_line)};
  std::string extra;
  if (!(request_line >> request.method >> request.target >> request.protocol) ||
      request_line >> extra ||
      !valid_control_request_line(request.method, request.target, request.protocol))
    return {ControlParseStatus::invalid, {}};
  request.header_bytes = end + 4;
  const auto headers = parse_control_headers(
      first_end == end ? std::string_view{} : buffer.substr(first_end + 2, end - first_end - 2),
      maximum_bytes - request.header_bytes);
  if (!headers) return {ControlParseStatus::invalid, {}};
  request.body_bytes = headers->body_bytes;
  request.cseq = headers->cseq;
  request.hkp = headers->hkp;
  request.consumed_bytes = request.header_bytes + request.body_bytes;
  return {buffer.size() < request.consumed_bytes ? ControlParseStatus::incomplete
                                                 : ControlParseStatus::complete,
          std::move(request)};
}

RtpInfoParse parse_rtp_info(std::string_view header_lines) {
  RtpInfoParse result;
  const auto number = [](std::string_view text,
                         std::uint64_t maximum) -> std::optional<std::uint64_t> {
    if (text.empty() || text.size() > 10) return std::nullopt;
    std::uint64_t value = 0;
    for (const char c : text) {
      if (c < '0' || c > '9') return std::nullopt;
      value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value > maximum) return std::nullopt;
    return value;
  };
  constexpr std::string_view name = "rtp-info:";
  while (!header_lines.empty()) {
    const auto end = header_lines.find("\r\n");
    const auto line = header_lines.substr(0, end);
    header_lines =
        end == std::string_view::npos ? std::string_view{} : header_lines.substr(end + 2);
    if (line.size() < name.size()) continue;
    bool matches = true;
    for (std::size_t i = 0; i < name.size(); ++i) {
      const char c = line[i];
      const char lower = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
      matches = matches && lower == name[i];
    }
    if (!matches) continue;
    if (result.present) {
      result.valid = false;
      result.info.reset();
      return result;
    }
    result.present = true;
    auto value = line.substr(name.size());
    std::optional<std::uint64_t> seq, rtptime;
    while (!value.empty()) {
      const auto separator = value.find_first_of(";,");
      auto field = value.substr(0, separator);
      value =
          separator == std::string_view::npos ? std::string_view{} : value.substr(separator + 1);
      while (!field.empty() && field.front() == ' ')
        field.remove_prefix(1);
      while (!field.empty() && field.back() == ' ')
        field.remove_suffix(1);
      if (field.starts_with("seq=")) {
        if (seq) result.valid = false;
        seq = number(field.substr(4), 0xffff);
        if (!seq) result.valid = false;
      } else if (field.starts_with("rtptime=")) {
        if (rtptime) result.valid = false;
        rtptime = number(field.substr(8), 0xffffffff);
        if (!rtptime) result.valid = false;
      }
    }
    if (result.valid && seq && rtptime)
      result.info = RtpInfo{static_cast<std::uint16_t>(*seq), static_cast<std::uint32_t>(*rtptime)};
    else
      result.valid = false;
  }
  return result;
}

}  // namespace headunit
