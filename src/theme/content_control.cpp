// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/content_control.hpp>
#include <algorithm>
#include <charconv>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace headunit::theme::vehicle {
namespace {
void check(bool value, const char* why) {
  if (!value) throw std::invalid_argument(why);
}
struct File {
  int fd = -1;
  ~File() {
    if (fd >= 0) ::close(fd);
  }
};
bool hex_nonce(std::string_view value) {
  return value.size() == 32 && value.find_first_not_of("0123456789abcdef") == value.npos;
}
std::uint64_t decimal(std::string_view value) {
  std::uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  check(!value.empty() && (value.size() == 1 || value.front() != '0') && parsed.ec == std::errc{} &&
            parsed.ptr == value.data() + value.size(),
        "invalid content request integer");
  return result;
}
struct Request {
  std::uint64_t sequence = 0, renewed = 0, expires = 0;
  std::string identifier;
};
class ContentProvider final : public Provider {
  std::shared_ptr<Provider> base_;
  std::shared_ptr<ContentStateSnapshot> state_;
  ContentRequestCapability configuration_;
  std::string nonce_;
  std::uint64_t generation_, watermark_ = 0, retired_sequence_ = 0, decision_ = 0;
  std::uint64_t seen_dismissals_ = 0;
  std::optional<std::uint64_t> polled_;
  std::optional<Request> request_;
  bool active_ = false;
  std::string url_;
  void withdraw(std::uint64_t now) {
    if (request_) retired_sequence_ = std::max(retired_sequence_, request_->sequence);
    request_.reset();
    decision_ = now;
    active_ = false;
    if (state_) state_->update({false, url_, decision_, 0});
  }
  void poll(std::uint64_t now) {
    if (polled_) check(now >= *polled_, "content owner clock reversed");
    if (state_) {
      const auto dismissed = state_->dismissal_generation();
      if (dismissed != seen_dismissals_) {
        seen_dismissals_ = dismissed;
        withdraw(now);
      }
    }
    if (request_ && now >= request_->expires) withdraw(now);
    if (polled_ && now - *polled_ < 100) return;
    polled_ = now;
    try {
      const std::filesystem::path path(configuration_.control_file);
      File directory{::open(path.parent_path().c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
      struct stat parent{}, named_parent{};
      check(directory.fd >= 0 && !::fstat(directory.fd, &parent) && S_ISDIR(parent.st_mode) &&
                parent.st_uid == ::getuid() && !(parent.st_mode & 0077) &&
                !::lstat(path.parent_path().c_str(), &named_parent) &&
                named_parent.st_dev == parent.st_dev && named_parent.st_ino == parent.st_ino,
            "unsafe content request directory");
      File input{::openat(directory.fd, path.filename().c_str(),
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
      if (input.fd < 0) {
        withdraw(now);
        return;
      }
      struct stat before{}, after{}, named{};
      check(!::fstat(input.fd, &before) && S_ISREG(before.st_mode) && before.st_uid == ::getuid() &&
                !(before.st_mode & 0077) && before.st_nlink == 1 && before.st_size > 0 &&
                before.st_size <= 384,
            "unsafe content request file");
      char bytes[385]{};
      const auto count = ::read(input.fd, bytes, sizeof(bytes));
      check(count == before.st_size && !::fstat(input.fd, &after) &&
                before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
                before.st_size == after.st_size && before.st_mode == after.st_mode &&
                before.st_uid == after.st_uid && before.st_nlink == after.st_nlink &&
                !::fstatat(directory.fd, path.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) &&
                named.st_dev == before.st_dev && named.st_ino == before.st_ino &&
                !::lstat(path.parent_path().c_str(), &named_parent) &&
                named_parent.st_dev == parent.st_dev && named_parent.st_ino == parent.st_ino,
            "content request changed during read");
#ifdef __APPLE__
      check(before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
                before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
                before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
                before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec,
            "content request changed during read");
#else
      check(before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
                before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
                before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
                before.st_ctim.tv_nsec == after.st_ctim.tv_nsec,
            "content request changed during read");
#endif
      std::istringstream fields(std::string(bytes, static_cast<std::size_t>(count)));
      std::string header, nonce, generation, sequence, renewed, expires, identifier, extra;
      check(bool(std::getline(fields, header)) && bool(std::getline(fields, identifier)) &&
                !std::getline(fields, extra) && bytes[count - 1] == '\n',
            "invalid content request framing");
      std::istringstream h(header);
      std::string version;
      check(bool(h >> version >> nonce >> generation >> sequence >> renewed >> expires) &&
                !(h >> extra) && version == "HUC1" && hex_nonce(nonce),
            "invalid content request header");
      const auto g = decimal(generation);
      Request next{decimal(sequence), decimal(renewed), decimal(expires), identifier};
      check(g && next.sequence && next.renewed <= now && next.expires > now &&
                next.expires > next.renewed && next.expires - next.renewed == 2000,
            "invalid or expired content request lease");
      if (nonce != nonce_ || g != generation_) {
        withdraw(now);
        return;
      }
      if (next.sequence < watermark_ || next.sequence <= retired_sequence_) {
        withdraw(now);
        return;
      }
      if (request_ && next.sequence == request_->sequence)
        check(next.identifier == request_->identifier && next.renewed >= request_->renewed,
              "content command changed without a new sequence");
      watermark_ = next.sequence;
      if (identifier == "-") {
        retired_sequence_ = next.sequence;
        withdraw(now);
        return;
      }
      const auto page = std::find_if(configuration_.pages.begin(), configuration_.pages.end(),
                                     [&](const auto& p) { return p.identifier == identifier; });
      if (page == configuration_.pages.end()) {
        retired_sequence_ = next.sequence;
        throw std::invalid_argument("content page is not declared");
      }
      request_ = std::move(next);
      active_ = true;
      url_ = page->url;
      decision_ = request_->renewed;
      if (state_) state_->update({true, url_, decision_, request_->expires, request_->sequence});
    } catch (const std::exception&) {
      withdraw(now);
    }
  }

 public:
  ContentProvider(std::shared_ptr<Provider> base, ContentRequestCapability config,
                  std::string nonce, std::uint64_t generation,
                  std::shared_ptr<ContentStateSnapshot> state)
      : base_(std::move(base)),
        state_(std::move(state)),
        configuration_(std::move(config)),
        nonce_(std::move(nonce)),
        generation_(generation) {
    check(base_ && hex_nonce(nonce_) && generation_ && !configuration_.pages.empty(),
          "content requests require one real connection publisher and declared pages");
    validate_content_request_endpoint(configuration_.control_file);
    if (state_)
      check(state_->capability().control_file == configuration_.control_file &&
                state_->capability().on_binding == configuration_.on_binding &&
                state_->capability().url_binding == configuration_.url_binding,
            "content snapshot does not belong to this capability");
  }
  std::optional<Sample> read(std::string_view key, std::uint64_t now) override {
    if (key != configuration_.on_binding && key != configuration_.url_binding)
      return base_->read(key, now);
    poll(now);
    if (key == configuration_.on_binding) {
      // OFF is a local withdrawal decision, not a refreshed sensor measurement.
      const auto visible = active_ && (!state_ || state_->read(now).on);
      return Sample{opack::Value{visible}, visible ? decision_ : now};
    }
    if (url_.empty()) return {};
    return Sample{opack::Value{url_}, decision_};
  }
  opack::Value apply(std::string_view key, const opack::Value& value, std::uint64_t now) override {
    if (key == configuration_.url_binding)
      throw std::invalid_argument("content URL is session-owned");
    if (key != configuration_.on_binding) return base_->apply(key, value, now);
    check(value == opack::Value{false}, "phone may only dismiss requested content");
    poll(now);
    withdraw(now);
    return opack::Value{false};
  }
};
}  // namespace
ContentStateSnapshot::ContentStateSnapshot(ContentRequestCapability capability)
    : capability_(std::move(capability)) {
  check(!capability_.pages.empty(), "content snapshot needs declared pages");
}
void ContentStateSnapshot::update(State value) {
  std::lock_guard lock(mutex_);
  if (closed_) return;
  if (value.on && retired_command_ && value.command_sequence <= *retired_command_) return;
  check(value.decided_ms >= state_.decided_ms, "content snapshot clock reversed");
  if (value.on) {
    check(value.expires_ms > value.decided_ms && value.expires_ms - value.decided_ms == 2000 &&
              std::any_of(capability_.pages.begin(), capability_.pages.end(),
                          [&](const auto& page) { return page.url == value.url; }),
          "content snapshot has no declared leased page");
  }
  state_ = std::move(value);
}
void ContentStateSnapshot::close(std::uint64_t now) {
  std::lock_guard lock(mutex_);
  state_.on = false;
  state_.expires_ms = 0;
  state_.decided_ms = std::max(state_.decided_ms, now);
  closed_ = true;
}
void ContentStateSnapshot::dismiss(std::uint64_t now) {
  std::lock_guard lock(mutex_);
  if (closed_) return;
  if (state_.on) retired_command_ = std::max(retired_command_.value_or(0), state_.command_sequence);
  state_.on = false;
  state_.expires_ms = 0;
  state_.decided_ms = std::max(state_.decided_ms, now);
  if (dismissals_ == std::numeric_limits<std::uint64_t>::max())
    closed_ = true;
  else
    ++dismissals_;
}
std::uint64_t ContentStateSnapshot::dismissal_generation() const {
  std::lock_guard lock(mutex_);
  return dismissals_;
}
void ContentStateSnapshot::allow_content(bool allowed) {
  std::lock_guard lock(mutex_);
  allowed_ = allowed;
}
ContentStateSnapshot::State ContentStateSnapshot::read(std::uint64_t now,
                                                       bool include_hidden) const {
  std::lock_guard lock(mutex_);
  auto result = state_;
  if ((!include_hidden && !allowed_) || result.decided_ms > now || !result.expires_ms ||
      now >= result.expires_ms) {
    result.on = false;
    result.decided_ms = now;
  }
  return result;
}
ContentLocalInput::ContentLocalInput(const LocalProfile& profile,
                                     std::shared_ptr<ContentStateSnapshot> state, std::uint64_t now)
    : source_(profile.source), state_(std::move(state)) {
  check(source_ && state_, "content input needs a local source and connection snapshot");
  const auto& page = state_->capability().pages.front();
  std::map<std::string, std::map<std::string, std::int64_t>> groups;
  for (const auto& binding : profile.bindings) {
    if (binding.path[0] != "automakerRequestContent" || binding.path[1] != "requestContent")
      continue;
    check(
        binding.instance && groups[*binding.instance].emplace(binding.path[2], binding.iid).second,
        "content input needs unique repeated service bindings");
    check(std::none_of(profile.vehicle_inputs.begin(), profile.vehicle_inputs.end(),
                       [&](const auto& input) { return input.iid == binding.iid; }),
          "content intent must not alias a car reading");
  }
  bool matched = false;
  for (const auto& [instance, fields] : groups) {
    (void)instance;
    const auto fixed = [&](const char* name, const opack::Value& expected) {
      const auto it = fields.find(name);
      if (it == fields.end() || source_->timed(it->second)) return false;
      const std::array<std::int64_t, 1> id{it->second};
      return source_->snapshot(id, now).at(id.front()).value == expected;
    };
    if (!fixed("displayPanelIdentifier", opack::Value{page.panel}) ||
        !fixed("displayZoneIdentifier", opack::Value{page.zone}))
      continue;
    check(!matched && fixed("userDismissible", opack::Value{true}) && fields.contains("on") &&
              fields.contains("contentURL"),
          "content input has ambiguous or incomplete zone bindings");
    on_ = fields.at("on");
    url_ = fields.at("contentURL");
    const auto on_policy = source_->policy(on_), url_policy = source_->policy(url_);
    check(source_->timed(on_) && source_->timed(url_) && on_policy && url_policy &&
              on_policy->kind == ValueKind::boolean && url_policy->kind == ValueKind::string &&
              on_policy->max_age_ms >= 2000 && url_policy->max_age_ms >= 2000,
          "content input needs timed intent fields covering the request lease");
    matched = true;
  }
  check(matched, "content request has no explicit local display-zone binding");
  source_->deactivate_boolean(on_, now);
  last_published_ = now;
}
void ContentLocalInput::pump(std::uint64_t now, bool allow_content) {
  const auto value = state_->read(now, true);
  if (!value.on || !allow_content) {
    state_->allow_content(false);
    source_->deactivate_boolean(on_, now);
    last_published_ = now;
    return;
  }
  // Frame ticks do not refresh an intent. Only the manager's real lease renewal
  // publishes a newer acquisition date; immediate OFF uses the visibility gate.
  if (last_published_ && value.decided_ms <= *last_published_) return;
  source_->publish_batch({{on_, opack::Value{true}, value.decided_ms},
                          {url_, opack::Value{value.url}, value.decided_ms}},
                         now);
  state_->allow_content(true);
  last_published_ = value.decided_ms;
}
ScriptView::Dismissals ContentLocalInput::dismissals() const {
  return {{on_, [state = state_, source = source_, id = on_](std::uint64_t now) {
             state->dismiss(now);
             source->deactivate_boolean(id, now);
           }}};
}
void validate_content_request_endpoint(std::string_view value) {
  const std::filesystem::path path(value);
  check(path.is_absolute() && path.lexically_normal() == path && !path.filename().empty() &&
            value.size() <= 128 && value.find('\0') == value.npos,
        "content request endpoint must be a bounded absolute private file");
  for (const auto& part : path)
    check(part != "." && part != "..", "unsafe content request endpoint");
  struct stat directory{};
  check(!::lstat(path.parent_path().c_str(), &directory) && S_ISDIR(directory.st_mode) &&
            directory.st_uid == ::getuid() && !(directory.st_mode & 0077),
        "content request parent must be a private display-account directory");
}
std::shared_ptr<Provider> content_request_provider(std::shared_ptr<Provider> base,
                                                   const ContentRequestCapability& config,
                                                   std::string nonce, std::uint64_t generation,
                                                   std::shared_ptr<ContentStateSnapshot> state) {
  return std::make_shared<ContentProvider>(std::move(base), config, std::move(nonce), generation,
                                           std::move(state));
}
}  // namespace headunit::theme::vehicle
