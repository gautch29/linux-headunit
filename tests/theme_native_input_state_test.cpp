// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/native_input.hpp>
#include <chrono>
#include <atomic>
#include <iostream>
#include <thread>

using namespace headunit;
using namespace headunit::theme::video;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
template <class F>
void rejects(F fn) {
  bool failed = false;
  try {
    fn();
  } catch (const std::exception&) {
    failed = true;
  }
  CHECK(failed);
}
void replacement_failure_with_pending_receipts() {
  // Native frames, media time and accepted flags are explicit fixtures. The
  // actual NativeInput admission exception must retire old Stream ownership
  // without releasing backing still held by an external composition snapshot.
  const auto at = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
  MediaClock clock;
  clock.update({true, (std::uint64_t{900 + 2208988800} << 32), at, at, at, 1000});
  auto memory = std::make_shared<Memory>();
  auto failed_stream = std::make_shared<Stream>(Input{"failed-replacement", 2, 2}, memory);
  auto failed_queue = std::make_shared<NativeInputQueue>();
  failed_queue->begin_generation(801);
  NativeInput failed(failed_stream, failed_queue);
  auto healthy_stream = std::make_shared<Stream>(Input{"healthy-peer", 2, 2}, memory);
  auto healthy_queue = std::make_shared<NativeInputQueue>();
  healthy_queue->begin_generation(901);
  NativeInput healthy(healthy_stream, healthy_queue);
  const auto publish = [](Stream& target, unsigned quarter) {
    auto frame = std::make_shared<NativeFrame>();
    frame->metadata = {(std::uint64_t{899} << 32) + (std::uint64_t{quarter} << 30),
                       899000000000ULL + quarter * 250000000ULL, 2, 2};
    frame->lifetime = std::make_shared<int>(1);
    const std::weak_ptr<const void> backing = frame->lifetime;
    CHECK(target.offer_native(899 + quarter * 0.25, frame));
    return backing;
  };
  const auto current_backing = publish(*failed_stream, 0);
  const auto current = failed.stage(clock);
  CHECK(current);
  failed.complete(*current, true);
  auto held_current = failed_stream->native();
  const auto current_metadata = held_current->metadata;
  const auto first_backing = publish(*failed_stream, 1);
  const auto second_backing = publish(*failed_stream, 2);
  const auto queued_backing = publish(*failed_stream, 3);
  const auto first = failed.stage(clock);
  const auto second = failed.stage(clock);
  CHECK(first && second && *current < *first && *first < *second && !failed.stage(clock));
  auto held_prepared = failed_stream->native();
  const auto prepared_metadata = held_prepared->metadata;
  publish(*healthy_stream, 0);
  const auto healthy_current = healthy.stage(clock);
  CHECK(healthy_current);
  healthy.complete(*healthy_current, true);
  publish(*healthy_stream, 1);
  const auto healthy_pending = healthy.stage(clock);
  CHECK(healthy_pending && memory->native_frames == 6);

  // The successor has supplied no codec configuration. Its actual admission
  // failure combines adoption and cleanup with two outstanding receipts from
  // the preceding transport; no decoder or GPU import runs in this fixture.
  failed_queue->begin_generation(802);
  CHECK(!failed.composition_fresh() && !failed.stage(clock));
  CHECK(failed_stream->prepared(*first) && failed_stream->prepared(*second));
  AirplayVideoPacket packet;
  packet.kind = AirplayVideoPacket::Kind::frame;
  packet.sender_ntp = std::uint64_t{900} << 32;
  packet.data = {1};
  CHECK(failed_queue->enqueue(packet));
  rejects([&] { failed.pump(nullptr); });
  const auto identity = failed.identity();
  CHECK(identity.closed && !identity.available && !identity.owner_current &&
        identity.owner_connection_id == 802 && identity.ingress_connection_id == 802 &&
        identity.owner_generation == identity.ingress_generation);
  CHECK(failed.stats().generation_changes == 1 && failed.stats().submitted == 0 &&
        failed.stats().decoded == 0 && failed.stats().presented == 1 &&
        failed.stats().discarded == 2);
  CHECK(!failed_stream->current_pts() && !failed_stream->next_pts() &&
        !failed_stream->composition_token() && !failed_stream->native());
  CHECK(!failed_stream->prepared(*first) && !failed_stream->prepared(*second));
  CHECK(first_backing.expired() && queued_backing.expired());
  CHECK(!current_backing.expired() && !second_backing.expired() && memory->native_frames == 4);
  CHECK(held_current->metadata.sender_ntp == current_metadata.sender_ntp &&
        held_current->metadata.timestamp_ns == current_metadata.timestamp_ns &&
        held_prepared->metadata.sender_ntp == prepared_metadata.sender_ntp &&
        held_prepared->metadata.timestamp_ns == prepared_metadata.timestamp_ns);

  for (unsigned poll = 0; poll < 3; ++poll) {
    CHECK(!failed.identity().owner_current && !failed.composition_fresh() && !failed.stage(clock));
    rejects([&] { failed.complete(*first, true); });
    rejects([&] { failed.complete(*second, true); });
    rejects([&] { failed.pump(nullptr); });
    CHECK(failed.stats().presented == 1 && failed.stats().discarded == 2 &&
          !failed_stream->composition_token());
  }
  AirplayVideoPacket late;
  late.kind = AirplayVideoPacket::Kind::frame;
  late.sender_ntp = std::uint64_t{900} << 32;
  late.data = {2};
  rejects([&] { failed_queue->enqueue(late); });
  CHECK(late.data == std::vector<std::uint8_t>{2});
  rejects([&] { failed_queue->begin_generation(803); });

  // This is local input isolation only. ReceiverVideo deliberately closes both
  // queues when its shared supervisor observes a terminal endpoint failure.
  CHECK(healthy.identity().owner_connection_id == 901 && healthy.composition_fresh());
  CHECK(healthy_stream->current_pts() == 899 && healthy_stream->prepared(*healthy_pending) &&
        healthy_stream->composition_token() == healthy_pending);
  CHECK(healthy_stream->native()->metadata.sender_ntp ==
        (std::uint64_t{899} << 32) + (std::uint64_t{1} << 30));
  CHECK(healthy.stats().presented == 1 && healthy.stats().discarded == 0);
  held_current.reset();
  CHECK(current_backing.expired() && !second_backing.expired() && memory->native_frames == 3);
  held_prepared.reset();
  CHECK(second_backing.expired() && memory->native_frames == 2);
  healthy.complete(*healthy_pending, true);
  CHECK(healthy_stream->current_pts() == 899.25 && healthy.stats().presented == 2 &&
        memory->native_frames == 1);
  rejects([&] { healthy.complete(*healthy_pending, true); });
  healthy.pump(nullptr);
  CHECK(!healthy.stage(clock) && healthy.stats().presented == 2);
  healthy.suspend();
  CHECK(memory->native_frames == 0);
}
int main() try {
  replacement_failure_with_pending_receipts();
  const auto at = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
  MediaClock clock;
  clock.update({true, (std::uint64_t{900 + 2208988800} << 32), at, at, at, 1000});
  auto memory = std::make_shared<Memory>();
  auto stream = std::make_shared<Stream>(Input{"receipts", 2, 2}, memory);
  auto queue = std::make_shared<NativeInputQueue>();
  NativeInput input(stream, queue);
  const auto standalone = input.identity();
  CHECK(standalone.observed_ns >= at);
  CHECK(!standalone.owner_connection_id && !standalone.ingress_connection_id);
  CHECK(standalone.owner_generation == 0 && standalone.ingress_generation == 0 &&
        standalone.owner_current && standalone.available && !standalone.closed);
  const auto offer = [&](double pts) {
    auto frame = std::make_shared<NativeFrame>();
    frame->metadata.width = 2;
    frame->metadata.height = 2;
    frame->lifetime = std::make_shared<int>(1);
    CHECK(stream->offer_native(pts, frame));
  };
  offer(899.8);
  offer(899.9);
  offer(900);
  const auto first = input.stage(clock), second = input.stage(clock);
  CHECK(first && second && first != second && !input.stage(clock));
  CHECK(input.stats().max_staged == 2);
  CHECK(!stream->current_pts() && !stream->can_offer_native());
  rejects(
      [&] { input.complete(*second, true); });  // Receipt FIFO is independent of token validity.
  input.complete(*first, false);
  CHECK(input.stats().discarded == 1 && input.stats().presented == 0 && memory->native_frames == 2);
  CHECK(stream->prepared(*second) && stream->next_pts() == 899.9);
  input.complete(*second, true);
  CHECK(input.stats().presented == 1 && stream->current_pts() == 899.9);
  auto third = input.stage(clock);
  CHECK(third && *third > *second);
  stream->flush();
  CHECK(memory->native_frames == 0);
  offer(900.000001);  // New generation in the same source timeline after a script reset.
  auto fourth = input.stage(clock);
  CHECK(fourth && *fourth > *third);
  input.complete(*third, true);
  CHECK(input.stats().discarded == 2 && !stream->current_pts());
  input.complete(*fourth, true);
  CHECK(input.stats().presented == 2 && stream->current_pts() == 900.000001);
  rejects([&] { input.complete(*fourth, true); });
  offer(900.000002);
  offer(900.000003);
  const auto fifth = input.stage(clock), sixth = input.stage(clock);
  CHECK(fifth && sixth);
  input.suspend();
  CHECK(input.stats().discarded == 4 && memory->native_frames == 0 && !stream->current_pts());
  rejects([&] { input.complete(*fifth, true); });
  offer(901);
  CHECK(!input.stage(clock));  // Future source frame must not be displayed early.
  input.suspend();
  CHECK(memory->native_frames == 0);
  {
    // Actual NativeInput/Stream ownership with explicit numeric decoded fixtures.
    auto q = std::make_shared<NativeInputQueue>();
    q->begin_generation(801);
    auto st = std::make_shared<Stream>(Input{"receipt-facts", 2, 2}, memory);
    NativeInput owner(st, q);
    const auto publish = [&](double pts, std::uint64_t sender) {
      auto frame = std::make_shared<NativeFrame>();
      frame->metadata = {sender, sender + 19, 2, 2};
      frame->lifetime = std::make_shared<int>(1);
      CHECK(st->offer_native(pts, frame));
    };
    const auto receipt = [&](std::uint64_t token) {
      InputPresentationReceipt r;
      r.submission_epoch = r.current_epoch = 7;
      r.submitted_ns = at;
      r.feedback.token = token;
      r.feedback.presented = true;
      r.feedback.clock_id = 4;
      r.feedback.seconds = 40;
      r.feedback.nanoseconds = 51;
      r.feedback.received_ns = at + 300;
      r.feedback.sequence = token + 90;
      r.feedback.correlation = ClockCorrelation{4, 40000001000, at + 200, at + 200};
      r.mapped = {token, at + 100, 77, 13, 900.0};
      return r;
    };
    publish(899.7, 0xfedcba9876543210ULL);
    auto token = owner.stage(clock);
    CHECK(token && !owner.presentation());
    publish(899.8, 1000);
    const auto successor = owner.stage(clock);
    CHECK(successor && *successor > *token);
    auto r = receipt(100);
    owner.complete(*token, true, &r);
    const auto current = owner.presentation();
    CHECK(current && current->stream_token == *token && current->owner_generation == 1 &&
          current->owner_connection_id == 801 && current->source_pts == 899.7 && current->decoded &&
          current->decoded->sender_ntp == 0xfedcba9876543210ULL &&
          current->decoded->timestamp_ns == 0xfedcba9876543223ULL &&
          current->receipt.feedback.token == 100 &&
          current->receipt.mapped.monotonic_ns == at + 100 &&
          current->receipt.feedback.received_ns == at + 300);
    CHECK(owner.presentation()->stream_token == *token);
    rejects([&] { owner.complete(*successor, true, &r); });
    CHECK(st->prepared(*successor) && owner.presentation()->stream_token == *token);
    r = receipt(101);
    r.current_epoch = 8;
    rejects([&] { owner.complete(*successor, true, &r); });
    CHECK(st->prepared(*successor));
    r.current_epoch = 7;
    owner.complete(*successor, true, &r);
    CHECK(owner.presentation()->stream_token == *successor &&
          owner.presentation()->decoded->sender_ntp == 1000);
    st->flush();
    CHECK(!owner.presentation());
    publish(899.9, 2000);
    const auto old = owner.stage(clock);
    q->retire_generation(801);
    CHECK(!owner.presentation());
    q->begin_generation(802);
    owner.pump(nullptr);
    publish(900, 3000);
    const auto fresh = owner.stage(clock);
    CHECK(old && fresh && *fresh > *old && !owner.presentation());
    r = receipt(102);
    owner.complete(*old, true, &r);
    CHECK(!owner.presentation() && st->prepared(*fresh));
    r = receipt(103);
    owner.complete(*fresh, true, &r);
    CHECK(owner.presentation()->owner_connection_id == 802 &&
          owner.presentation()->decoded->sender_ntp == 3000);
    owner.suspend();
    CHECK(!owner.presentation());
    publish(900.000001, 4000);
    token = owner.stage(clock);
    CHECK(token);
    owner.complete(*token, true);  // Legacy bool cannot invent mapped receipt.
    CHECK(!owner.presentation() && st->current_token() == token);
    AirplayVideoPacket bad;
    bad.kind = AirplayVideoPacket::Kind::frame;
    bad.sender_ntp = 0;
    CHECK(q->enqueue(bad));
    rejects([&] { owner.pump(nullptr); });
    CHECK(!owner.presentation() && q->closed());
  }
  {
    auto replacement_stream = std::make_shared<Stream>(Input{"replacement", 2, 2}, memory);
    auto endpoint = std::make_shared<NativeInputQueue>();
    endpoint->begin_generation(501);
    NativeInput replacement(replacement_stream, endpoint);
    const auto original = replacement.identity();
    CHECK(original.owner_connection_id == 501 && original.ingress_connection_id == 501 &&
          original.owner_generation == 1 && original.ingress_generation == 1 &&
          original.owner_current);
    const auto publish = [&](double pts) {
      auto frame = std::make_shared<NativeFrame>();
      frame->metadata.width = 2;
      frame->metadata.height = 2;
      frame->lifetime = std::make_shared<int>(1);
      CHECK(replacement_stream->offer_native(pts, frame));
    };
    publish(899.8);
    const auto old = replacement.stage(clock);
    CHECK(old && replacement.composition_fresh());
    endpoint->retire_generation(501);
    CHECK(!endpoint->closed() && !endpoint->available() && !replacement.composition_fresh());
    CHECK(!replacement.stage(clock));
    const auto retired = replacement.identity();
    CHECK(retired.owner_connection_id == 501 && retired.ingress_connection_id == 501 &&
          retired.owner_generation == 1 && retired.ingress_generation == 2 &&
          !retired.owner_current && !retired.available && !retired.closed);
    // Retirement can race a receipt before the next decoder-owner turn.
    replacement.complete(*old, true);
    CHECK(!replacement_stream->current_pts() && replacement.stats().presented == 0 &&
          replacement.stats().discarded == 1 && memory->native_frames == 0);
    replacement.pump(nullptr);
    endpoint->begin_generation(502);
    const auto awaiting = replacement.identity();
    CHECK(awaiting.owner_connection_id == 501 && awaiting.ingress_connection_id == 502 &&
          awaiting.available && !awaiting.owner_current);
    replacement.pump(nullptr);
    const auto adopted = replacement.identity();
    CHECK(adopted.owner_connection_id == 502 && adopted.ingress_connection_id == 502 &&
          adopted.owner_generation == adopted.ingress_generation && adopted.owner_current);
    publish(899.9);
    const auto late = replacement.stage(clock);
    CHECK(late && *late > *old);
    endpoint->retire_generation(502);
    endpoint->begin_generation(503);
    replacement.pump(nullptr);
    publish(900);
    const auto fresh = replacement.stage(clock);
    CHECK(fresh && *fresh > *late);
    const auto successor = replacement.identity();
    CHECK(successor.owner_connection_id == 503 && successor.ingress_connection_id == 503 &&
          successor.owner_current);
    replacement.complete(*late, true);
    CHECK(replacement.identity().owner_connection_id == 503);
    CHECK(!replacement_stream->current_pts() && replacement_stream->prepared(*fresh));
    replacement.complete(*fresh, true);
    CHECK(replacement_stream->current_pts() == 900 && replacement.stats().presented == 1 &&
          replacement.stats().discarded == 2);
    endpoint->retire_generation(501);  // A closed transport tombstone is harmless.
    CHECK(endpoint->available() && replacement.composition_fresh());
    rejects([&] { endpoint->begin_generation(501); });
    CHECK(endpoint->available() && replacement.composition_fresh());
    endpoint->close();
    const auto closed = replacement.identity();
    CHECK(closed.closed && !closed.available && !closed.owner_current &&
          closed.owner_connection_id == 503 && closed.ingress_connection_id == 503);
    rejects([&] { replacement.pump(nullptr); });
    rejects([&] { endpoint->begin_generation(504); });
    CHECK(memory->native_frames == 0);
  }
  {
    auto zero_stream = std::make_shared<Stream>(Input{"zero-id", 2, 2}, memory);
    auto zero_queue = std::make_shared<NativeInputQueue>();
    zero_queue->begin_generation(0);
    NativeInput zero(zero_stream, zero_queue);
    CHECK(zero.identity().owner_connection_id == 0 && zero.identity().ingress_connection_id == 0);
    zero_queue->retire_generation(0);
    CHECK(!zero.identity().owner_current && zero.identity().ingress_connection_id == 0);
  }
  {
    // Actual production admission failure: a frame before codec configuration
    // closes the endpoint. Diagnostic identity must not report a live owner.
    auto failed_stream = std::make_shared<Stream>(Input{"failed-owner", 2, 2}, memory);
    auto failed_queue = std::make_shared<NativeInputQueue>();
    failed_queue->begin_generation(701);
    NativeInput failed(failed_stream, failed_queue);
    AirplayVideoPacket packet;
    packet.kind = AirplayVideoPacket::Kind::frame;
    packet.sender_ntp = 0;
    CHECK(failed_queue->enqueue(packet));
    rejects([&] { failed.pump(nullptr); });
    const auto identity = failed.identity();
    CHECK(identity.closed && !identity.available && !identity.owner_current &&
          identity.owner_connection_id == 701 && identity.ingress_connection_id == 701);
    CHECK(failed.stats().presented == 0 && !failed_stream->current_pts());
  }
  {
    NativeInputQueue early;
    early.retire_generation(0);  // EOF before any packet/worker is valid.
    CHECK(!early.available() && !early.closed());
    rejects([&] { early.begin_generation(0); });
    for (std::uint64_t id = 1; id < 12; ++id)
      early.begin_generation(id);
    rejects([&] { early.begin_generation(12); });
    CHECK(early.available());
  }
  {
    auto concurrent_stream = std::make_shared<Stream>(Input{"concurrent", 2, 2}, memory);
    auto endpoint = std::make_shared<NativeInputQueue>();
    NativeInput consumer(concurrent_stream, endpoint);
    std::atomic<bool> done = false;
    std::exception_ptr failure;
    std::jthread producer([&](std::stop_token stop) {
      try {
        for (std::uint64_t id = 0; id < 12 && !stop.stop_requested(); ++id) {
          endpoint->begin_generation(id);
          for (unsigned packet = 0; packet < 128 && !stop.stop_requested(); ++packet) {
            AirplayVideoPacket config;
            config.kind = AirplayVideoPacket::Kind::config;
            while (!stop.stop_requested() && !endpoint->enqueue(config))
              std::this_thread::yield();
          }
          if (id != 11) endpoint->retire_generation(id);
        }
      } catch (...) {
        failure = std::current_exception();
      }
      done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done && std::chrono::steady_clock::now() < deadline)
      consumer.pump(nullptr);
    CHECK(done);
    producer.join();
    if (failure) std::rethrow_exception(failure);
    consumer.pump(nullptr);
    CHECK(!endpoint->closed() && endpoint->available());
    // Old owner-held packets must not debit the replacement generation's credits.
    AirplayVideoPacket config;
    config.kind = AirplayVideoPacket::Kind::config;
    for (unsigned packet = 0; packet < NativeInputQueue::max_packets; ++packet)
      CHECK(endpoint->enqueue(config));
    CHECK(!endpoint->enqueue(config));
    endpoint->close();
    rejects([&] { consumer.pump(nullptr); });
    CHECK(memory->native_frames == 0);
  }
  {
    // A presentation advance stages a frame that much before its PTS is due,
    // keeps strict ordering and still holds frames beyond the advance. A fresh
    // clock keeps the earlier threaded checks' run time out of the margins.
    const auto now_at = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
    MediaClock advance_clock;
    advance_clock.update(
        {true, (std::uint64_t{900 + 2208988800} << 32), now_at, now_at, now_at, 1000});
    rejects([&] {
      NativeInput(std::make_shared<Stream>(Input{"too-early", 2, 2}, memory),
                  std::make_shared<NativeInputQueue>(), std::string{},
                  NativeInput::max_presentation_advance_ns + 1);
    });
    const auto early_offer = [&](Stream& target, double pts) {
      auto frame = std::make_shared<NativeFrame>();
      frame->metadata.width = 2;
      frame->metadata.height = 2;
      frame->lifetime = std::make_shared<int>(1);
      CHECK(target.offer_native(pts, frame));
    };
    auto strict_stream = std::make_shared<Stream>(Input{"strict", 2, 2}, memory);
    NativeInput strict(strict_stream, std::make_shared<NativeInputQueue>());
    early_offer(*strict_stream, 900.04);
    CHECK(!strict.stage(advance_clock));  // Zero advance keeps strict PTS staging.
    strict.suspend();

    auto early_stream = std::make_shared<Stream>(Input{"advanced", 2, 2}, memory);
    NativeInput early(early_stream, std::make_shared<NativeInputQueue>(), std::string{}, 50000000);
    early_offer(*early_stream, 900.04);
    early_offer(*early_stream, 900.2);
    const auto advanced = early.stage(advance_clock);
    CHECK(advanced);
    CHECK(!early.stage(advance_clock));  // Beyond the advance, the frame still waits.
    early.complete(*advanced, true);
    CHECK(early_stream->current_pts() == 900.04);  // The genuine PTS is retained.
    early.suspend();
    CHECK(memory->native_frames == 0);
  }
  std::cout << "native source ownership: distinct tokens, ordered receipts, reject, reset, "
               "suspension, due-time and presentation-advance checks passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
