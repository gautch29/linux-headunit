// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/script_runtime.hpp>
#include <headunit/theme/ui_channel.hpp>

namespace headunit::theme::script {
enum class Delivery { accepted, unknown_display, backpressure, malformed, faulted };

// Transport adapter for asset-owned sessions. Unlike ui::Channel, this class
// does NOT add a C++ session: APL.ClusterControlSession owns sequencing and ACKs
// inside each display's VM. The shared phone channel carries only the OPACK
// displayUUID/data envelope. All operations run on one owning event-loop thread.
class Channel {
 public:
  Channel(ResourceReader, std::vector<Display>, Limits = {});
  Channel(const TrustedDirectory&, std::vector<Display>, Limits = {});
  Runtime& runtime(std::string_view display);
  void set_connected(bool);
  Delivery receive(std::span<const std::uint8_t>);
  // Already decoded envelope, for the independent receiver's bounded codec.
  Delivery receive(ui::Message);
  // Call only when the transport can accept an entire message. The transport
  // must retain a returned message until its asynchronous write completes.
  std::optional<Bytes> take_outgoing();
  std::optional<ui::Message> take_outgoing_message();

 private:
  template <class Resources>
  void initialize(const Resources&, std::vector<Display>, Limits);
  struct Entry {
    std::string identifier;
    std::unique_ptr<Runtime> vm;
  };
  std::vector<Entry> entries_;
  std::size_t next_ = 0;
};
}  // namespace headunit::theme::script
