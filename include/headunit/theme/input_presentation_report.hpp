// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/native_input.hpp>
#include <iomanip>
#include <ostream>

namespace headunit::theme::video {
// Called only at the existing finite report cadence, never from completion.
// Integer identities/dates use decimal strings to preserve all uint64 bits.
inline void write_input_presentation(std::ostream& out,
                                     const std::optional<NativeInputPresentation>& value) {
  out << " videoPresentation=";
  if (!value) {
    out << "null";
    return;
  }
  const auto precision = out.precision();
  out << std::setprecision(17);
  const auto& p = *value;
  const auto& r = p.receipt;
  const auto& f = r.feedback;
  out << R"({"authority":"gpu-display-native-input","hostClock":"CLOCK_MONOTONIC","streamToken":")"
      << p.stream_token << R"(","ownerGeneration":")" << p.owner_generation
      << R"(","ownerConnectionId":)";
  if (p.owner_connection_id)
    out << '"' << *p.owner_connection_id << '"';
  else
    out << "null";
  out << R"(,"sourcePtsSeconds":)" << p.source_pts << R"(,"submissionToken":")" << f.token
      << R"(","submissionEpoch":")" << r.submission_epoch << R"(","currentEpoch":")"
      << r.current_epoch << R"(","submittedMonoNs":")" << r.submitted_ns
      << R"(","presentedMonoNs":")" << r.mapped.monotonic_ns << R"(","pollMonoNs":")"
      << f.received_ns << R"(","mappedMediaNtp":")" << r.mapped.media_ntp
      << R"(","mappedPtsSeconds":)" << r.mapped.pts_seconds << R"(,"uncertaintyNs":")"
      << r.mapped.uncertainty_ns << R"(","feedbackClockId":)" << f.clock_id
      << R"(,"feedbackSeconds":")" << f.seconds << R"(","feedbackNanoseconds":)" << f.nanoseconds
      << R"(,"feedbackSequence":")" << f.sequence << R"(","refreshNs":)" << f.refresh_ns
      << R"(,"flags":)" << f.flags << R"(,"correlation":)";
  if (f.correlation) {
    const auto& c = *f.correlation;
    out << R"({"clockId":)" << c.clock_id << R"(,"sourceNs":")" << c.source_ns
        << R"(","beforeMonoNs":")" << c.before_ns << R"(","afterMonoNs":")" << c.after_ns
        << R"("})";
  } else
    out << "null";
  out << R"(,"decodedSource":)";
  if (p.decoded) {
    const auto& d = *p.decoded;
    out << R"({"senderNtp":")" << d.sender_ntp << R"(","senderTimestampNs":")" << d.timestamp_ns
        << R"(","width":)" << d.width << R"(,"height":)" << d.height << '}';
  } else
    out << "null";
  out << R"(,"senderClockMapping":null,"phoneAck":null,"optical":null})";
  out.precision(precision);
}
}  // namespace headunit::theme::video
