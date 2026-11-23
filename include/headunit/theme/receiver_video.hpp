// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/native_input.hpp>
#include <headunit/airplay_session_setup.hpp>
#include <array>

namespace headunit::theme::video {
// One control-session generation. Receiver calls attach/claim/pump/close;
// only the worker consumes the two thread-safe queues. No decoder lives here.
class ReceiverVideo {
 public:
  using Queues = std::array<std::shared_ptr<NativeInputQueue>, 2>;
  ReceiverVideo();
  ~ReceiverVideo();
  ReceiverVideo(const ReceiverVideo&) = delete;
  ReceiverVideo& operator=(const ReceiverVideo&) = delete;
  // Diagnostic replay defaults to an explicit native consumer. Production
  // automatic mode must gate packets until the phone selects its UI service.
  void attach(AirplaySessionSetup&, bool negotiated_only = false);
  Queues claim();  // Once: a replacement UI worker requires a fresh control session.
  void pump(const AirplaySessionSetup&);
  void close();
  bool closed() const { return closed_; }
  bool claimed() const { return claimed_; }

 private:
  Queues queues_;
  const AirplaySessionSetup* session_ = nullptr;
  bool closed_ = false, claimed_ = false;
  bool negotiated_only_ = false;
};
}  // namespace headunit::theme::video
