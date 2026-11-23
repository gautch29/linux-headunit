// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/layout_control.hpp>
#include <headunit/presentation_status.hpp>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <random>

namespace headunit::theme::script {
namespace {
bool uuid(std::string_view id) {
  if (id.size() != 36) return false;
  for (std::size_t i = 0; i < id.size(); ++i) {
    const auto c = id[i];
    if ((i == 8 || i == 13 || i == 18 || i == 23)
            ? c != '-'
            : !(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'F'))
      return false;
  }
  return true;
}
struct File {
  int fd;
  ~File() {
    if (fd >= 0) ::close(fd);
  }
};
bool palette_name(std::string_view name) {
  return !name.empty() && name.size() <= 64 &&
         name.find_first_not_of(
             "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") == name.npos;
}
bool atomic_write(const std::string& path, std::string_view text) {
  std::string temporary = path + ".XXXXXX";
  File output{::mkstemp(temporary.data())};
  if (output.fd < 0) return false;
  std::size_t at = 0;
  while (at < text.size()) {
    const auto count = ::write(output.fd, text.data() + at, text.size() - at);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      ::unlink(temporary.c_str());
      return false;
    }
    at += std::size_t(count);
  }
  const bool saved = !::rename(temporary.c_str(), path.c_str());
  ::unlink(temporary.c_str());
  return saved;
}
void quoted(std::ostream& out, std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  out << '"';
  for (const unsigned char c : value) {
    if (c == '"' || c == '\\')
      out << '\\' << char(c);
    else if (c < 32)
      out << "\\u00" << hex[c >> 4] << hex[c & 15];
    else
      out << char(c);
  }
  out << '"';
}
}  // namespace
LayoutControl::LayoutControl(std::string path, std::string preference_scope)
    : LayoutControl(std::move(path), std::move(preference_scope), Publisher{}) {}
