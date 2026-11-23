// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/sensor_input.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <stdexcept>

using namespace headunit::theme;
namespace v = vehicle;
namespace op = opack;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
op::Value text(const char* s) {
  return op::Value{std::string(s)};
}
op::Value number(std::int64_t n) {
  return op::Value{n};
}
op::Value object(std::initializer_list<op::DictionaryEntry> fields) {
  return op::Value{op::Dictionary(fields)};
}
op::Value list(std::initializer_list<op::Value> values) {
  return op::Value{op::Array(values)};
}
std::shared_ptr<v::ReadOnlyService> source() {
  auto characteristic = [](std::int64_t iid) {
    return object({{text("iid"), number(iid)}, {text("supportsInvalid"), op::Value{true}}});
  };
  const auto config = object(
      {{text("accessories"),
        list({object({{text("iid"), number(1)},
                      {text("services"),
                       list({object({{text("iid"), number(2)},
                                     {text("characteristics"),
                                      list({characteristic(3), characteristic(4)})}})})}})})}});
  return std::make_shared<v::ReadOnlyService>(
      config,
      std::vector<v::SignalPolicy>{{3, v::ValueKind::real, 100, 0, 300},
                                   {4, v::ValueKind::real, 100, 0, 300}},
      std::map<std::int64_t, op::Value>{{4, op::Value{220.0}}});
}
void send_record(int fd, op::Value value) {
  const auto bytes = op::encode(value);
  check(::send(fd, bytes.data(), bytes.size(), 0) == ssize_t(bytes.size()), "sensor send failed");
}
op::Value sample(std::int64_t time, op::Value value) {
  return object({{text("iid"), number(3)},
                 {text("sampledMs"), number(time)},
                 {text("value"), std::move(value)}});
}
void pair(int sockets[2]) {
  check(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0, "sensor socketpair failed");
}
int main() {
  try {
    int sockets[2];
    pair(sockets);
    auto model = source();
    v::SensorInput input(sockets[0], model);
    ::close(sockets[0]);
    const std::array<std::int64_t, 2> ids{3, 4};
    send_record(sockets[1], sample(1000, op::Value{25.5}));
    check(input.pump(1000) == 1 && input.active(), "fresh sensor sample was not accepted");
    auto readings = model->snapshot(ids, 1000);
    check(readings.at(3).value == op::Value{25.5} && readings.at(4).value == op::Value{220.0},
          "sensor sample or fixed gauge bound missing");
    send_record(sockets[1], object({{text("iid"), number(3)},
                                    {text("sampledMs"), number(1001)},
                                    {text("invalid"), op::Value{true}}}));
    check(input.pump(1001) == 1 && model->snapshot(ids, 1001).at(3).value == op::Value{},
          "sensor invalidation missing");
    send_record(sockets[1], sample(1002, op::Value{0.0}));
    check(input.pump(1002) == 1 && model->snapshot(ids, 1002).at(3).value == op::Value{0.0},
          "zero reading lost");
    send_record(sockets[1], sample(1002, op::Value{17.0}));  // Duplicate timestamp is rejected.
    check(input.pump(1003) == 0 && !input.active() && !input.failure().empty(),
          "out-of-order provider was not disabled");
    readings = model->snapshot(ids, 1003);
    check(readings.at(3).value == op::Value{} && readings.at(4).value == op::Value{220.0},
          "provider fault retained a timed reading or lost calibration");
    ::close(sockets[1]);

    pair(sockets);
    model = source();
    v::SensorInput eof(sockets[0], model);
    ::close(sockets[0]);
    send_record(sockets[1], sample(2000, op::Value{41.0}));
    check(eof.pump(2000) == 1, "second sensor sample missing");
    ::close(sockets[1]);
    check(eof.pump(2001) == 0 && !eof.active() &&
              model->snapshot(std::span(ids.data(), 1), 2001).at(3).value == op::Value{},
          "provider EOF retained speed");

    pair(sockets);
    model = source();
    v::SensorInput oversized(sockets[0], model);
    ::close(sockets[0]);
    std::array<std::uint8_t, 2049> large{};
    check(::send(sockets[1], large.data(), large.size(), 0) == ssize_t(large.size()),
          "oversized send failed");
    check(oversized.pump(3000) == 0 && !oversized.active(), "oversized sensor packet accepted");
    ::close(sockets[1]);

    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "stream socketpair failed");
    bool rejected = false;
    try {
      v::SensorInput wrong(sockets[0], source());
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "stream transport accepted as packet sensor input");
    ::close(sockets[0]);
    ::close(sockets[1]);
    std::cout
        << "sensor input: typed timed samples, invalidation, provider loss and bounds passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
