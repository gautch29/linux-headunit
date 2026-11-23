// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/receiver_video.hpp>
#include <stdexcept>
namespace headunit::theme::video {
ReceiverVideo::ReceiverVideo()
    : queues_{std::make_shared<NativeInputQueue>(), std::make_shared<NativeInputQueue>()} {}
ReceiverVideo::~ReceiverVideo() {
  close();
}
void ReceiverVideo::attach(AirplaySessionSetup& session, bool negotiated_only) {
  if (session_ || closed_) throw std::logic_error("native video already attached or closed");
  session.set_video_consumer([queues = queues_, setup = &session, negotiated_only](
                                 std::uint64_t type, AirplayVideoPacket& packet) {
    if (type != 110 && type != 111) throw std::invalid_argument("unknown native screen type");
    if (negotiated_only) {
      const auto mode = setup->negotiated_mode();
      if (mode == CarplayMode::standard) return true;  // Standard sink already owns the packet.
      if (mode != CarplayMode::ultra)
        return false;  // Retain compressed data until actual UI selection.
    }
    const auto id = setup->video_connection_id(type);
    if (!id) throw std::logic_error("native packet has no active transport generation");
    auto& queue = queues[type == 110 ? 0 : 1];
    queue->begin_generation(*id);
    return queue->enqueue(packet);
  });
  session_ = &session;
  negotiated_only_ = negotiated_only;
}
ReceiverVideo::Queues ReceiverVideo::claim() {
  if (!session_ || closed_ || claimed_)
    throw std::logic_error("native video needs a fresh control session");
  if (negotiated_only_ && session_->negotiated_mode() != CarplayMode::ultra)
    throw std::logic_error("phone has not selected the Ultra UI service");
  claimed_ = true;
  return queues_;
}
void ReceiverVideo::pump(const AirplaySessionSetup& session) {
  if (&session != session_) throw std::logic_error("native video belongs to another session");
  if (closed_) return;
  for (const auto& queue : queues_)
    if (queue->closed()) {
      close();
      return;
    }
  if (negotiated_only_ && session.negotiated_mode() == CarplayMode::standard) return;
  for (const auto& status : session.video_status()) {
    auto& queue = queues_[status.type == 110 ? 0 : 1];
    if (status.state == AirplaySessionSetup::ServiceState::closed)
      queue->retire_generation(status.connection_id);
    else
      queue->begin_generation(status.connection_id);
  }
}
void ReceiverVideo::close() {
  if (closed_) return;
  closed_ = true;
  for (const auto& queue : queues_)
    queue->close();
}
}  // namespace headunit::theme::video
