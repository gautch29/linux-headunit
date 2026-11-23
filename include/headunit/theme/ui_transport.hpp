// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/airplay_data_stream.hpp>
#include <headunit/theme/script_channel.hpp>
#include <array>

namespace headunit::theme::script {
struct DisplayRoute {
  std::string phone_uuid;
  std::string asset_display;
};
enum class TransportDelivery {
  accepted,
  wrong_stream,
  closed,
  unsupported,
  malformed,
  unknown_display,
  backpressure,
  faulted
};
enum class UiFieldShape : std::uint8_t {
  absent,
  boolean,
  integer,
  real,
  string,
  bytes,
  array,
  dictionary
};
std::string_view ui_field_shape_name(UiFieldShape);
// Original, fixed-size diagnostics for the existing command route. No packet,
// arbitrary dictionary key, layout/component identity or captured data escapes.
// Numeric focus tokens are ephemeral correlation data for a PRIVATE sink only.
struct UiCommandDiagnostic {
  enum class Direction : std::uint8_t { incoming_vm_queue, outgoing_worker_queue };
  inline static constexpr std::array<std::string_view, 8> field_names{
      "tokenID",          "pts",           "layoutID",      "activeComponents",
      "targetComponents", "presentStream", "dismissStream", "isTransitioning"};
  Direction direction = Direction::incoming_vm_queue;
  std::uint8_t panel = 0;
  std::uint64_t generation = 0, count = 0, monotonic_ns = 0;
  std::optional<std::int64_t> command_type, focus_token;
  UiFieldShape type_shape = UiFieldShape::absent, data_shape = UiFieldShape::absent;
  std::array<UiFieldShape, field_names.size()> fields{};
  std::optional<bool> transitioning;
  bool budget_exhausted = false;
  bool converted_transition_timestamp = false;
};
using UiCommandDiagnosticSink = std::function<void(const UiCommandDiagnostic&)>;

// One negotiated UI DataStream and one connection-scoped set of display VMs.
// Call on the VMs' owner thread. Channel must outlive this adapter. The caller
// supplies messages only after AirplayDataStream authenticates/decrypts them.
// Asset Lua owns session sequencing/ACKs; this adds no second UI session.
class UiTransport {
 public:
  // Optional private owner-thread sink; must be bounded/nonblocking and must
  // not reenter this adapter or VM. Exceptions are ignored. At most 1..256
  // events per panel over this transport's lifetime, never replenished by reset.
  UiTransport(Channel&, std::uint64_t stream_id, std::vector<DisplayRoute>,
              UiCommandDiagnosticSink = {}, std::size_t diagnostic_limit = 256);
  ~UiTransport();
  UiTransport(const UiTransport&) = delete;
  UiTransport& operator=(const UiTransport&) = delete;
  void start();
  void close();  // Terminal. Reconnect requires a new adapter and fresh VMs.
  TransportDelivery receive(std::uint64_t stream_id, const DataMessage&);
  // Bounded parser diagnostic only; never includes peer payload contents.
  const std::string& rejection_reason() const { return rejection_reason_; }
  // Reacquire after every VM tick: a script reset can invalidate a held message.
  // Retain this message while send_service_message returns false and its display
  // has not reset. Do not tick the VM between successful enqueue and accepted().
  // accepted() means transport enqueue succeeded, not GPU presentation.
  const DataMessage* pending();
  void accepted();

 private:
  Channel& channel_;
  std::uint64_t stream_id_;
  std::vector<DisplayRoute> routes_;
  std::vector<std::string> phone_ids_;
  bool started_ = false, closed_ = false;
  std::optional<DataMessage> pending_;
  std::string pending_display_;
  std::string rejection_reason_;
  std::uint64_t pending_generation_ = 0;
  UiCommandDiagnosticSink diagnostic_sink_;
  std::size_t diagnostic_limit_;
  std::array<std::size_t, 8> diagnostic_counts_{};
  std::optional<UiCommandDiagnostic> pending_diagnostic_;
  std::size_t pending_panel_ = 0;
  void diagnose(std::optional<UiCommandDiagnostic>, std::size_t, UiCommandDiagnostic::Direction,
                std::uint64_t) noexcept;
  bool current() const;
};
}  // namespace headunit::theme::script