LayoutControl::LayoutControl(std::string path, std::string preference_scope, Publisher publisher,
                             std::shared_ptr<PreferenceFile> preferences)
    : path_(std::move(path)), publisher_(std::move(publisher)) {
  // Standalone diagnostics have an explicitly unbound connection (zero), with
  // a fresh owner token. Managed receivers supply their launch/connection pair.
  if (publisher_.nonce.empty()) {
    publisher_.connection_generation = 0;
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned word = 0; word < 4; ++word) {
      const auto value = random();
      for (unsigned nibble = 0; nibble < 8; ++nibble)
        publisher_.nonce += hex[(value >> (nibble * 4)) & 15];
    }
  }
  if (publisher_.nonce.size() != 32 ||
      publisher_.nonce.find_first_not_of("0123456789abcdef") != publisher_.nonce.npos)
    throw std::invalid_argument("invalid layout publisher nonce");
  if (path_.size() > 4000 || !std::filesystem::path(path_).is_absolute() ||
      path_.find('\0') != path_.npos)
    throw std::invalid_argument("invalid layout control path");
  struct stat parent{};
  if (::stat(std::filesystem::path(path_).parent_path().c_str(), &parent) ||
      !S_ISDIR(parent.st_mode) || parent.st_uid != getuid() || (parent.st_mode & 0022))
    throw std::invalid_argument("layout control directory must be private and owned by this user");
  // A live button request never survives a VM restart, even when persistence
  // is disabled. Only a separately phone-confirmed pack preference can restore.
  read_request();
  attempted_stamp_ = desired_stamp_;
  if (preference_scope.empty()) return;
  if (preference_scope.size() > 160 ||
      preference_scope.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-:") !=
          preference_scope.npos ||
      std::filesystem::path(path_).filename().string().size() + preference_scope.size() > 210)
    throw std::invalid_argument("invalid verified cluster preference scope");
  preference_path_ = path_ + ".confirmed." + preference_scope;
  // A button request belongs to its live session. The profile supplies the
  // default; only the pack-scoped confirmation is a reconnect preference.
  preferences_ =
      preferences ? std::move(preferences) : std::make_shared<PreferenceFile>(preference_path_);
  palette_preferences_ = std::make_shared<PreferenceFile>(preference_path_ + ".palettes", 4096);
}
void LayoutControl::wait_for_preferences() {
  if (!preferences_) return;
  preferences_->wait();
  palette_preferences_->wait();
  refresh_preferences();
}
void LayoutControl::refresh_preferences() {
  if (!preferences_) return;
  const auto snapshot = preferences_->snapshot();
  // Admit both startup records together. A phone edit observed while the
  // history reader is delayed must not race with and then be overwritten by
  // that reader's initial snapshot.
  if (!snapshot.loaded || !palette_preferences_->snapshot().loaded) return;
  if (!preferences_loaded_) {
    preferences_loaded_ = true;
    preference_error_ = snapshot.error;
    if (snapshot.data) {
      const std::string_view text(*snapshot.data);
      const auto palette =
          text.size() >= 39 ? text.substr(37, text.size() - 38) : std::string_view{};
      if (text.size() < 37 || text.size() == 38 || text[36] != '\n' || !uuid(text.substr(0, 36)) ||
          text.back() != '\n' || (text.size() > 37 && !palette_name(palette))) {
        preference_error_ = "Invalid private saved cluster style";
      } else {
        restoring_ = confirmed_ = std::string(text.substr(0, 36));
        restoring_palette_ = confirmed_palette_ = std::string(palette);
      }
    }
  }
  if (submitted_preference_ && snapshot.completed >= submitted_preference_) {
    preference_error_ = snapshot.error;
    if (snapshot.error.empty()) {
      next_preference_attempt_ = {};
      confirmed_ = submitted_layout_;
      confirmed_palette_ = submitted_palette_;
      if (confirmed_candidate_ == submitted_layout_ && candidate_palette_ == submitted_palette_) {
        confirmed_candidate_.clear();
        candidate_palette_.clear();
      }
    }
    submitted_preference_ = 0;
  }
  refresh_palette_preferences();
}
void LayoutControl::refresh_palette_preferences() {
  if (!palette_preferences_) return;
  const auto snapshot = palette_preferences_->snapshot();
  if (!snapshot.loaded) return;
  if (!palette_preferences_loaded_) {
    palette_preferences_loaded_ = true;
    palette_preference_error_ = snapshot.error;
    if (snapshot.data) {
      const std::string_view text(*snapshot.data);
      bool valid = text.starts_with("HUP1\n") && text.size() > 5 && text.back() == '\n';
      std::vector<std::pair<std::string, std::string>> entries;
      std::size_t at = 5;
      while (valid && at < text.size()) {
        const auto end = text.find('\n', at);
        const auto line = text.substr(at, end - at);
        valid = end != text.npos && line.size() >= 37 && line[36] == ' ' &&
                uuid(line.substr(0, 36)) && (line.size() == 37 || palette_name(line.substr(37))) &&
                entries.size() < 32 &&
                std::none_of(
                    entries.begin(), entries.end(),
                    [&](const auto& entry) { return entry.first == line.substr(0, 36); });
        if (valid) entries.emplace_back(line.substr(0, 36), line.substr(37));
        at = end == text.npos ? text.size() : end + 1;
      }
      if (valid)
        palette_history_ = std::move(entries);
      else
        palette_preference_error_ = "Invalid private cluster colour history";
    }
    // The existing last-selection record remains authoritative for startup.
    // It also seeds history for a v1 installation; no migration write occurs
    // before a fresh qualified confirmation.
    if (!confirmed_.empty()) {
      remember_palette(confirmed_, confirmed_palette_);
      saved_palette_revision_ = 0;
      palette_candidate_layout_.clear();
      palette_candidate_.clear();
    }
  }
  if (submitted_palette_preference_ && snapshot.completed >= submitted_palette_preference_) {
    palette_preference_error_ = snapshot.error;
    if (snapshot.error.empty()) {
      saved_palette_revision_ = submitted_palette_revision_;
      next_palette_attempt_ = {};
    }
    submitted_palette_preference_ = 0;
  }
}
void LayoutControl::remember_palette(std::string_view layout, std::string_view palette) {
  const auto found = std::find_if(palette_history_.begin(), palette_history_.end(),
                                  [&](const auto& entry) { return entry.first == layout; });
  if (found != palette_history_.end() && found->second == palette &&
      found + 1 == palette_history_.end()) {
    // A failed history write remains tied to this confirmed choice.
    palette_candidate_layout_ = layout;
    palette_candidate_ = palette;
    return;
  }
  if (found != palette_history_.end()) palette_history_.erase(found);
  if (palette_history_.size() == 32) palette_history_.erase(palette_history_.begin());
  palette_history_.emplace_back(layout, palette);
  ++palette_revision_;
  palette_candidate_layout_ = layout;
  palette_candidate_ = palette;
}
void LayoutControl::persist_palette_history(Clock::time_point now) {
  if (!palette_preferences_ || submitted_palette_preference_ ||
      saved_palette_revision_ == palette_revision_ || now < next_palette_attempt_)
    return;
  std::string record = "HUP1\n";
  for (const auto& [layout, palette] : palette_history_)
    record += layout + ' ' + palette + '\n';
  submitted_palette_revision_ = palette_revision_;
  submitted_palette_preference_ = palette_preferences_->save(std::move(record));
  next_palette_attempt_ = now + std::chrono::seconds(1);
}
void LayoutControl::read_request() {
  File input{::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (input.fd < 0) {
    if (errno == ENOENT) {
      desired_.clear();
      desired_stamp_.reset();
      return;
    }
    desired_.clear();
    desired_stamp_.reset();
    error_ = "Cannot read cluster selection";
    return;
  }
  struct stat info{};
  if (fstat(input.fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
      (info.st_mode & 0077) || info.st_nlink != 1 || info.st_size <= 0 || info.st_size > 384) {
    desired_.clear();
    desired_stamp_.reset();
    error_ = "Invalid private cluster selection file";
    return;
  }
  Stamp stamp{std::uint64_t(info.st_dev), std::uint64_t(info.st_ino)};
#ifdef __APPLE__
  stamp.seconds = info.st_mtimespec.tv_sec;
  stamp.nanoseconds = info.st_mtimespec.tv_nsec;
#else
  stamp.seconds = info.st_mtim.tv_sec;
  stamp.nanoseconds = info.st_mtim.tv_nsec;
#endif
  if (desired_stamp_ == stamp) return;
  char bytes[385]{};
  const auto count = ::read(input.fd, bytes, sizeof(bytes));
  const std::string_view request(bytes, count > 0 ? std::size_t(count) : 0);
  const bool layout_only = request.starts_with("HUL1 ");
  const bool appearance = request.starts_with("HUL2 ");
  const auto header_end = request.find('\n');
  std::string_view request_nonce;
  std::uint64_t request_generation = 0;
  bool valid = count == info.st_size && (layout_only || appearance) && header_end != request.npos &&
               header_end >= 5;
  if (valid) {
    const auto header = request.substr(5, header_end - 5);
    valid = header.size() >= 34 && header[32] == ' ';
    if (valid) {
      request_nonce = header.substr(0, 32);
      const auto generation = header.substr(33);
      const auto parsed = std::from_chars(generation.data(), generation.data() + generation.size(),
                                          request_generation);
      valid = request_nonce.find_first_not_of("0123456789abcdef") == request_nonce.npos &&
              (generation.size() == 1 || generation.front() != '0') && parsed.ec == std::errc{} &&
              parsed.ptr == generation.data() + generation.size();
    }
  }
  ClusterSelectionRequest options;
  if (valid && layout_only) {
    const auto prefix_size = header_end + 1;
    valid = request.size() == prefix_size + 37 && request.back() == '\n';
    if (valid) options.layout = request.substr(prefix_size, 36);
  } else if (valid && appearance) {
    std::istringstream fields(std::string(request.substr(header_end + 1)));
    std::string extra;
    valid = valid && bool(std::getline(fields, options.layout)) &&
            bool(std::getline(fields, options.palette)) &&
            bool(std::getline(fields, options.zone)) &&
            bool(std::getline(fields, options.component)) && !std::getline(fields, extra) &&
            request.back() == '\n';
    valid = valid && options.palette.size() <= 64 &&
            options.palette.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") ==
                std::string::npos &&
            options.zone.empty() == options.component.empty() &&
            (options.zone.empty() || (uuid(options.zone) && uuid(options.component)));
  } else {
    valid = false;
  }
  if (!valid || !uuid(options.layout)) {
    desired_.clear();
    desired_options_ = {};
    desired_stamp_.reset();
    error_ = "Invalid cluster selection";
    return;
  }
  if (request_nonce != publisher_.nonce || request_generation != publisher_.connection_generation) {
    // A valid button request belongs only to its original launch/connection.
    // Retained bytes are not an error in the new session and confer no authority.
    desired_.clear();
    desired_options_ = {};
    desired_stamp_ = stamp;
    return;
  }
  desired_ = options.layout;
  desired_options_ = std::move(options);
  desired_stamp_ = stamp;
  error_.clear();
}
void LayoutControl::poll(const Selection& select, Clock::time_point now) {
  poll_cluster(
      [&](const std::optional<ClusterSelectionRequest>& request) {
        return select(request ? std::optional<std::string_view>(request->layout) : std::nullopt);
      },
      now);
}
void LayoutControl::poll_cluster(const ClusterSelection& select, Clock::time_point now) {
  if (now < next_poll_) return;
  next_poll_ = now + std::chrono::milliseconds(250);
  read_request();
  refresh_preferences();
  auto state = select({});
  const auto declared_layout = [](const LayoutSelection& selection, std::string_view identifier) {
    return uuid(identifier) &&
           std::any_of(selection.choices.begin(), selection.choices.end(),
                       [&](const auto& choice) { return choice.identifier == identifier; });
  };
  const auto confirmed_layout = [&](const LayoutSelection& selection, std::string_view identifier) {
    return selection.ready && selection.idle && !selection.transitioning &&
           selection.active == identifier && declared_layout(selection, identifier);
  };
  // The live highlight must belong to the current catalogue. A historical
  // persisted confirmation is separate and remains available for restoration.
  if (!declared_layout(state, confirmed_active_)) confirmed_active_.clear();
  if (confirmed_layout(state, state.active)) confirmed_active_ = state.active;
  const auto confirmed_palette = [](const LayoutSelection& selection,
                                    const ClusterSelectionRequest& request) {
    return request.palette.empty() || request.palette == selection.active_palette;
  };
  const auto confirmed_content = [](const LayoutSelection& selection,
                                    const ClusterSelectionRequest& request) {
    return request.component.empty() ||
           std::any_of(selection.contents.begin(), selection.contents.end(),
                       [&](const auto& choice) {
                         return choice.active && choice.available && choice.zone == request.zone &&
                                choice.identifier == request.component;
                       });
  };
  const auto qualified_palette = [](const LayoutSelection& selection) {
    return selection.active_palette.empty() ||
           std::any_of(
               selection.palettes.begin(), selection.palettes.end(),
               [&](const auto& choice) { return choice.identifier == selection.active_palette; });
  };
  const auto confirmed_options = [&](const LayoutSelection& selection,
                                     const ClusterSelectionRequest& request) {
    return confirmed_palette(selection, request) && confirmed_content(selection, request);
  };
  if (!pending_.empty()) {
    // A native request may partially change the scene before failing. Observe
    // its settled outcome as a new baseline, never as a phone Customize edit.
    observed_layout_.clear();
    observed_palette_.clear();
    if (confirmed_layout(state, pending_) && confirmed_options(state, pending_options_)) {
      if (qualified_palette(state)) remember_confirmed(state.active, state.active_palette);
      pending_.clear();
      error_.clear();
    } else if (state.ready && declared_layout(state, state.active) &&
               !declared_layout(state, pending_)) {
      // Retire an obsolete choice even while transitioning, so native readers
      // can use this catalogue. New requests still require ready/idle below.
      pending_.clear();
      error_ = "Layout is not declared by this pack";
    } else if (now - pending_since_ >= std::chrono::seconds(8)) {
      // A phone can finish the requested layout while retaining its own colour
      // or content choice. Report that partial result without turning it into
      // a successful appearance confirmation or retrying against the phone.
      if (confirmed_layout(state, pending_)) {
        const bool palette_missing = !confirmed_palette(state, pending_options_);
        const bool content_missing = !confirmed_content(state, pending_options_);
        if (palette_missing && content_missing) {
          error_ = "Cluster colour and centre content were not confirmed by the phone";
        } else if (palette_missing) {
          error_ = "Cluster colour was not confirmed. Change colours using Customize in CarPlay";
        } else if (content_missing) {
          error_ = "Cluster centre content was not confirmed by the phone";
        }
      } else {
        error_ = "Cluster style was not confirmed by the phone";
      }
      pending_.clear();
    }
  }
  const bool new_request = !desired_.empty() && desired_stamp_ != attempted_stamp_;
  if (pending_.empty() && state.ready && state.idle && !state.transitioning &&
      (new_request || !restoring_.empty()) &&
      (!preferences_ || (preferences_loaded_ && palette_preferences_loaded_))) {
    const auto requested = new_request ? desired_ : restoring_;
    // A saved palette is offered only while this pack still declares it.
    const bool palette_declared =
        std::any_of(state.palettes.begin(), state.palettes.end(),
                    [&](const auto& choice) { return choice.identifier == restoring_palette_; });
    auto options = new_request ? desired_options_
                               : ClusterSelectionRequest{
                                     requested, palette_declared ? restoring_palette_ : "", {}, {}};
    if (new_request && options.palette.empty()) {
      const auto remembered =
          std::find_if(palette_history_.begin(), palette_history_.end(),
                       [&](const auto& entry) { return entry.first == requested; });
      if (remembered != palette_history_.end() &&
          std::any_of(state.palettes.begin(), state.palettes.end(),
                      [&](const auto& choice) { return choice.identifier == remembered->second; }))
        options.palette = remembered->second;
    }
    state = select(options);
    if (state.result == LayoutRequest::requested) {
      pending_ = requested;
      pending_options_ = options;
      pending_since_ = now;
      error_.clear();
    } else if (state.result == LayoutRequest::selected && confirmed_layout(state, requested) &&
               confirmed_options(state, options)) {
      if (qualified_palette(state)) remember_confirmed(state.active, state.active_palette);
      error_.clear();
    } else if (state.result == LayoutRequest::unknown) {
      error_ = declared_layout(state, requested) &&
                       (!options.palette.empty() || !options.component.empty())
                   ? "Cluster choice is not supported by this pack"
                   : "Layout is not declared by this pack";
    }
    if (state.result == LayoutRequest::requested || state.result == LayoutRequest::selected ||
        state.result == LayoutRequest::unknown) {
      observed_layout_.clear();
      observed_palette_.clear();
      if (new_request) attempted_stamp_ = desired_stamp_;
      if (!new_request) restore_settled_ = true;
      restoring_.clear();
      restoring_palette_.clear();
    }
  }
  if ((!preferences_ || preferences_loaded_) && restoring_.empty() && pending_.empty() &&
      !restore_settled_)
    restore_settled_ = true;
  // After any restore, also keep choices made on the phone (CarPlay Customize):
  // a settled, idle, declared layout and its palette become the preference.
  if (restore_settled_ && pending_.empty() && confirmed_layout(state, state.active) &&
      qualified_palette(state)) {
    // First observation is a baseline, not a new preference. In particular an
    // unsupported restore must not overwrite the last confirmed saved choice.
    // A later qualified phone edit still recovers that preference even while
    // the rejected native request remains diagnosed in the selector status.
    if (!observed_layout_.empty() &&
        (observed_layout_ != state.active || observed_palette_ != state.active_palette))
      if (qualified_palette(state)) remember_confirmed(state.active, state.active_palette);
    observed_layout_ = state.active;
    observed_palette_ = state.active_palette;
  }
  // Only retry a previously phone-confirmed choice while that choice remains
  // qualified in this live session. A request, reset or catalogue change cannot
  // turn a failed preference write into a new confirmation.
  if (!confirmed_candidate_.empty() && confirmed_layout(state, confirmed_candidate_) &&
      qualified_palette(state) && state.active_palette == candidate_palette_)
    persist_confirmed(now);
  if (!palette_candidate_layout_.empty() && confirmed_layout(state, palette_candidate_layout_) &&
      qualified_palette(state) && state.active_palette == palette_candidate_)
    persist_palette_history(now);
  publish(state, now);
}
void LayoutControl::remember_confirmed(std::string_view layout, std::string_view palette) {
  if (preference_path_.empty() || !uuid(layout)) return;
  if (!palette.empty() && !palette_name(palette)) palette = {};
  remember_palette(layout, palette);
  if (!submitted_preference_ && !confirmed_.empty() && layout == confirmed_ &&
      palette == confirmed_palette_ && preference_error_.empty()) {
    confirmed_candidate_.clear();
    candidate_palette_.clear();
    return;
  }
  confirmed_candidate_ = layout;
  candidate_palette_ = palette;
}
void LayoutControl::persist_confirmed(Clock::time_point now) {
  if (!preferences_ || submitted_preference_ || now < next_preference_attempt_) return;
  const auto record = confirmed_candidate_ + '\n' +
                      (candidate_palette_.empty() ? std::string() : candidate_palette_ + '\n');
  submitted_layout_ = confirmed_candidate_;
  submitted_palette_ = candidate_palette_;
  submitted_preference_ = preferences_->save(record);
  next_preference_attempt_ = now + std::chrono::seconds(1);
}
void LayoutControl::publish(const LayoutSelection& state, Clock::time_point now) {
  std::ostringstream json;
  json << "{\"active\":";
  quoted(json, state.active);
  json << ",\"pending\":";
  quoted(json, pending_);
  json << ",\"restoring\":";
  quoted(json, restoring_);
  json << ",\"confirmed\":";
  quoted(json, confirmed_);
  json << ",\"confirmedActive\":";
  quoted(json, confirmed_active_);
  json << ",\"ready\":" << (state.ready ? "true" : "false")
       << ",\"idle\":" << (state.idle ? "true" : "false")
       << ",\"transitioning\":" << (state.transitioning ? "true" : "false") << ",\"error\":";
  quoted(json, error_);
  json << ",\"choices\":[";
  bool first = true;
  for (const auto& choice : state.choices) {
    if (!first) json << ',';
    first = false;
    json << "{\"id\":";
    quoted(json, choice.identifier);
    json << ",\"name\":";
    quoted(json, choice.name);
    json << '}';
  }
  json << "],\"phoneVideo\":";
  if (state.phone_video) {
    const auto& video = *state.phone_video;
    json << "{\"visible\":" << (video.visible ? "true" : "false")
         << ",\"opacity\":" << video.opacity
         << ",\"hasCurrentPTS\":" << (video.has_current_pts ? "true" : "false") << '}';
  } else {
    json << "null";
  }
  json << ",\"activePalette\":";
  quoted(json, state.active_palette);
  json << ",\"palettes\":[";
  first = true;
  for (const auto& choice : state.palettes) {
    if (!first) json << ',';
    first = false;
    json << "{\"id\":";
    quoted(json, choice.identifier);
    json << ",\"name\":";
    quoted(json, choice.name);
    json << '}';
  }
  json << "],\"contents\":[";
  first = true;
  for (const auto& choice : state.contents) {
    if (!first) json << ',';
    first = false;
    json << "{\"id\":";
    quoted(json, choice.identifier);
    json << ",\"name\":";
    quoted(json, choice.name);
    json << ",\"zone\":";
    quoted(json, choice.zone);
    json << ",\"active\":" << (choice.active ? "true" : "false")
         << ",\"available\":" << (choice.available ? "true" : "false") << '}';
  }
  json << "],\"preferenceError\":";
  quoted(json, preference_error_.empty() ? palette_preference_error_ : preference_error_);
  json << '}';
  const auto text = json.str();
  if (text == last_status_ && now < next_status_) return;
  // Small atomic status writes, no fsync in the render loop. This is transient
  // UI state, not a durable preference or a source of vehicle data.
  const auto publication = text.substr(0, text.size() - 1) + ",\"version\":2,\"nonce\":\"" +
                           publisher_.nonce + "\",\"connectionGeneration\":\"" +
                           std::to_string(publisher_.connection_generation) + "\",\"sequence\":\"" +
                           std::to_string(sequence_ + 1) + "\",\"updatedMonotonicNs\":\"" +
                           std::to_string(presentation_monotonic_ns()) + "\"}\n";
  if (atomic_write(path_ + ".status", publication)) {
    ++sequence_;
    last_status_ = text;
    next_status_ = now + std::chrono::seconds(1);
  }
}
}  // namespace headunit::theme::script
