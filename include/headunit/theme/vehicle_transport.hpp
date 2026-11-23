// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/airplay_data_stream.hpp>
#include <headunit/binary_plist.hpp>
#include <headunit/theme/vehicle_service.hpp>
#include <deque>
#include <array>
#include <memory>

namespace headunit::theme::vehicle {
inline constexpr const char* normal_service = "3E2F3C61-AAD0-42CB-A8AA-BF22186DA62E";
inline constexpr const char* high_service = "FF4A6720-F2BE-4F56-A3E1-DB3B4E37D634";
struct RequestCounts {
  std::size_t configuration = 0, read = 0, registration = 0, unregistration = 0, control = 0,
              other = 0;
  bool operator==(const RequestCounts&) const = default;
};
// Content-free, receiver-thread coverage for one connection. Known profile IIDs
// cannot be displaced by unknown requests. Unknown IIDs use a rotating bound;
// counters survive channel retirement. Values/commands/transactions are absent.
class Coverage {
 public:
  struct Channel {
    std::size_t received = 0, writes = 0, response_errors = 0, outputs = 0;
    std::size_t unsupported_types = 0, retries = 0, faults = 0, receipts = 0;
    RequestCounts requests;
    bool operator==(const Channel&) const = default;
  };
  struct ResourceCounts {
    std::size_t reads = 0, registrations = 0, unregistrations = 0, writes = 0;
    std::size_t returned_values = 0, returned_errors = 0;
    std::size_t transferred_values = 0, transferred_errors = 0, confirmed_writes = 0;
    bool operator==(const ResourceCounts&) const = default;
  };
  struct Resource {
    std::int64_t iid;
    std::optional<std::int64_t> service_iid, service_type;
    std::array<ResourceCounts, 2> channels;
    bool operator==(const Resource&) const = default;
  };
  struct Summary {
    std::array<Channel, 2> channels;
    std::size_t unknown_evictions = 0, diagnostic_errors = 0;
    bool operator==(const Summary&) const = default;
  };
  static constexpr std::size_t max_unknown = 32;
  explicit Coverage(const opack::Value& validated_profile);
  const Summary& summary() const noexcept { return summary_; }
  std::vector<Resource> resources() const;  // Bounded; take only for reporting.
  void request(unsigned channel, const Incoming&, const std::optional<Command>&) noexcept;
  void response(unsigned channel, const Command&) noexcept;
  void transferred(unsigned channel, const Incoming&) noexcept;
  void unsupported(unsigned channel) noexcept;
  void retry(unsigned channel) noexcept;
  void fault(unsigned channel) noexcept;
  void receipt(unsigned channel) noexcept;

 private:
  std::map<std::int64_t, Resource> known_;
  std::array<std::optional<Resource>, max_unknown> unknown_;
  std::size_t next_unknown_ = 0;
  Summary summary_;
  ResourceCounts& resource(unsigned channel, std::int64_t iid);
};
// Content-free evidence for two explicitly configured Boolean resources.
// Receiver-thread only; never stores peer commands, identities or car values.
// Outbound counts mean accepted by the session transport, not peer receipt.
class ResourceTrace {
 public:
  struct Counts {
    std::size_t reads = 0, registrations = 0, unregistrations = 0;
    std::size_t responses_on = 0, responses_off = 0, response_errors = 0;
    std::size_t updates_on = 0, updates_off = 0, update_errors = 0;
    bool operator==(const Counts&) const = default;
  };
  explicit ResourceTrace(std::array<std::int64_t, 2>);
  void request(const Command&, const Command& response) noexcept;
  void transferred(const Command&) noexcept;
  const std::array<Counts, 2>& counts() const noexcept { return counts_; }

 private:
  std::array<std::int64_t, 2> iids_;
  std::array<Counts, 2> counts_{};
};
// Called only after the socket layer authenticated a DataStream message. Both
// priority channels share one connection's Service; each has an ordered queue.
class Transport {
 public:
  using WriteObserver = std::function<void(const Command& request, const Command& response)>;
  explicit Transport(Service& service, WriteObserver observer = {},
                     std::shared_ptr<ResourceTrace> trace = {},
                     std::shared_ptr<Coverage> coverage = {}, unsigned channel = 0);
  ~Transport();
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;
  void pump(std::uint64_t now_ms);
  bool receive(const DataMessage&, std::uint64_t now_ms);  // false: retry intact.
  bool notify(Bytes);  // false: retain/coalesce at the session owner.
  const DataMessage* pending() const;
  void sent();
  std::size_t received() const { return coverage_->summary().channels[channel_].received; }
  std::size_t writes() const { return coverage_->summary().channels[channel_].writes; }
  using RequestCounts = vehicle::RequestCounts;
  RequestCounts request_counts() const { return coverage_->summary().channels[channel_].requests; }

 private:
  static constexpr std::size_t max_messages = 32, max_bytes = 2 * AirplayDataStream::max_package;
  Service& service_;
  std::deque<DataMessage> pending_;
  std::size_t pending_bytes_ = 0;
  WriteObserver observer_;
  std::shared_ptr<ResourceTrace> trace_;
  std::shared_ptr<Coverage> coverage_;
  unsigned channel_ = 0;
  std::uint64_t response_owner_ = 0;
  std::optional<Command> deferred_request_;
  void enqueue(DataMessage);
};
// Accept only locally generated make_airplay_info output, never a peer payload.
// The same initialized configuration is used for /info and configResponse.
// Conversion rejects non-plist objects and numeric dictionary keys here.
Bytes add_server_info(std::span<const std::uint8_t> airplay_info, const Service&,
                      std::uint64_t now_ms);
}  // namespace headunit::theme::vehicle
