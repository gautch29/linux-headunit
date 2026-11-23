// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_service.hpp>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <limits>
#include <algorithm>

namespace headunit::theme::vehicle {
namespace {
using opack::Array;
using opack::Dictionary;
using opack::Value;
constexpr std::size_t max_signals = 512;
constexpr std::size_t max_value_bytes = 1024;
constexpr std::int32_t unsupported = -6710, not_found = -6727;
constexpr std::int32_t parameter_error = -6705, type_error = -6756, malformed = -6737;
Value key(std::string_view s) {
  return Value{std::string(s)};
}
bool text_equals(const Value& v, std::string_view text) {
  const auto* s = std::get_if<std::string>(&v.data);
  return s && *s == text;
}
const Value* field(const Value& v, std::string_view k) {
  const auto* d = std::get_if<Dictionary>(&v.data);
  if (!d) throw std::invalid_argument("configuration dictionary required");
  for (const auto& [a, b] : *d)
    if (text_equals(a, k)) return &b;
  return nullptr;
}
const Value& required(const Value& v, std::string_view k) {
  const auto* p = field(v, k);
  if (!p) throw std::invalid_argument("missing configuration field");
  return *p;
}
bool boolean(const Value& v, std::string_view k) {
  const auto* p = field(v, k);
  if (!p) return false;
  const auto* b = std::get_if<bool>(&p->data);
  if (!b) throw std::invalid_argument("configuration boolean required");
  return *b;
}
std::int64_t iid(const Value& v) {
  const auto* n = std::get_if<std::int64_t>(&required(v, "iid").data);
  if (!n || *n < 0) throw std::invalid_argument("invalid configuration IID");
  return *n;
}
const Array& array(const Value& v) {
  const auto* a = std::get_if<Array>(&v.data);
  if (!a) throw std::invalid_argument("configuration array required");
  return *a;
}
Array& children(Value& v, std::string_view k) {
  auto& d = std::get<Dictionary>(v.data);
  for (auto& [a, b] : d)
    if (text_equals(a, k)) return std::get<Array>(b.data);
  throw std::logic_error("validated configuration lost children");
}
void put(Value& v, std::string_view k, const Value& value) {
  auto& d = std::get<Dictionary>(v.data);
  for (auto& [a, b] : d)
    if (text_equals(a, k)) {
      b = value;
      return;
    }
  d.emplace_back(key(k), value);
}
std::set<std::int64_t> requested(const Value& v) {
  const auto* a = std::get_if<Array>(&v.data);
  if (!a) throw type_error;
  if (a->empty() || a->size() > max_signals) throw malformed;
  std::set<std::int64_t> ids;
  for (const auto& e : *a) {
    const auto* n = std::get_if<std::int64_t>(&e.data);
    if (!n) throw type_error;
    ids.insert(*n);
  }
  return ids;
}
bool wildcard(const Value& v) {
  return text_equals(v, "*");
}
}  // namespace

Service::Service(Value config, std::vector<SignalPolicy> policies,
                 std::map<std::int64_t, Value> fixed, std::vector<WriteBinding> writers,
                 std::vector<ConfirmationBinding> confirmations)
    : configuration_(std::move(config)) {
  // This also verifies uniqueness of dictionary keys, nesting and total size.
  if (opack::encode(configuration_).size() > max_packet_bytes / 4)
    throw std::invalid_argument("configuration too large");
  if (policies.size() > max_signals) throw std::invalid_argument("too many signals");
  std::map<std::int64_t, SignalPolicy> by_id;
  for (const auto& p : policies) {
    if (p.iid < 0 || p.max_age_ms == 0 || !by_id.emplace(p.iid, p).second ||
        (p.minimum && !std::isfinite(*p.minimum)) || (p.maximum && !std::isfinite(*p.maximum)) ||
        (p.minimum && p.maximum && *p.minimum > *p.maximum) ||
        ((p.minimum || p.maximum) && p.kind != ValueKind::integer && p.kind != ValueKind::real))
      throw std::invalid_argument("invalid signal policy");
  }
  if (writers.size() > max_signals) throw std::invalid_argument("too many writers");
  for (auto& writer : writers)
    if (writer.iid < 0 || !writer.apply ||
        !writers_.emplace(writer.iid, std::move(writer.apply)).second)
      throw std::invalid_argument("invalid or duplicate write binding");
  if (confirmations.size() > max_signals)
    throw std::invalid_argument("too many confirmation bindings");
  for (auto& binding : confirmations)
    if (binding.iid < 0 || !binding.start || writers_.contains(binding.iid) ||
        !confirmations_.emplace(binding.iid, std::move(binding)).second)
      throw std::invalid_argument("invalid or duplicate confirmation binding");
  std::set<std::int64_t> ids;
  auto unique = [&](const Value& v) {
    const auto id = iid(v);
    if (!ids.insert(id).second) throw std::invalid_argument("duplicate configuration IID");
    return id;
  };
  for (const auto& a : array(required(configuration_, "accessories"))) {
    unique(a);
    for (const auto& s : array(required(a, "services"))) {
      unique(s);
      if (const auto* c = field(s, "controls"); c && !array(*c).empty())
        throw std::invalid_argument("read-only service cannot advertise controls");
      for (const auto& c : array(required(s, "characteristics"))) {
        const auto id = unique(c);
        const auto p = by_id.find(id);
        const bool writable = boolean(c, "writable") || boolean(c, "oemWritable");
        if (p == by_id.end() || field(c, "initialValue") ||
            writable != (writers_.contains(id) || confirmations_.contains(id)))
          throw std::invalid_argument("characteristic needs a matching policy and write binding");
        signals_.emplace(id, Signal{p->second, boolean(c, "supportsInvalid"), {}, {}});
      }
    }
  }
  if (signals_.size() != by_id.size()) throw std::invalid_argument("policy absent from profile");
  for (const auto& [id, write] : writers_)
    if (!signals_.contains(id)) throw std::invalid_argument("write binding absent from profile");
  for (const auto& [id, binding] : confirmations_) {
    (void)binding;
    if (!signals_.contains(id))
      throw std::invalid_argument("confirmation binding absent from profile");
  }
  if (fixed.size() > max_signals)
    throw std::invalid_argument("too many fixed configuration values");
  for (auto& [id, value] : fixed) {
    if (writers_.contains(id) || confirmations_.contains(id))
      throw std::invalid_argument("fixed configuration cannot be writable");
    publish(id, std::move(value), 0, 0);  // Identical type, size and range validation.
    auto& signal = signals_.at(id);
    signal.fixed = true;
    signal.sampled_ms.reset();
  }
  for (auto& a : children(configuration_, "accessories"))
    for (auto& s : children(a, "services"))
      for (auto& c : children(s, "characteristics"))
        if (signals_.at(iid(c)).fixed) put(c, "mutable", Value{false});
  last_now_.reset();
}

void Service::check_time(std::uint64_t now) {
  if (last_now_ && now < *last_now_) throw std::invalid_argument("monotonic clock moved backwards");
  last_now_ = now;
}

void Service::validate_value(const Signal& s, const Value& value) const {
  bool valid = false;
  std::optional<double> number;
  switch (s.policy.kind) {
    case ValueKind::boolean:
      valid = std::holds_alternative<bool>(value.data);
      break;
    case ValueKind::integer:
      if (auto* n = std::get_if<std::int64_t>(&value.data)) {
        valid = true;
        number = *n;
      }
      break;
    case ValueKind::real:
      if (auto* n = std::get_if<double>(&value.data)) {
        valid = std::isfinite(*n);
        number = *n;
      }
      break;
    case ValueKind::string:
      valid = std::holds_alternative<std::string>(value.data);
      break;
    case ValueKind::bytes:
      valid = std::holds_alternative<Bytes>(value.data);
      break;
    case ValueKind::array:
      valid = std::holds_alternative<opack::Array>(value.data);
      break;
  }
  if (!valid || (number && s.policy.minimum && *number < *s.policy.minimum) ||
      (number && s.policy.maximum && *number > *s.policy.maximum) ||
      opack::encode(value).size() > max_value_bytes)
    throw std::invalid_argument("value violates signal policy");
}

Service::Reading Service::reading(std::int64_t id, std::uint64_t now) const {
  const auto it = signals_.find(id);
  if (it == signals_.end()) return {{}, not_found};
  const auto& s = it->second;
  if (s.fixed) return {s.value, {}};
  if (s.value && s.sampled_ms && now >= *s.sampled_ms && now - *s.sampled_ms < s.policy.max_age_ms)
    return {s.value, {}};
  // Null is the invalid value only for characteristics that advertise support.
  if (s.supports_invalid) return {Value{}, {}};
  return {{}, not_found};
}
std::map<std::int64_t, Service::Reading> Service::snapshot(std::span<const std::int64_t> ids,
                                                           std::uint64_t now) {
  if (ids.size() > max_signals) throw std::invalid_argument("too many local vehicle reads");
  check_time(now);
  std::map<std::int64_t, Reading> result;
  for (const auto id : ids)
    result.emplace(id, reading(id, now));
  return result;
}

void Service::publish(std::int64_t id, Value value, std::uint64_t sampled, std::uint64_t now) {
  auto it = signals_.find(id);
  if (it == signals_.end()) throw std::invalid_argument("unknown signal IID");
  auto& s = it->second;
  if (s.fixed) throw std::invalid_argument("fixed configuration cannot be published as a sample");
  if (sampled > now || (s.sampled_ms && sampled <= *s.sampled_ms))
    throw std::invalid_argument("future or out-of-order sample");
  validate_value(s, value);
  check_time(now);
  s.value = std::move(value);
  s.sampled_ms = sampled;
}

void Service::publish_batch(std::vector<Publication> publications, std::uint64_t now) {
  if (publications.size() > max_signals)
    throw std::invalid_argument("too many signal publications");
  if (last_now_ && now < *last_now_) throw std::invalid_argument("monotonic clock moved backwards");
  std::set<std::int64_t> seen;
  std::vector<Signal*> targets;
  targets.reserve(publications.size());
  for (const auto& publication : publications) {
    const auto it = signals_.find(publication.iid);
    if (it == signals_.end()) throw std::invalid_argument("unknown signal IID");
    if (!seen.insert(publication.iid).second)
      throw std::invalid_argument("duplicate signal publication");
    const auto& signal = it->second;
    if (signal.fixed)
      throw std::invalid_argument("fixed configuration cannot be published as a sample");
    if (publication.value && !publication.sampled_ms)
      throw std::invalid_argument("signal value requires an acquisition timestamp");
    if (publication.sampled_ms) {
      const auto sampled = *publication.sampled_ms;
      if (sampled > now ||
          (signal.sampled_ms &&
           (publication.value ? sampled <= *signal.sampled_ms : sampled < *signal.sampled_ms)))
        throw std::invalid_argument("future or out-of-order publication");
    }
    if (publication.value) validate_value(signal, *publication.value);
    targets.push_back(&it->second);
  }
  // All allocation, encoding, policy and ordering checks have finished. The
  // commit cannot throw halfway through a coherent group of source readings.
  static_assert(std::is_nothrow_move_assignable_v<std::optional<Value>>);
  static_assert(std::is_nothrow_copy_assignable_v<std::optional<std::uint64_t>>);
  for (std::size_t index = 0; index < publications.size(); ++index) {
    auto& signal = *targets[index];
    auto& publication = publications[index];
    signal.value = std::move(publication.value);
    if (publication.sampled_ms) signal.sampled_ms = publication.sampled_ms;
  }
  last_now_ = now;
}

void Service::invalidate(std::int64_t id, std::uint64_t sampled, std::uint64_t now) {
  auto it = signals_.find(id);
  if (it == signals_.end()) throw std::invalid_argument("unknown signal IID");
  if (it->second.fixed)
    throw std::invalid_argument("fixed configuration cannot be invalidated as a sample");
  if (sampled > now || (it->second.sampled_ms && sampled < *it->second.sampled_ms))
    throw std::invalid_argument("future or out-of-order invalidation");
  check_time(now);
  it->second.value.reset();
  it->second.sampled_ms = sampled;
}

void Service::withdraw(std::int64_t id, std::uint64_t now) {
  auto it = signals_.find(id);
  if (it == signals_.end()) throw std::invalid_argument("unknown signal IID");
  if (it->second.fixed) throw std::invalid_argument("fixed configuration cannot be withdrawn");
  check_time(now);
  it->second.value.reset();
}

void Service::invalidate_dynamic(std::uint64_t now) {
  check_time(now);
  for (auto& [id, signal] : signals_) {
    (void)id;
    if (signal.fixed) continue;
    signal.value.reset();
    signal.sampled_ms = now;
  }
}

void Service::append(Command& c, std::int64_t id, const Reading& r) const {
  auto& target = r.error ? c.errors : c.values;
  if (!target) target = Value{Dictionary{}};
  std::get<Dictionary>(target->data)
      .emplace_back(Value{id}, r.error ? Value{std::int64_t(*r.error)} : *r.value);
}

Value Service::configuration(std::uint64_t now) const {
  auto result = configuration_;
  for (auto& a : children(result, "accessories"))
    for (auto& s : children(a, "services"))
      for (auto& c : children(s, "characteristics")) {
        const auto r = reading(iid(c), now);
        // Binary-plist /info has no null value. Unavailable readings have no
        // initialValue; supportsInvalid still permits OPACK null read/updates.
        if (r.value && !std::holds_alternative<std::monostate>(r.value->data))
          put(c, "initialValue", *r.value);
      }
  return result;
}

Value Service::configuration_snapshot(std::uint64_t now) const {
  if (last_now_ && now < *last_now_) throw std::invalid_argument("monotonic clock moved backwards");
  return configuration(now);
}

std::optional<Bytes> Service::handle(std::span<const std::uint8_t> bytes, std::uint64_t now,
                                     std::uint64_t response_owner) {
  const auto request = decode_incoming(bytes);
  check_time(now);
  const auto& c = request.command;
  if (!c.name.ends_with("Request")) return std::nullopt;
  Command reply{"generalError", c.transaction, {}, {}, unsupported};
  if (request.plugin_id != 0) return encode_outgoing(reply, request.plugin_id);
  if (c.name == "configRequest") {
    reply.name = c.transaction ? "configResponse" : "configNotify";
    reply.error.reset();
    reply.values = configuration(now);
  } else if (c.name == "registerRequest" || c.name == "unregisterRequest" ||
             c.name == "readRequest") {
    reply.name = c.name.substr(0, c.name.size() - 7) + "Response";
    reply.error.reset();
    try {
      if (!c.transaction || !c.values) throw parameter_error;
      std::set<std::int64_t> ids;
      const bool all = wildcard(*c.values);
      if (all && c.name != "readRequest") {
        if (c.name == "unregisterRequest")
          ids = subscriptions_;
        else
          for (const auto& [id, signal] : signals_) {
            (void)signal;
            ids.insert(id);
          }
      } else {
        if (c.name != "readRequest" && std::holds_alternative<std::string>(c.values->data))
          throw std::int32_t(-6735);
        ids = requested(*c.values);
      }
      if (c.name == "registerRequest") {
        auto next = all ? std::set<std::int64_t>{} : subscriptions_;
        next.insert(ids.begin(), ids.end());
        if (next.size() > max_signals) throw malformed;
        subscriptions_ = std::move(next);
        if (all) delivered_.clear();
      }
      for (const auto id : ids) {
        if (c.name == "unregisterRequest") {
          subscriptions_.erase(id);
          delivered_.erase(id);
        } else {
          const auto r = reading(id, now);
          append(reply, id, r);
          if (c.name == "registerRequest") delivered_[id] = r;
        }
      }
    } catch (std::int32_t error) {
      reply.error = error;
    }
  } else if (c.name == "writeRequest") {
    reply.name = "writeResponse";
    if (writers_.empty() && confirmations_.empty())
      return encode_outgoing(reply, request.plugin_id);
    reply.error.reset();
    if (!c.transaction || !c.values)
      reply.error = parameter_error;
    else if (const auto* values = std::get_if<Dictionary>(&c.values->data);
             !values || values->empty() || values->size() > 32)
      reply.error = malformed;
    else {
      std::set<std::int64_t> ids;
      // Reject structural ambiguity before any provider callback runs. Valid
      // batches have independent per-IID results, not an actuator transaction.
      for (const auto& [key, value] : *values) {
        (void)value;
        const auto* id = std::get_if<std::int64_t>(&key.data);
        if (!id) {
          reply.error = type_error;
          break;
        }
        if (!ids.insert(*id).second) {
          reply.error = malformed;
          break;
        }
      }
      if (!reply.error) {
        const bool can_defer =
            response_owners_.contains(response_owner) && !batches_.contains(response_owner) &&
            now <= std::numeric_limits<std::uint64_t>::max() - confirmation_wait_ms;
        Batch batch{reply, now, can_defer ? now + confirmation_wait_ms : now, {}};
        if (can_defer) batch.operations.reserve(32);
        Dictionary applied_values, errors;
        for (const auto& [key, value] : *values) {
          const auto id = std::get<std::int64_t>(key.data);
          std::optional<std::int32_t> error;
          if (!signals_.contains(id))
            error = not_found;
          else if (!writers_.contains(id) && !confirmations_.contains(id))
            error = unsupported;
          else {
            try {
              auto& signal = signals_.at(id);
              validate_value(signal, value);
              if (writing(id)) throw std::invalid_argument("write already pending");
              if (confirmations_.contains(id)) {
                if (!can_defer)
                  throw std::invalid_argument("confirmation response owner unavailable");
                auto pending = confirmations_.at(id).start(value, now, batch.deadline_ms);
                if (!pending) throw std::invalid_argument("owner rejected confirmation request");
                if (!pending->poll || !pending->cancel) {
                  cancel(*pending);
                  throw std::invalid_argument("incomplete confirmation operation");
                }
                batch.operations.push_back({id, std::move(*pending)});
                continue;
              }
              const auto applied = writers_.at(id)(value);
              validate_value(signal, applied);
              signal.value = applied;
              signal.sampled_ms = now;
              applied_values.emplace_back(key, applied);
            } catch (const std::exception&) {
              error = parameter_error;
            }
          }
          if (error) errors.emplace_back(key, Value{std::int64_t(*error)});
        }
        if (!applied_values.empty()) reply.values = Value{std::move(applied_values)};
        if (!errors.empty()) reply.errors = Value{std::move(errors)};
        if (!batch.operations.empty()) {
          batch.reply = std::move(reply);
          batches_.emplace(response_owner, std::move(batch));
          return {};
        }
      }
    }
  } else if (c.name == "controlRequest")
    reply.name = "controlResponse";
  // Controls and unbound writes never mutate this model or claim success.
  return encode_outgoing(reply, request.plugin_id);
}

Service::~Service() {
  for (auto& [owner, batch] : batches_) {
    (void)owner;
    for (auto& operation : batch.operations)
      cancel(operation.write);
  }
}
void Service::cancel(PendingWrite& write) noexcept {
  write.retire();
}
std::uint64_t Service::attach_response_owner() {
  if (confirmations_.empty()) return 0;
  if (response_owners_.size() >= 2 ||
      next_response_owner_ == std::numeric_limits<std::uint64_t>::max())
    throw std::invalid_argument("vehicle response owner capacity exhausted");
  const auto owner = ++next_response_owner_;
  response_owners_.insert(owner);
  return owner;
}
bool Service::pending_response(std::uint64_t owner) const {
  return batches_.contains(owner);
}
bool Service::writing(std::int64_t iid) const {
  for (const auto& [owner, batch] : batches_) {
    (void)owner;
    for (const auto& operation : batch.operations)
      if (operation.iid == iid) return true;
  }
  return false;
}
void Service::retire_response_owner(std::uint64_t owner) noexcept {
  if (const auto batch = batches_.find(owner); batch != batches_.end()) {
    for (auto& operation : batch->second.operations)
      cancel(operation.write);
    batches_.erase(batch);
  }
  response_owners_.erase(owner);
}
std::optional<Bytes> Service::complete_response(std::uint64_t owner, std::uint64_t now) {
  check_time(now);
  const auto found = batches_.find(owner);
  if (found == batches_.end()) return {};
  auto& batch = found->second;
  for (auto operation = batch.operations.begin(); operation != batch.operations.end();) {
    bool done = now >= batch.deadline_ms;
    WriteCompletion result;
    {
      try {
        result = operation->write.poll(now);
        done = done || result.status != WriteCompletion::Status::pending;
      } catch (...) {
        done = true;
        result.status = WriteCompletion::Status::rejected;
      }
    }
    if (!done) {
      ++operation;
      continue;
    }
    Reading outcome{{}, parameter_error};
    if (now < batch.deadline_ms && result.status == WriteCompletion::Status::applied &&
        result.value && result.sampled_ms && *result.sampled_ms >= batch.service_tick_ms &&
        *result.sampled_ms <= now) {
      try {
        auto& signal = signals_.at(operation->iid);
        validate_value(signal, *result.value);
        // An independent newer observation keeps authority. Equal-date conflict
        // cannot rewrite that observation; old completion never renews its age.
        if (signal.sampled_ms && *signal.sampled_ms == *result.sampled_ms &&
            (!signal.value || *signal.value != *result.value))
          throw std::invalid_argument("conflicting confirmation date");
        if (!signal.sampled_ms || *signal.sampled_ms < *result.sampled_ms) {
          signal.value = result.value;
          signal.sampled_ms = result.sampled_ms;
        }
        outcome = {result.value, {}};
      } catch (const std::exception&) {
      }
    }
    append(batch.reply, operation->iid, outcome);
    cancel(operation->write);
    operation = batch.operations.erase(operation);
  }
  if (!batch.operations.empty()) return {};
  auto bytes = encode_outgoing(batch.reply, 0);
  batches_.erase(found);
  return bytes;
}

void Service::set_update_groups(const std::vector<std::vector<std::int64_t>>& groups) {
  std::map<std::int64_t, std::vector<std::int64_t>> next;
  for (const auto& group : groups) {
    if (group.size() < 2 || group.size() > 512)
      throw std::invalid_argument("vehicle update group needs 2..512 signals");
    for (const auto id : group) {
      if (!signals_.contains(id)) throw std::invalid_argument("unknown vehicle update group IID");
      if (!next.emplace(id, group).second)
        throw std::invalid_argument("vehicle signal belongs to two update groups");
    }
  }
  update_groups_ = std::move(next);
}

std::optional<Bytes> Service::updates(std::uint64_t now) {
  check_time(now);
  Command c{"updateNotify", {}, {}, {}, {}};
  std::set<std::int64_t> changed;
  for (const auto id : subscriptions_) {
    const auto r = reading(id, now);
    const auto old = delivered_.find(id);
    if (old == delivered_.end() || old->second != r) changed.insert(id);
  }
  // Resend unchanged subscribed siblings so a composite updater never sees
  // half of its inputs, e.g. a gear position without its selector.
  auto outgoing = changed;
  for (const auto id : changed) {
    const auto group = update_groups_.find(id);
    if (group == update_groups_.end()) continue;
    for (const auto member : group->second) {
      if (subscriptions_.contains(member)) outgoing.insert(member);
    }
  }
  for (const auto id : outgoing)
    append(c, id, reading(id, now));
  if (!c.values && !c.errors) return std::nullopt;
  auto bytes = encode_outgoing(c, 0);
  for (const auto id : subscriptions_)
    delivered_[id] = reading(id, now);
  return bytes;
}

void Service::disconnect() {
  while (!batches_.empty()) {
    for (auto& operation : batches_.begin()->second.operations)
      cancel(operation.write);
    batches_.erase(batches_.begin());
  }
  subscriptions_.clear();
  delivered_.clear();
  for (auto& [id, s] : signals_) {
    (void)id;
    if (!s.fixed) {
      s.value.reset();
      s.sampled_ms.reset();
      s.visibility_capture_ms.reset();
    }
  }
  last_now_.reset();
}
bool Service::timed(std::int64_t iid) const {
  const auto it = signals_.find(iid);
  return it != signals_.end() && !it->second.fixed;
}
std::optional<SignalPolicy> Service::policy(std::int64_t iid) const {
  const auto it = signals_.find(iid);
  return it == signals_.end() ? std::nullopt : std::optional(it->second.policy);
}
void Service::deactivate_boolean(std::int64_t iid, std::uint64_t now) {
  deactivate_boolean(iid, now, now);
}
void Service::deactivate_boolean(std::int64_t iid, std::uint64_t sampled, std::uint64_t now) {
  auto it = signals_.find(iid);
  if (it == signals_.end() || it->second.fixed || it->second.policy.kind != ValueKind::boolean)
    throw std::invalid_argument("local deactivation requires a timed Boolean");
  if (sampled > now || (it->second.sampled_ms && sampled < *it->second.sampled_ms))
    throw std::invalid_argument("invalid local deactivation date");
  check_time(now);
  it->second.value = Value{false};
  it->second.sampled_ms = sampled;
}
void Service::publish_request_intent(std::int64_t iid, std::uint64_t renewed, std::uint64_t now) {
  auto it = signals_.find(iid);
  if (it == signals_.end() || it->second.fixed || it->second.policy.kind != ValueKind::boolean ||
      !writers_.contains(iid))
    throw std::invalid_argument("content intent requires a timed dismissible Boolean");
  auto& s = it->second;
  if (renewed > now || (s.visibility_capture_ms && renewed <= *s.visibility_capture_ms))
    throw std::invalid_argument("future or repeated content intent acquisition");
  check_time(now);
  s.value = Value{true};
  s.sampled_ms = s.visibility_capture_ms = renewed;
}
void Service::publish_visibility_capture(std::int64_t iid, std::uint64_t captured,
                                         std::uint64_t now) {
  auto it = signals_.find(iid);
  if (it == signals_.end() || it->second.fixed || it->second.policy.kind != ValueKind::boolean ||
      writers_.contains(iid))
    throw std::invalid_argument("camera visibility requires a read-only timed Boolean");
  auto& s = it->second;
  if (captured > now || (s.visibility_capture_ms && captured <= *s.visibility_capture_ms))
    throw std::invalid_argument("future or repeated camera acquisition");
  check_time(now);
  s.value = Value{true};
  s.sampled_ms = s.visibility_capture_ms = captured;
}
}  // namespace headunit::theme::vehicle
