// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/control_request.hpp>

#include <iostream>
#include <array>
#include <stdexcept>
#include <string>

namespace {
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
}  // namespace

int main() try {
  using headunit::ControlParseStatus;
  using headunit::parse_control_request;
  check(parse_control_request("POST /pair-setup RTSP/1.0\r\nContent-Len").status ==
            ControlParseStatus::incomplete,
        "partial header rejected");

  const std::string first =
      "POST /pair-setup RTSP/1.0\r\n"
      "Content-Length: 3\r\nCSeq: 12\r\nX-Apple-HKP: 4\r\n\r\nabc";
  const std::string second = "GET /info HTTP/1.1\r\n\r\n";
  const auto partial = parse_control_request(first.substr(0, first.size() - 1));
  check(partial.status == ControlParseStatus::incomplete && partial.request.body_bytes == 3,
        "partial body rejected or mis-sized");
  const auto pipelined = parse_control_request(first + second);
  check(pipelined.status == ControlParseStatus::complete && pipelined.request.method == "POST" &&
            pipelined.request.target == "/pair-setup" && pipelined.request.protocol == "RTSP/1.0" &&
            pipelined.request.cseq == "12" && pipelined.request.hkp == "4" &&
            pipelined.request.consumed_bytes == first.size(),
        "first pipelined request changed");
  check(parse_control_request((first + second).substr(pipelined.request.consumed_bytes))
                .request.target == "/info",
        "second pipelined request lost");

  for (const std::string bad :
       {"POST /x HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
        "POST /x HTTP/1.1\r\nContent-Length: 2x\r\n\r\n",
        "POST /x HTTP/1.1\r\nContent-Length: 65536\r\n\r\n",
        "GET /info\r\n\r\n",
        "GET /info HTTP/2.0\r\n\r\n",
        "GET /info HTTP/1.1 extra\r\n\r\n",
        "GET\n/info HTTP/1.1\r\n\r\n",
        "GET /info HTTP/1.1\r\nContent-Length: 0\r\ncontent-length: 0\r\n\r\n",
        "GET /info HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\nx",
        "GET /info HTTP/1.1\r\nCSeq: 1\r\nCSeq: 1\r\n\r\n",
        "GET /info HTTP/1.1\r\nCSeq: 4294967296\r\n\r\n",
        "GET /info HTTP/1.1\r\nCSeq: not-a-number\r\n\r\n",
        "GET /info HTTP/1.1\r\nX-Apple-HKP: 4\r\nX-Apple-HKP: 4\r\n\r\n",
        "GET /info HTTP/1.1\r\nX-Apple-HKP: 1234\r\n\r\n",
        "GET /info HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET /info HTTP/1.1\r\nContent-Length : 0\r\n\r\n",
        "GET /info HTTP/1.1\r\n CSeq: 1\r\n\r\n",
        "GET /info HTTP/1.1\r\nMissing-Colon\r\n\r\n",
        "GET /info HTTP/1.1\r\nCSeq: 1\nContent-Length: 0\r\n\r\n",
        "GET /info HTTP/1.1\r\nOpaque: value\r\r\n\r\n",
        "GET /info HTTP/1.1\r\nOpaque: value\x7f\r\n\r\n"}) {
    check(parse_control_request(bad).status == ControlParseStatus::invalid,
          "ambiguous or malformed control request accepted");
    const auto preceding = parse_control_request(first + bad);
    check(
        preceding.status == ControlParseStatus::complete &&
            preceding.request.consumed_bytes == first.size() &&
            parse_control_request((first + bad).substr(preceding.request.consumed_bytes)).status ==
                ControlParseStatus::invalid,
        "malformed successor changed preceding request framing");
  }
  const auto ignored = parse_control_request(
      "GET /info HTTP/1.1\r\nCSeq: not-a-number\r\nX-Apple-HKP: 1234\r\n\r\n");
  check(ignored.status == ControlParseStatus::invalid, "malformed numeric headers accepted");
  for (const auto duplicate : {"0", "3"}) {
    const std::string ambiguous =
        "POST /pair-setup RTSP/1.0\r\nContent-Length: 3\r\ncontent-length: " +
        std::string(duplicate) + "\r\n\r\nabc";
    check(parse_control_request(ambiguous).status == ControlParseStatus::invalid,
          "duplicate control length admitted ambiguous request framing");
  }
  check(parse_control_request("GET /info", 8).status == ControlParseStatus::invalid,
        "partial header exceeded caller's smaller input budget");
  check(parse_control_request("GET /inf", 8).status == ControlParseStatus::incomplete,
        "partial header at caller's budget was rejected prematurely");
  check(parse_control_request(std::string(8193, 'x')).status == ControlParseStatus::invalid,
        "oversized partial header accepted");
  std::string crowded = "GET /info HTTP/1.1\r\n";
  for (unsigned i = 0; i < 129; ++i)
    crowded += "Opaque: value\r\n";
  crowded += "\r\n";
  check(parse_control_request(crowded).status == ControlParseStatus::invalid,
        "header count limit ignored");
  const auto boundary =
      parse_control_request("OPTIONS * RTSP/1.0\r\nCSeq: 4294967295\r\nContent-Length: 0\r\n\r\n");
  check(boundary.status == ControlParseStatus::complete && boundary.request.cseq == "4294967295",
        "valid numeric boundary or wildcard target rejected");
  headunit::ProtocolCoverage coverage;
  const auto known = headunit::control_route_key("GET", "/info");
  check(known.category == static_cast<unsigned>(headunit::ControlRoute::info), "known route lost");
  coverage.observe(known, 0);
  coverage.result(known, 0, 200);
  coverage.observe(known, 1);
  coverage.result(known, 1, 501);
  for (unsigned i = 0; i < 49; ++i) {
    const auto key = headunit::control_route_key("POST", "/late/" + std::to_string(i));
    check(!key.category, "unknown route classified as implemented");
    coverage.observe(key, 0);
    coverage.result(key, 0, 501);
  }
  const auto recent = headunit::control_route_key("POST", "/late/48");
  coverage.observe(recent, 1);
  coverage.result(recent, 1, 400);
  std::size_t rows = 0;
  bool found = false;
  for (const auto& item : coverage.unknown())
    if (item) {
      ++rows;
      if (item->fingerprint == recent.fingerprint) {
        found = true;
        check(item->channels[0].received == 1 && item->channels[0].unsupported == 1 &&
                  item->channels[1].received == 1 && item->channels[1].bad_request == 1,
              "channel-local response coverage lost");
      }
    }
  check(rows == 32 && found && coverage.evictions() == 17 && coverage.errors() == 0 &&
            coverage.totals()[0].received == 50 && coverage.totals()[0].unsupported == 49 &&
            coverage.totals()[1].received == 2 && coverage.totals()[1].bad_request == 1 &&
            coverage.known()[*known.category][0].ok == 1 &&
            coverage.known()[*known.category][1].unsupported == 1,
        "late unknown diversity displaced known lifetime counts");
  coverage.result(headunit::control_route_key("POST", "/late/0"), 0, 200);
  coverage.result(known, 0, 200);
  coverage.observe(known, 2);
  check(coverage.errors() == 3 && coverage.known()[*known.category][0].results == 1,
        "unobserved/duplicate response manufactured coverage");
  check(headunit::protocol_fingerprint("ab", "c") != headunit::protocol_fingerprint("a", "bc") &&
            headunit::control_route_key("SETUP", "rtsp://synthetic/session").category ==
                static_cast<unsigned>(headunit::ControlRoute::setup) &&
            !headunit::control_route_key("POST", "/info").category,
        "fingerprint domains or method-specific routes collapsed");
  headunit::ProtocolCoverage successor;
  check(successor.known()[*known.category][0].received == 0 && !successor.unknown()[0],
        "coverage leaked into successor generation");
  check(headunit::control_route_key("FLUSH", "rtsp://phone/1").category ==
            static_cast<unsigned>(headunit::ControlRoute::flush),
        "FLUSH not classified");
  check(std::string(headunit::control_route_name(
            static_cast<unsigned>(headunit::ControlRoute::flush))) == "FLUSH",
        "FLUSH route name");
  const auto rtp = headunit::parse_rtp_info(
      "FLUSH rtsp://x RTSP/1.0\r\nCSeq: 9\r\nRTP-Info: seq=58176;rtptime=27958272\r\n");
  check(rtp.present && rtp.valid && rtp.info && rtp.info->seq == 58176 &&
            rtp.info->rtptime == 27958272,
        "RTP-Info resume point lost");
  const auto lower = headunit::parse_rtp_info("rtp-info: url=x; rtptime=1, seq=65535");
  check(lower.valid && lower.info && lower.info->seq == 65535 && lower.info->rtptime == 1,
        "case-insensitive RTP-Info with other fields rejected");
  const auto absent = headunit::parse_rtp_info("FLUSH * RTSP/1.0\r\nCSeq: 1");
  check(!absent.present && absent.valid && !absent.info, "absent RTP-Info invented a point");
  for (const auto* invalid :
       {"RTP-Info: seq=65536;rtptime=1", "RTP-Info: seq=1", "RTP-Info: seq=x;rtptime=1",
        "RTP-Info: seq=1;rtptime=4294967296", "RTP-Info: seq=1;seq=2;rtptime=1",
        "RTP-Info: seq=1;rtptime=1\r\nRTP-Info: seq=2;rtptime=2"}) {
    const auto parsed = headunit::parse_rtp_info(invalid);
    check(parsed.present && !parsed.valid && !parsed.info, "malformed RTP-Info accepted");
  }
  std::cout << "bounded control framing, fragmentation and pipelining passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
