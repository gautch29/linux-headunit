// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/ui_session.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace headunit::theme::script {
namespace {
bool is_ui(std::string id) {
  std::transform(id.begin(), id.end(), id.begin(),
                 [](unsigned char c) { return char(std::toupper(c)); });
  return id == ultra_ui_service;
}
}  // namespace
UiSession::UiSession(Factory factory) : factory_(std::move(factory)) {
  if (!factory_) throw std::invalid_argument("UI session requires a configuration factory");
}
void UiSession::attach(AirplaySessionSetup& session) {
  if (session_ || stopped_) throw std::logic_error("UI session already attached or stopped");
  session.register_data_service(ultra_ui_service);
  session_ = &session;
}
void UiSession::retire() {
  if (worker_) worker_->request_stop();
  stream_id_.reset();
  blocked_since_.reset();
}
void UiSession::stop() {
  stopped_ = true;
  retire();
}
bool UiSession::finished() const {
  return stopped_ && (!worker_ || worker_->finished());
}
std::optional<UiWorker::Status> UiSession::status() const {
  if (!worker_) return {};
  return worker_->status();
}
void UiSession::pump(AirplaySessionSetup& session) {
  if (&session != session_) throw std::logic_error("UI session used with another connection");
  if (stopped_) return;
  if (worker_) {
    const auto reason = worker_->failure_reason();
    if (!reason.empty()) failure_ = reason;
  }
  const auto services = session.service_status();
  if (stream_id_) {
    const auto active = std::find_if(services.begin(), services.end(), [&](const auto& s) {
      return s.stream_id == *stream_id_ && is_ui(s.service_uuid) &&
             s.state == AirplaySessionSetup::ServiceState::connected;
    });
    if (active == services.end()) retire();
  }
  if (worker_ && worker_->finished()) {
    if (stream_id_) {
      failure_ = worker_->failure_reason();
      if (failure_.empty()) failure_ = "Ultra UI worker ended";
      (void)session.close_service(*stream_id_);
      retire();
    }
    worker_.reset();  // finished() includes owner-thread GPU/resource destruction.
  }
  for (const auto& service : services) {
    if (!is_ui(service.service_uuid) ||
        service.state != AirplaySessionSetup::ServiceState::connected ||
        attempted_.contains(service.stream_id))
      continue;
    // Never put a second renderer on screen while the previous generation is
    // still being destroyed. The peer can negotiate a fresh stream afterward.
    attempted_.insert(service.stream_id);
    if (worker_) {
      (void)session.close_service(service.stream_id);
      continue;
    }
    try {
      worker_ = std::make_unique<UiWorker>(service.stream_id, factory_());
      stream_id_ = service.stream_id;
      failure_.clear();
    } catch (const std::exception& error) {
      failure_ = std::string_view(error.what()).substr(0, 512);
      (void)session.close_service(service.stream_id);
    }
  }
  if (!stream_id_ || !worker_) return;
  try {
    bool blocked = false;
    (void)session.deliver_service_messages(*stream_id_, [&](const DataMessage& message) {
      if (message.payload.size() > 512 * 1024)
        throw std::invalid_argument("Ultra UI payload too large");
      const bool accepted = worker_->receive(*stream_id_, message);
      blocked = !accepted;
      return accepted;
    });
    (void)session.pause_service_input(*stream_id_, blocked);
    const auto now = std::chrono::steady_clock::now();
    if (blocked) {
      if (!blocked_since_) blocked_since_ = now;
      if (now - *blocked_since_ >= std::chrono::seconds(5))
        throw std::runtime_error("Ultra UI worker input stalled");
    } else
      blocked_since_.reset();
    for (unsigned n = 0; n < 32; ++n) {
      if (!worker_->send_pending([&](const DataMessage& message) {
            return session.send_service_message(*stream_id_, message);
          }))
        break;
    }
  } catch (const std::exception& error) {
    failure_ = std::string_view(error.what()).substr(0, 512);
    (void)session.close_service(*stream_id_);
    retire();
  }
}
}  // namespace headunit::theme::script
