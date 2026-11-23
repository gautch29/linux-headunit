// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_session.hpp>
#include <algorithm>
#include <stdexcept>
#include <limits>

namespace headunit::theme::vehicle {
namespace {
void add(std::size_t& total, std::size_t count) {
  total += std::min(count, std::numeric_limits<std::size_t>::max() - total);
}
int priority(std::string uuid) {
  for (auto& c : uuid)
    if (c >= 'a' && c <= 'f') c -= 32;
  if (uuid == normal_service) return 0;
  if (uuid == high_service) return 1;
  return -1;
}
}  // namespace
Session::Session(Configuration config, std::shared_ptr<Provider> provider,
                 Transport::WriteObserver observer)
    : observer_(std::move(observer)) {
  if (config.camera)
    camera_trace_ = std::make_shared<ResourceTrace>(
        std::array{config.camera->input_stream_on_iid, config.camera->camera_general_on_iid});
  model_ = std::make_unique<ConfiguredModel>(std::move(config), std::move(provider));
}
void Session::attach(AirplaySessionSetup& session) {
  if (owner_) throw std::logic_error("vehicle session already attached");
  coverage_ = std::make_shared<Coverage>(service().profile());
  session.register_data_service(normal_service);
  session.register_data_service(high_service);
  session.enable_vehicle_state();
  owner_ = &session;
}
void Session::pump(AirplaySessionSetup& session, std::uint64_t now) {
  if (&session != owner_) throw std::logic_error("wrong vehicle session owner");
  const auto services = session.service_status();
  for (auto& port : ports_)
    if (port.id) {
      const auto active = std::find_if(services.begin(), services.end(), [&](const auto& s) {
        return s.stream_id == port.id && s.state == AirplaySessionSetup::ServiceState::connected;
      });
      if (active == services.end()) port = Port{};
    }
  bool any = false;
  for (const auto& service : services) {
    const int index = priority(service.service_uuid);
    if (index < 0 || service.state != AirplaySessionSetup::ServiceState::connected) continue;
    auto& port = ports_[index];
    if (port.id && port.id != service.stream_id) {
      (void)session.close_service(service.stream_id);
      continue;
    }
    if (!port.id) {
      port.id = service.stream_id;
      port.transport = std::make_unique<Transport>(this->service(), observer_, camera_trace_,
                                                   coverage_, static_cast<unsigned>(index));
    }
    any = true;
  }
  if (!any) {
    if (had_channels_) {
      if (model_)
        model_->disconnect();
      else
        bench_->disconnect();
      notification_.reset();
    }
    had_channels_ = false;
    return;
  }
  had_channels_ = true;
  if (model_)
    model_->refresh(now);
  else
    bench_->refresh(now);
  for (auto& port : ports_)
    if (port.id) {
      try {
        port.transport->pump(now);
        bool blocked = false;
        session.deliver_service_messages(
            port.id,
            [&](const DataMessage& message) {
              const bool accepted = port.transport->receive(message, now);
              blocked = !accepted;
              return accepted;
            },
            8);
        (void)session.pause_service_input(port.id, blocked);
        for (unsigned count = 0; count < 32; ++count) {
          const auto* message = port.transport->pending();
          if (!message) break;
          if (!session.send_service_message(port.id, *message)) {
            blocked = true;
            break;
          }
          port.transport->sent();
        }
        if (blocked) {
          if (!port.blocked_since) port.blocked_since = now;
          if (now - *port.blocked_since >= 5000)
            throw std::runtime_error("vehicle channel stalled");
        } else
          port.blocked_since.reset();
      } catch (const std::exception& e) {
        coverage_->fault(&port == &ports_[0] ? 0 : 1);
        failure_ = std::string_view(e.what()).substr(0, 256);
        (void)session.close_service(port.id);
        port = Port{};
      }
    }
  // Updates use the ordinary channel when it is present, with a high-channel
  // fallback. A queued notification is retained until transport accepts it.
  auto& output = ports_[0].id ? ports_[0] : ports_[1];
  if (output.id) {
    if (!notification_) notification_ = service().updates(now);
    if (notification_ && output.transport->notify(*notification_)) notification_.reset();
  }
}
Session::Status Session::status() const {
  Status result;
  if (camera_trace_) result.camera_resources = camera_trace_->counts();
  if (bench_) {
    result.simulated_target_c = bench_->target_c();
    result.simulated_fan_level = bench_->fan_level();
    result.simulated_comfort_lighting = bench_->comfort_lighting();
  }
  for (const auto& port : ports_)
    if (port.id) ++result.channels;
  if (coverage_)
    for (const auto& channel : coverage_->summary().channels) {
      add(result.received, channel.received);
      add(result.writes, channel.writes);
      const auto counts = channel.requests;
      add(result.requests.configuration, counts.configuration);
      add(result.requests.read, counts.read);
      add(result.requests.registration, counts.registration);
      add(result.requests.unregistration, counts.unregistration);
      add(result.requests.control, counts.control);
      add(result.requests.other, counts.other);
    }
  return result;
}
}  // namespace headunit::theme::vehicle
