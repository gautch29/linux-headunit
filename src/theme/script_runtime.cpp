// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/script_runtime.hpp>
#include <headunit/binary_plist.hpp>
#include <time.h>
#include <headunit/theme/ui_channel.hpp>
#include "script_graphics.hpp"
#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>
#include <plist/plist.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace headunit::theme::script {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_source = 4 * 1024 * 1024, max_source_total = 16 * 1024 * 1024;
constexpr std::size_t max_messages = 64, max_queued_bytes = 2 * max_packet_bytes;
constexpr std::size_t max_preference_bytes = 256 * 1024;
constexpr const char* blob_type = "Headunit.Theme.PlistData";
constexpr const char* array_type = "Headunit.Theme.PlistArray";
constexpr const char* task_type = "Headunit.Theme.LuaTask";
constexpr const char* invalid_type = "Headunit.Theme.InvalidVehicleValue";
constexpr std::array session_diagnostic_names{"session_start",
                                              "session_reset",
                                              "reset_received",
                                              "reset_complete_received",
                                              "session_ready",
                                              "reset_watchdog",
                                              "foreground_reset",
                                              "identity_rejected",
                                              "reset_state_rejected",
                                              "session_number_rejected",
                                              "sequence_rejected",
                                              "sequence_gap",
                                              "acknowledgement_rejected",
                                              "malformed_message",
                                              "transport_error",
                                              "layout_requested",
                                              "layout_not_ready",
                                              "layout_transition_started",
                                              "layout_transition_completed",
                                              "transition_watchdog",
                                              "transition_metadata_rejected",
                                              "remote_ui_shown",
                                              "remote_ui_hidden",
                                              "video_pts_unavailable",
                                              "video_late"};
static_assert(session_diagnostic_names.size() ==
              std::size_t(SessionDiagnosticKind::video_late) + 1);
static_assert(sizeof(SessionDiagnostic) <= 32);  // No variable-size asset data in an event.
std::optional<SessionDiagnosticKind> session_diagnostic(std::string_view tag,
                                                        std::string_view message) {
  // APL.Log prefixes its module tag with the display. Inspect only the suffix;
  // neither that prefix nor any dynamic suffix of the message can escape here.
  if (const auto prefix = tag.rfind(" | "); prefix != tag.npos) tag.remove_prefix(prefix + 3);
  const auto begins = [&](std::string_view text) {
    return message.starts_with(text);
  };
  using Kind = SessionDiagnosticKind;
  if (tag == "ClusterControlSession") {
    if (message == "Starting ...") return Kind::session_start;
    if (message == "Resetting...") return Kind::session_reset;
    if (begins("Received reset from peer ")) return Kind::reset_received;
    if (begins("Received reset complete from peer ")) return Kind::reset_complete_received;
    if (message == "Ready to send/receive data.") return Kind::session_ready;
    if (message == "Reset Complete Watchdog timed out!") return Kind::reset_watchdog;
    if (message == "Still waiting for reset complete, forcing reset ...")
      return Kind::foreground_reset;
    if (begins("Received unexpected reset message id ") ||
        begins("Received unexpected reset complete message id "))
      return Kind::identity_rejected;
    if (begins("Received unexpected reset complete received while in state "))
      return Kind::reset_state_rejected;
    if (begins("Invalid ssn.")) return Kind::session_number_rejected;
    if (begins("Invalid old psn.")) return Kind::sequence_rejected;
    if (begins("Detected missing message psn.")) return Kind::sequence_gap;
    if (begins("Invalid ack sn.")) return Kind::acknowledgement_rejected;
    if (message == "unable to parse incoming data") return Kind::malformed_message;
    if (begins("Failed to send message: ") || begins("Failed to receive message: "))
      return Kind::transport_error;
  } else if (tag == "LayoutController") {
    if (begins("Queuing requestLayout command: ")) return Kind::layout_requested;
    if (begins("Not ready to send requestLayout command, ")) return Kind::layout_not_ready;
    if (begins("Showing remote UI (animated = ")) return Kind::remote_ui_shown;
    if (begins("Hiding remote UI (animated = ")) return Kind::remote_ui_hidden;
    if (message == "Couldn't get current video stream PTS to check freshness")
      return Kind::video_pts_unavailable;
  } else if (tag == "Workspace") {
    if (begins("Layout change started, preparing layout ")) return Kind::layout_transition_started;
    if (begins("Layout change ended, showing remote UI for layout "))
      return Kind::layout_transition_completed;
    if (message == "Transition watchdog timer fired") return Kind::transition_watchdog;
    if (message == "Video stream freshness is late.") return Kind::video_late;
  } else if (tag == "Metadata") {
    if (begins("Invalid transition metadata: ") ||
        begins("Discarding transition command outside of transition markers: ") ||
        message == "No next layout, ignoring layout transition metadata")
      return Kind::transition_metadata_rejected;
  }
  return {};
}
struct File {
  int fd = -1;
  explicit File(int value) : fd(value) {}
  ~File() {
    if (fd >= 0) ::close(fd);
  }
  File(const File&) = delete;
};
[[noreturn]] void fail(const char* text) {
  throw std::runtime_error(text);
}
bool valid_text(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size()) {
    const auto first = static_cast<unsigned char>(s[i++]);
    if (!first) return false;
    if (first < 0x80) continue;
    const unsigned n = first >= 0xc2 && first <= 0xdf   ? 1
                       : first >= 0xe0 && first <= 0xef ? 2
                       : first >= 0xf0 && first <= 0xf4 ? 3
                                                        : 0;
    if (!n || n > s.size() - i) return false;
    std::uint32_t cp = first & (0x7f >> (n + 1));
    for (unsigned j = 0; j < n; ++j) {
      const auto c = static_cast<unsigned char>(s[i++]);
      if ((c & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (c & 0x3f);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return false;
  }
  return true;
}
std::string_view string_arg(lua_State* L, int index, std::size_t max = max_source) {
  luaL_checktype(L, index, LUA_TSTRING);
  std::size_t length = 0;
  const char* data = lua_tolstring(L, index, &length);
  if (length > max) fail("script string exceeds host limit");
  return {data, length};
}
void* test_userdata(lua_State* L, int index, const char* type) {
  if (lua_type(L, index) != LUA_TUSERDATA) return nullptr;
  auto* data = lua_touserdata(L, index);
  if (!lua_getmetatable(L, index)) return nullptr;
  luaL_getmetatable(L, type);
  const bool matches = lua_rawequal(L, -1, -2);
  lua_pop(L, 2);
  return matches ? data : nullptr;
}
struct Blob {
  std::size_t size;
};
void push_blob(lua_State* L, std::span<const std::uint8_t> bytes) {
  if (bytes.size() > max_source) fail("plist data exceeds host limit");
  auto* blob = static_cast<Blob*>(lua_newuserdata(L, sizeof(Blob) + bytes.size()));
  blob->size = bytes.size();
  std::memcpy(blob + 1, bytes.data(), bytes.size());
  luaL_getmetatable(L, blob_type);
  lua_setmetatable(L, -2);
}
std::span<const std::uint8_t> binary_arg(lua_State* L, int index) {
  if (const auto* b = static_cast<Blob*>(test_userdata(L, index, blob_type)))
    return {reinterpret_cast<const std::uint8_t*>(b + 1), b->size};
  const auto s = string_arg(L, index);
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}
struct FreePlist {
  void operator()(void* p) const { plist_free(p); }
};
using Tree = std::unique_ptr<void, FreePlist>;
struct Conversion {
  std::size_t nodes = 0, bytes = 0;
  std::set<const void*> tables;
  void check(lua_State* L, unsigned depth, std::size_t count = 0) {
    if (depth > 32 || ++nodes > 16384 || count > max_source - bytes)
      fail("plist conversion limit exceeded");
    bytes += count;
    luaL_checkstack(L, 8, "plist conversion");
  }
  Tree from_lua(lua_State* L, int index, unsigned depth = 0) {
    check(L, depth);
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
      case LUA_TBOOLEAN:
        return Tree(plist_new_bool(lua_toboolean(L, index)));
      case LUA_TNUMBER: {
        const double n = lua_tonumber(L, index);
        if (!std::isfinite(n)) fail("nonfinite plist number");
        if (std::floor(n) == n && std::abs(n) <= 9007199254740991.0)
          return Tree(plist_new_int(static_cast<std::int64_t>(n)));
        return Tree(plist_new_real(n));
      }
      case LUA_TSTRING: {
        const auto s = string_arg(L, index);
        check(L, depth, s.size());
        if (!valid_text(s))
          fail("plist strings require UTF-8 without NUL; use data for binary values");
        return Tree(plist_new_string(std::string(s).c_str()));
      }
      case LUA_TUSERDATA: {
        const auto* b = static_cast<Blob*>(test_userdata(L, index, blob_type));
        if (!b) fail("unsupported plist userdata");
        check(L, depth, b->size);
        return Tree(plist_new_data(reinterpret_cast<const char*>(b + 1), b->size));
      }
      case LUA_TTABLE:
        break;
      default:
        fail("unsupported plist value");
    }
    const auto* identity = lua_topointer(L, index);
    if (!tables.insert(identity).second) fail("cyclic plist table");
    struct Pop {
      std::set<const void*>& s;
      const void* p;
      ~Pop() { s.erase(p); }
    } pop{tables, identity};
    std::size_t numbers = 0, strings = 0, highest = 0;
    lua_pushnil(L);
    while (lua_next(L, index)) {
      if (lua_type(L, -2) == LUA_TSTRING)
        ++strings;
      else if (lua_type(L, -2) == LUA_TNUMBER) {
        const auto n = lua_tonumber(L, -2);
        if (n < 1 || n > 16384 || std::floor(n) != n) fail("invalid plist array index");
        ++numbers;
        highest = std::max(highest, static_cast<std::size_t>(n));
      } else
        fail("invalid plist table key");
      lua_pop(L, 1);
      if (numbers + strings > 16384) fail("plist table too large");
    }
    bool tagged_array = false;
    if (lua_getmetatable(L, index)) {
      luaL_getmetatable(L, array_type);
      tagged_array = lua_rawequal(L, -1, -2);
      lua_pop(L, 2);
    }
    if ((numbers && strings) || numbers != highest || (tagged_array && strings))
      fail("mixed or sparse plist table");
    if (numbers || tagged_array) {
      Tree result(plist_new_array());
      for (std::size_t i = 1; i <= numbers; ++i) {
        lua_rawgeti(L, index, static_cast<int>(i));
        auto value = from_lua(L, -1, depth + 1);
        lua_pop(L, 1);
        plist_array_append_item(result.get(), value.release());
      }
      return result;
    }
    Tree result(plist_new_dict());
    lua_pushnil(L);
    while (lua_next(L, index)) {
      const auto key = string_arg(L, -2, 1024);
      if (!valid_text(key)) fail("invalid plist dictionary key");
      auto value = from_lua(L, -1, depth + 1);
      check(L, depth, key.size());
      plist_dict_set_item(result.get(), std::string(key).c_str(), value.release());
      lua_pop(L, 1);
    }
    return result;
  }
  void to_lua(lua_State* L, plist_t node, unsigned depth = 0) {
    check(L, depth);
    switch (plist_get_node_type(node)) {
      case PLIST_BOOLEAN: {
        std::uint8_t b = 0;
        plist_get_bool_val(node, &b);
        lua_pushboolean(L, b);
        break;
      }
      case PLIST_INT: {
        std::int64_t n = 0;
        plist_get_int_val(node, &n);
        if (n < -9007199254740991LL || n > 9007199254740991LL) fail("inexact Lua plist integer");
        lua_pushnumber(L, static_cast<double>(n));
        break;
      }
      case PLIST_REAL: {
        double n = 0;
        plist_get_real_val(node, &n);
        if (!std::isfinite(n)) fail("nonfinite plist number");
        lua_pushnumber(L, n);
        break;
      }
      case PLIST_STRING: {
        std::uint64_t size = 0;
        const char* data = plist_get_string_ptr(node, &size);
        if (!data || !valid_text({data, static_cast<std::size_t>(size)}))
          fail("invalid plist string");
        check(L, depth, size);
        lua_pushlstring(L, data, size);
        break;
      }
      case PLIST_DATA: {
        std::uint64_t size = 0;
        const char* data = plist_get_data_ptr(node, &size);
        check(L, depth, size);
        if (size && !data) fail("invalid plist data");
        push_blob(L, {reinterpret_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size)});
        break;
      }
      case PLIST_ARRAY: {
        const auto size = plist_array_get_size(node);
        if (size > 16384) fail("plist array too large");
        lua_createtable(L, size, 0);
        for (std::uint32_t i = 0; i < size; ++i) {
          to_lua(L, plist_array_get_item(node, i), depth + 1);
          lua_rawseti(L, -2, i + 1);
        }
        luaL_getmetatable(L, array_type);
        lua_setmetatable(L, -2);
        break;
      }
      case PLIST_DICT: {
        if (plist_dict_get_size(node) > 16384) fail("plist dictionary too large");
        lua_newtable(L);
        plist_dict_iter raw = nullptr;
        plist_dict_new_iter(node, &raw);
        std::unique_ptr<void, decltype(&std::free)> iterator(raw, std::free);
        for (;;) {
          char* key = nullptr;
          plist_t value = nullptr;
          plist_dict_next_item(node, raw, &key, &value);
          std::unique_ptr<char, decltype(&std::free)> name(key, std::free);
          if (!value) break;
          if (!key) fail("invalid plist dictionary key");
          check(L, depth, std::strlen(key));
          to_lua(L, value, depth + 1);
          lua_setfield(L, -2, key);
        }
        break;
      }
      default:
        fail("unsupported plist node type");
    }
  }
};
}  // namespace

std::string_view session_diagnostic_name(SessionDiagnosticKind kind) {
  const auto index = std::size_t(kind);
  return index < session_diagnostic_names.size() ? session_diagnostic_names[index] : "unknown";
}

struct Runtime::Impl {
  struct Task {
    Impl* owner;
    lua_State* thread = nullptr;
    std::uint64_t id = 0, generation = 0;
    int thread_ref = LUA_NOREF, self_ref = LUA_NOREF;
    bool busy = false, running = false, cancelled = false;
    std::array<char, 128> name{};
  };
  struct Scheduled {
    int reference;
    std::uint64_t id, generation;
  };
  Display display;
  Limits limits;
  File root;
  ResourceReader resource_reader;
  std::unique_ptr<Graphics> graphics;
  std::unique_ptr<vehicle::ScriptView> vehicle =
      std::make_unique<vehicle::ScriptView>(std::make_shared<vehicle::ReadOnlyService>());
  std::function<std::uint64_t()> vehicle_clock = [] {
    return std::uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
            .count());
  };
  int invalid_ref = LUA_NOREF;
  bool evaluated = false, remote_map_selection = false;
  std::optional<std::string> startup_layout_id;
  bool live_layout_selection = false, observe_scene = false;
  int layout_controller_ref = LUA_NOREF, layout_adapter_ref = LUA_NOREF;
  int scene_adapter_ref = LUA_NOREF;
  std::function<void(const VerificationReport&)> verification_reporter;
  lua_State* state = nullptr;
  lua_State* script = nullptr;
  std::size_t memory = 0, live_tasks = 0, source_bytes = 0, logs = 0, dynamic_bytes = 0;
  std::size_t incoming_bytes = 0, outgoing_bytes = 0;
  std::uint64_t next_task = 1;
  std::uint64_t message_generation = 0;
  int log_level = 4;
  bool tasks_started = false;
  std::function<void(int, std::string_view, std::string_view)> log_sink;
  std::function<void(const SessionDiagnostic&)> session_diagnostic_sink;
  std::array<std::uint64_t, session_diagnostic_names.size()> session_diagnostic_counts{};
  std::array<std::uint8_t, session_diagnostic_names.size()> session_diagnostic_emitted{};
  std::size_t session_diagnostic_limit = 0, session_diagnostic_total = 0;
  std::function<std::optional<Bytes>(std::string_view)> storage_read;
  std::function<void(std::string_view, std::span<const std::uint8_t>)> storage_write;
  std::unordered_map<std::int64_t, std::pair<Clock::time_point, std::string>> signposts;
  std::int64_t next_signpost = 1;
  bool connected = false, faulted = false, foregrounded = false, foreground_ready_requested = false;
  std::optional<double> pts;
  Clock::time_point deadline = Clock::time_point::max();
  Clock::time_point turn_started;
  std::uint64_t turn_cpu_ns = 0, turn_budget_ms = 0;
  std::size_t turn_resumes = 0;
  struct BudgetFrame {
    std::array<char, 129> source{};
    int line = -1;
  };
  std::array<BudgetFrame, 4> budget_frames{};
  std::size_t budget_frame_count = 0;
  std::unordered_map<std::string, int> modules;
  std::set<std::string> loading;
  std::vector<Scheduled> scheduled;
  std::deque<Bytes> incoming, outgoing;

  static Impl& self(lua_State* L) { return *static_cast<Impl*>(lua_callbacks(L)->userdata); }
  static void* allocate(void* context, void* pointer, std::size_t old, std::size_t size) {
    auto& h = *static_cast<Impl*>(context);
    if (!pointer) old = 0;
    if (!size) {
      std::free(pointer);
      h.memory -= old;
      return nullptr;
    }
    if (size > h.limits.memory_bytes - (h.memory - old)) {
      h.faulted = true;
      return nullptr;
    }
    void* result = std::realloc(pointer, size);
    if (result)
      h.memory = h.memory - old + size;
    else
      h.faulted = true;
    return result;
  }
  static void interrupt(lua_State* L, int gc) {
    if (gc >= 0) return;  // Allocation/GC callbacks cannot raise a VM error.
    auto& h = self(L);
    if (h.faulted || h.budget_expired(true)) {
      if (!h.faulted) h.capture_budget_frames(L);
      h.faulted = true;
      luaL_errorL(L, "asset execution budget exceeded");
    }
  }
  void capture_budget_frames(lua_State* L) {
    // Capture the first over-budget location before a signed protected-call
    // wrapper replaces the error's line. Inspect only fixed stack records;
    // requesting source/line does not push values or allocate in Luau.
    for (int depth = 0; depth < 12 && budget_frame_count < budget_frames.size(); ++depth) {
      lua_Debug frame{};
      if (!lua_getinfo(L, depth, "sl", &frame)) break;
      if (!frame.source || frame.source[0] != '@') continue;
      const auto* source = frame.source + 1;
      const auto length = strnlen(source, 129);
      if (!length || length > 128 || source[0] == '/') continue;
      bool valid = true;
      for (std::size_t i = 0; i < length; ++i) {
        const auto c = source[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '/' || c == '.' || c == '-' || c == '_')) {
          valid = false;
          break;
        }
      }
      if (!valid) continue;
      auto& destination = budget_frames[budget_frame_count++];
      std::memcpy(destination.source.data(), source, length);
      destination.source[length] = '\0';
      destination.line = frame.currentline;
    }
  }
  template <class F>
  static int boundary(lua_State* L, F f) {
    try {
      return f(self(L));
    } catch (const std::runtime_error& error) {
      luaL_errorL(L, "%s", error.what());
    } catch (const std::bad_alloc&) {
      self(L).faulted = true;
      luaL_errorL(L, "asset allocation failed");
    }
  }
  static void function(lua_State* L, const char* name, lua_CFunction f) {
    lua_pushcfunction(L, f, name);
    lua_setfield(L, -2, name);
  }
  static int blob_length(lua_State* L) {
    const auto* b = static_cast<Blob*>(luaL_checkudata(L, 1, blob_type));
    lua_pushnumber(L, b->size);
    return 1;
  }
  static int blob_description(lua_State* L) {
    lua_pushliteral(L, "<plist data>");
    return 1;
  }
  static int parse(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto bytes = binary_arg(L, 1);
      if (bytes.empty() || bytes.size() > max_source) fail("invalid plist input size");
      // Both network envelopes and their nested DATA are parsed through this
      // entry point. Bound binary graph expansion before libplist builds a tree.
      if (bytes.size() >= 6 && std::memcmp(bytes.data(), "bplist", 6) == 0) {
        try {
          preflight_binary_plist(bytes, {max_source, 16384, max_source, 32});
        } catch (const std::invalid_argument&) {
          fail("invalid bounded binary property list");
        }
      }
      plist_t node = nullptr;
      plist_format_t format = PLIST_FORMAT_NONE;
      const auto error = plist_from_memory(reinterpret_cast<const char*>(bytes.data()),
                                           bytes.size(), &node, &format);
      Tree tree(node);
      if (error != PLIST_ERR_SUCCESS || !tree ||
          (format != PLIST_FORMAT_BINARY && format != PLIST_FORMAT_XML))
        fail("invalid property list");
      Conversion convert;
      convert.to_lua(L, tree.get());
      return 1;
    });
  }
  static std::string_view storage_name(lua_State* L) {
    const auto name = string_arg(L, 1, 128);
    if (name.empty()) fail("empty persistent storage name");
    for (const unsigned char c : name)
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.'))
        fail("invalid persistent storage name");
    return name;
  }
  static int storage_load(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto name = storage_name(L);
      if (!h.storage_read) fail("persistent storage is unavailable");
      const auto bytes = h.storage_read(name);
      if (!bytes) {
        lua_newtable(L);
        return 1;
      }
      if (bytes->empty() || bytes->size() > max_preference_bytes)
        fail("invalid persistent storage size");
      try {
        preflight_binary_plist(*bytes, {max_preference_bytes, 16384, max_preference_bytes, 32});
      } catch (const std::invalid_argument&) {
        fail("invalid bounded persistent property list");
      }
      plist_t raw = nullptr;
      plist_format_t format = PLIST_FORMAT_NONE;
      const auto error = plist_from_memory(reinterpret_cast<const char*>(bytes->data()),
                                           bytes->size(), &raw, &format);
      Tree tree(raw);
      if (error != PLIST_ERR_SUCCESS || !tree || format != PLIST_FORMAT_BINARY ||
          (plist_get_node_type(tree.get()) != PLIST_DICT &&
           plist_get_node_type(tree.get()) != PLIST_ARRAY))
        fail("invalid stored property list");
      Conversion convert;
      convert.to_lua(L, tree.get());
      return 1;
    });
  }
  static int storage_save(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto name = storage_name(L);
      if (!h.storage_write) fail("persistent storage is unavailable");
      luaL_checktype(L, 2, LUA_TTABLE);
      Conversion convert;
      auto tree = convert.from_lua(L, 2);
      char* raw = nullptr;
      std::uint32_t size = 0;
      const auto error = plist_to_bin(tree.get(), &raw, &size);
      std::unique_ptr<char, decltype(&plist_mem_free)> bytes(raw, plist_mem_free);
      if (error != PLIST_ERR_SUCCESS || !raw || !size || size > max_preference_bytes)
        fail("persistent storage value exceeds limit");
      try {
        preflight_binary_plist({reinterpret_cast<const std::uint8_t*>(raw), size},
                               {max_preference_bytes, 16384, max_preference_bytes, 32});
      } catch (const std::invalid_argument&) {
        fail("persistent storage expansion exceeds limit");
      }
      h.storage_write(name, {reinterpret_cast<const std::uint8_t*>(raw), size});
      lua_pushboolean(L, true);
      return 1;
    });
  }
  std::optional<std::pair<std::string, std::string>> declared_remote_map(
      lua_State* L, std::string_view layout_id) {
    // The signed package module has already been loaded by main.lua. Read its
    // declarations, never source text or guessed IDs. A missing/ambiguous
    // remote-only map leaves the outgoing request untouched.
    const auto module = modules.find("r/PackageData");
    if (module == modules.end()) return {};
    const auto top = lua_gettop(L);
    struct Restore {
      lua_State* state;
      int top;
      ~Restore() { lua_settop(state, top); }
    } restore{L, top};
    const auto field_is = [&](int table, const char* field, std::string_view expected) {
      lua_getfield(L, table, field);
      std::size_t size = 0;
      const char* value = lua_tolstring(L, -1, &size);
      const bool matches = value && std::string_view(value, size) == expected;
      lua_pop(L, 1);
      return matches;
    };
    const auto field_id = [&](int table, const char* field) -> std::optional<std::string> {
      lua_getfield(L, table, field);
      std::size_t size = 0;
      const char* value = lua_tolstring(L, -1, &size);
      std::optional<std::string> result;
      if (value && size > 0 && size <= 128 && !std::memchr(value, 0, size))
        result = std::string(value, size);
      lua_pop(L, 1);
      return result;
    };
    lua_getref(L, module->second);
    if (!lua_istable(L, -1)) return {};
    lua_getfield(L, -1, "displays");
    if (!lua_istable(L, -1) || lua_objlen(L, -1) > 8) return {};
    std::optional<std::pair<std::string, std::string>> found;
    for (int d = 1; d <= int(lua_objlen(L, -1)); ++d) {
      lua_rawgeti(L, -1, d);
      if (!lua_istable(L, -1) || !field_is(-1, "id", display.identifier)) {
        lua_pop(L, 1);
        continue;
      }
      lua_getfield(L, -1, "layouts");
      if (lua_istable(L, -1) && lua_objlen(L, -1) <= 64)
        for (int l = 1; l <= int(lua_objlen(L, -1)); ++l) {
          lua_rawgeti(L, -1, l);
          if (!lua_istable(L, -1) || !field_is(-1, "id", layout_id)) {
            lua_pop(L, 1);
            continue;
          }
          lua_getfield(L, -1, "zones");
          if (lua_istable(L, -1) && lua_objlen(L, -1) <= 64)
            for (int z = 1; z <= int(lua_objlen(L, -1)); ++z) {
              lua_rawgeti(L, -1, z);
              if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
              }
              lua_getfield(L, -1, "type");
              bool dca = false;
              if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "dca");
                dca = lua_istable(L, -1);
                lua_pop(L, 1);
              }
              lua_pop(L, 1);
              const auto zone = dca ? field_id(-1, "id") : std::nullopt;
              if (zone) {
                lua_getfield(L, -1, "components");
                if (lua_istable(L, -1) && lua_objlen(L, -1) <= 64)
                  for (int c = 1; c <= int(lua_objlen(L, -1)); ++c) {
                    lua_rawgeti(L, -1, c);
                    if (lua_istable(L, -1) && field_is(-1, "name", "DCA_Map") &&
                        field_is(-1, "type", "primary")) {
                      lua_getfield(L, -1, "slots");
                      const bool remote_only = lua_istable(L, -1) && lua_objlen(L, -1) == 0;
                      lua_pop(L, 1);
                      if (remote_only)
                        if (const auto component = field_id(-1, "id")) {
                          if (found) return {};
                          found = std::pair{*zone, *component};
                        }
                    }
                    lua_pop(L, 1);
                  }
                lua_pop(L, 1);
              }
              lua_pop(L, 1);
            }
          lua_pop(L, 1);
          lua_pop(L, 1);
        }
      lua_pop(L, 1);
      lua_pop(L, 1);
    }
    return found;
  }
  static int serialize(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      // A remote-only DCA Map has no local gauges, so the asset's Lua request
      // can omit its selection. The host may select only one unambiguous
      // component declared by this verified, loaded asset.
      if (h.remote_map_selection && lua_istable(L, 1)) {
        lua_getfield(L, 1, "type");
        const bool request_layout = lua_isnumber(L, -1) && lua_tointeger(L, -1) == 5;
        lua_pop(L, 1);
        if (request_layout) {
          lua_getfield(L, 1, "data");
          if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "layoutID");
            std::size_t size = 0;
            const char* id = lua_tolstring(L, -1, &size);
            const auto map =
                id ? h.declared_remote_map(L, std::string_view(id, size)) : std::nullopt;
            lua_pop(L, 1);
            if (map) {
              lua_getfield(L, -1, "activeComponents");
              if (lua_istable(L, -1)) {
                lua_getfield(L, -1, map->first.c_str());
                const bool unset = lua_isnil(L, -1);
                lua_pop(L, 1);
                if (unset) {
                  lua_pushlstring(L, map->second.data(), map->second.size());
                  lua_setfield(L, -2, map->first.c_str());
                }
              }
              lua_pop(L, 1);
            }
          }
          lua_pop(L, 1);
        }
      }
      Conversion convert;
      auto tree = convert.from_lua(L, 1);
      char* bytes = nullptr;
      std::uint32_t size = 0;
      const auto error = plist_to_bin(tree.get(), &bytes, &size);
      std::unique_ptr<char, decltype(&plist_mem_free)> memory(bytes, plist_mem_free);
      if (error != PLIST_ERR_SUCCESS || !bytes || size > max_packet_bytes)
        fail("cannot serialize property list");
      push_blob(L, {reinterpret_cast<const std::uint8_t*>(bytes), size});
      return 1;
    });
  }
  std::string read(std::string_view path) {
    if (path.empty() || path.size() > 1024 || path.front() == '/' || path.find('\0') != path.npos ||
        path.find('\\') != path.npos)
      fail("invalid asset-relative path");
    if (resource_reader) {
      auto remaining = path;
      unsigned depth = 0;
      while (!remaining.empty()) {
        const auto separator = remaining.find('/');
        const auto part = remaining.substr(0, separator);
        if (part.empty() || part == "." || part == ".." || part.size() > 255 || ++depth > 32)
          fail("invalid asset path component");
        if (separator == remaining.npos) break;
        remaining.remove_prefix(separator + 1);
        if (remaining.empty()) fail("invalid trailing asset path separator");
      }
      auto bytes = resource_reader(path);
      if (bytes.size() > max_source) fail("asset resource exceeds source limit");
      return bytes;
    }
    File current(::dup(root.fd));
    if (current.fd < 0) fail("cannot access asset root");
    unsigned depth = 0;
    while (!path.empty()) {
      const auto separator = path.find('/');
      const auto part = path.substr(0, separator);
      if (part.empty() || part == "." || part == ".." || part.size() > 255 || ++depth > 32)
        fail("invalid asset path component");
      const bool last = separator == path.npos;
      const int fd =
          ::openat(current.fd, std::string(part).c_str(),
                   O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (last ? 0 : O_DIRECTORY));
      if (fd < 0) fail("asset file unavailable");
      ::close(current.fd);
      current.fd = fd;
      if (last) break;
      path.remove_prefix(separator + 1);
      if (path.empty()) fail("invalid trailing asset path separator");
    }
    struct stat stat{};
    if (::fstat(current.fd, &stat) || !S_ISREG(stat.st_mode) || stat.st_size < 0 ||
        stat.st_nlink != 1 || stat.st_size > static_cast<off_t>(max_source))
      fail("invalid asset source file");
    std::string bytes(static_cast<std::size_t>(stat.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto n = ::read(current.fd, bytes.data() + offset, bytes.size() - offset);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) fail("asset source read failed");
      offset += static_cast<std::size_t>(n);
    }
    return bytes;
  }
  static int raw_asset(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto path = string_arg(L, 1, 1024);
      if (path == "Config.json" && h.startup_layout_id) {
        const auto config = "{\"STARTUP\":{\"LAYOUT\":{\"ID\":\"" + *h.startup_layout_id + "\"}}}";
        lua_pushlstring(L, config.data(), config.size());
        return 1;
      }
      const auto bytes = h.read(path);
      lua_pushlstring(L, bytes.data(), bytes.size());
      return 1;
    });
  }
  void compile(lua_State* L, std::string_view source, const std::string& name) {
    if (source.size() > max_source) fail("asset source too large");
    Luau::CompileOptions options;
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    const char* mutable_globals[] = {"CP", nullptr};
    options.mutableGlobals = mutable_globals;
    const auto bytecode = Luau::compile(std::string(source), options);
    if (bytecode.size() > 2 * max_source) fail("compiled asset module too large");
    if (luau_load(L, name.c_str(), bytecode.data(), bytecode.size(), 0) != 0)
      throw std::runtime_error(lua_tostring(L, -1) ? lua_tostring(L, -1)
                                                   : "asset compilation failed");
  }
  void prepare_layout_adapter(lua_State* L) {
    if (layout_adapter_ref != LUA_NOREF) return;
    // Original host adapter to this pack dialect, not a replacement layout
    // engine. Use the asset's own session so sequence numbers, retries and
    // phone-driven transitions keep their normal semantics.
    compile(L, R"(
    return function(controller, requested, connected, palette, zone, component)
      local choices = {}
      local target
      for _, info in ipairs(controller._display:getLayouts('primary')) do
        assert(#choices < 64, 'too many primary layouts')
        local id = info:getID()
        choices[#choices + 1] = {id=id, name=info:getDisplayName()}
        if requested == id then target = controller._layoutsById[id] end
      end
      local current = controller:getActiveLayout()
      local active = current and current:getID() or ''
      local state = controller._workspace:getState()
      local ready = connected and controller._session:getIsReady()
      local busy = state:isTransitioning()
      local idle = type(state.isIdle) == 'function' and state:isIdle()
      local palettes, contents = {}, {}
      local activePalette = current and type(current.getPaletteID)=='function' and current:getPaletteID() or ''
      -- Older diagnostic dialects have no theme/component catalogue.
      local theme = controller._theme
      if theme and theme.palettes then
        for id, info in pairs(theme.palettes) do
          assert(#palettes < 64, 'too many cluster palettes')
          palettes[#palettes + 1] = {id=id, name=info.displayName or id}
        end
        table.sort(palettes, function(a,b) return a.id < b.id end)
      end
      local shown = target or current
      local activeComponents = shown and type(shown.getActiveDCAComponentIDs)=='function' and shown:getActiveDCAComponentIDs() or {}
      local inputs = {}
      local registered = CP.enumerateInputStreams()
      assert(#registered <= 8, 'too many local inputs')
      for _, input in ipairs(registered) do inputs[input.identifier]=true end
      local function inputsAvailable(item)
        local slots = item.slots or {}
        assert(#slots <= 128, 'too many component slots')
        for _, slot in ipairs(slots) do
          local instrument = slot.instrument
          local kind = instrument and instrument.kind
          if kind=='inputStream' or kind=='punchThrough' then
            -- The signed package parser normalizes named input declarations
            -- before constructing the exposed LayoutInfo catalogue.
            if type(instrument.streamIdentifier)~='string' or
               not inputs[instrument.streamIdentifier] then return false end
          elseif type(kind)=='table' then
            local input = kind.inputStream or kind.punchThrough
            if input and (type(input)~='table' or type(input.identifier)~='string' or
                          not inputs[input.identifier]) then return false end
          end
        end
        return true
      end
      for _, info in ipairs(controller._display:getLayouts('primary')) do
        if shown and info:getID()==shown:getID() and type(info.getZones)=='function' then
          for _, area in ipairs(info:getZones()) do
            if area.type and area.type.dca then
              for _, item in ipairs(area.components) do
                if item.type=='primary' then
                  assert(#contents < 128, 'too many cluster content choices')
                  contents[#contents+1]={zone=area.id, id=item.id,
                    name=item.displayName or item.name or item.id, active=activeComponents[area.id]==item.id,
                    available=inputsAvailable(item)}
                end
              end
            end
          end
        end
      end
      table.sort(contents, function(a,b) return a.zone..a.id < b.zone..b.id end)
      local result = 0
      if requested then
        local validPalette = not palette or (theme and theme.palettes[palette])
        local validContent = not component
        for _, option in ipairs(contents) do
          if option.id==component and option.zone==zone and option.available then validContent=true end
        end
        if not target or not validPalette or not validContent then result = 1
        elseif busy or (ready and not idle) then result = 2
        elseif requested == active and ready and idle and
          (not palette or palette==activePalette) and
          (not component or current:getActiveDCAComponentIDs()[zone]==component) then result = 3
        elseif ready then
          if not palette and not component then
            controller:_sendLayoutRequest(target, state.appearance.targetAppearanceMode, true, true)
          else
            local components = target:getActiveDCAComponentIDs()
            if component then components[zone]=component end
            -- Request through the existing signed session. Do not mutate
            -- visible local state before the phone's transition arrives.
            controller._session:sendCommand({type=5, data={layoutID=requested,
              paletteID=palette or target:getPaletteID(),
              appearanceMode=state.appearance.targetAppearanceMode,
              fadeInNewLayout=true, fadeOutOldLayout=true, activeComponents=components}})
          end
          result = 4
        end
      end
      local videoState
      local input=controller._iPhoneVideoInputStream
      if type(input)=='table' and type(input.getRootLayer)=='function' then
        local video=input:getRootLayer()
        videoState={visible=video:getVisible(), opacity=video:getOpacity(),
                    hasCurrentPTS=input:getCurrentPTS()~=nil}
      end
      return choices, active, ready, busy, result, idle, palettes, contents, activePalette, videoState
    end
  )",
            "host-live-layout-adapter");
    check(lua_pcall(L, 0, 1, 0), L);
    layout_adapter_ref = lua_ref(L, -1);
    lua_pop(L, 1);
  }
  static int require(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      std::string name(string_arg(L, 1, 512));
      if (const auto i = h.modules.find(name); i != h.modules.end()) {
        lua_getref(L, i->second);
        return 1;
      }
      if (h.modules.size() >= 512 || h.loading.size() >= 64 || !h.loading.insert(name).second)
        fail("cyclic or excessive module loading");
      struct Remove {
        std::set<std::string>& s;
        const std::string& n;
        ~Remove() { s.erase(n); }
      } remove{h.loading, name};
      // Dotted APL names and slash-separated resource names are both used by
      // signed assets. Only .lua text is loaded; package/native searchers do not exist.
      if (name.empty() || name.find("..") != name.npos || name.front() == '.' || name.back() == '.')
        fail("invalid module name");
      std::string path = name;
      std::replace(path.begin(), path.end(), '.', '/');
      path += ".lua";
      const auto source = h.read(path);
      if (source.size() > max_source_total - h.source_bytes) fail("asset module budget exceeded");
      h.source_bytes += source.size();
      h.compile(L, source, "@" + path);
      if (lua_pcall(L, 0, 1, 0) != LUA_OK)
        throw std::runtime_error(lua_tostring(L, -1) ? lua_tostring(L, -1) : "asset module failed");
      if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
      }
      if ((h.live_layout_selection || h.observe_scene) && name == "LayoutController")
        h.observe_layout_constructor(L);
      if (h.live_layout_selection && name == "Layout") h.observe_remote_component_constructor(L);
      if (name == "APL.StableNumberLayer") h.observe_stable_number_constructor(L);
      const int reference = lua_ref(L, -1);
      h.modules.emplace(name, reference);
      return 1;
    });
  }
  static int layout_constructor(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto arguments = lua_gettop(L);
      lua_pushvalue(L, lua_upvalueindex(1));
      lua_insert(L, 1);
      h.check(lua_pcall(L, arguments, LUA_MULTRET, 0), L);
      const auto results = lua_gettop(L);
      if (results && lua_istable(L, 1)) {
        if (h.layout_controller_ref != LUA_NOREF)
          fail("multiple layout controllers in one display VM");
        h.layout_controller_ref = lua_ref(L, 1);
      }
      return results;
    });
  }
  void observe_layout_constructor(lua_State* L) {
    if (!lua_istable(L, -1)) fail("layout controller module must return a table");
    const auto original = lua_absindex(L, -1);
    lua_getfield(L, original, "new");
    if (!lua_isfunction(L, -1)) fail("layout controller has no constructor");
    lua_newtable(L);
    const auto proxy = lua_absindex(L, -1);
    lua_pushvalue(L, -2);
    lua_pushcclosure(L, layout_constructor, "host-layout-constructor", 1);
    lua_setfield(L, proxy, "new");
    lua_newtable(L);
    lua_pushvalue(L, original);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "protected layout adapter");
    lua_setfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, 1);
    lua_setmetatable(L, proxy);
    lua_setreadonly(L, proxy, 1);
    // Delegate all other module properties to the original immutable table.
    // The signed constructor and its object/prototype remain untouched.
    lua_replace(L, original);
    lua_settop(L, original);
  }
  void observe_remote_component_constructor(lua_State* L) {
    const auto original = lua_absindex(L, -1);
    compile(L, R"(
      return function(original)
        return setmetatable({new=function(info, ...)
          local layout=original.new(info, ...)
          local declared, selected={}, {}
          local zones=info:getZones()
          assert(#zones<=64, 'too many layout zones')
          local count=0
          for _, zone in ipairs(zones) do
            if zone.type and zone.type.dca then
              for _, component in ipairs(zone.components) do
                if component.type=='primary' and component.slots and #component.slots==0 then
                  count=count+1
                  assert(count<=128, 'too many remote components')
                  declared[zone.id]=declared[zone.id] or {}
                  declared[zone.id][component.id]=true
                end
              end
            end
          end
          local set=layout.setActiveDCAComponentIDs
          local get=layout.getActiveDCAComponentIDs
          layout.setActiveDCAComponentIDs=function(self, components)
            local result=set(self, components)
            -- Observe the signed setter after it succeeds. A request does not
            -- populate this state; the normal phone transition calls it.
            for zone, choices in pairs(declared) do
              local id=components[zone]
              selected[zone]=choices[id] and id or nil
            end
            return result
          end
          layout.getActiveDCAComponentIDs=function(self)
            local components=get(self)
            for zone, id in pairs(selected) do
              if components[zone]==nil then components[zone]=id end
            end
            return components
          end
          return layout
        end}, {__index=original, __metatable='protected remote component adapter'})
      end
    )",
            "host-remote-component-adapter");
    check(lua_pcall(L, 0, 1, 0), L);
    lua_pushvalue(L, original);
    check(lua_pcall(L, 1, 1, 0), L);
    lua_setreadonly(L, -1, 1);
    lua_replace(L, original);
    lua_settop(L, original);
  }
  void observe_stable_number_constructor(lua_State* L) {
    // Reserve the stable glyph container for the number actually displayed.
    // For example 19.6 formats as 20; reserving the pre-rounded tens bucket
    // allows the digits to collide with the following unit label.
    const auto original = lua_absindex(L, -1);
    compile(L, R"(
      return function(original)
        return setmetatable({new=function(...)
          local layer=original.new(...)
          local setNumber=layer.setNumber
          layer.setNumber=function(self, value, skip)
            if type(value)=='number' then
              local text=require('APL.LuaUtils').numberToString(value,
                self._maximumFractionDigits, self._minFractionDigits, self._minIntegerDigits)
              value=tonumber(text)
              assert(value~=nil, 'invalid formatted stable number')
            end
            return setNumber(self, value, skip)
          end
          return layer
        end}, {__index=original, __metatable='protected stable number adapter'})
      end
    )",
            "host-stable-number-adapter");
    check(lua_pcall(L, 0, 1, 0), L);
    lua_pushvalue(L, original);
    check(lua_pcall(L, 1, 1, 0), L);
    lua_setreadonly(L, -1, 1);
    lua_replace(L, original);
    lua_settop(L, original);
  }
  static int load_string(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      // The package's animation library builds easing functions from text.
      // Compile only bounded source through our pinned compiler, never bytecode.
      const auto source = string_arg(L, 1, 65536);
      if (source.size() > 1024 * 1024 - h.dynamic_bytes) fail("dynamic source budget exceeded");
      h.dynamic_bytes += source.size();
      const auto label = lua_isnoneornil(L, 2) ? std::string("asset-expression")
                                               : std::string(string_arg(L, 2, 256));
      const auto top = lua_gettop(L);
      try {
        h.compile(L, source, label);
        return 1;
      } catch (const std::runtime_error& e) {
        lua_settop(L, top);
        lua_pushnil(L);
        lua_pushstring(L, e.what());
        return 2;
      }
    });
  }
  static int uptime(lua_State* L) {
    lua_pushnumber(L, std::chrono::duration<double>(Clock::now().time_since_epoch()).count());
    return 1;
  }
  static int get_log(lua_State* L) {
    lua_pushinteger(L, self(L).log_level);
    return 1;
  }
  static int set_log(lua_State* L) {
    const int level = luaL_checkinteger(L, 1);
    if (level < 0 || level > 5) luaL_errorL(L, "invalid log level");
    self(L).log_level = level;
    return 0;
  }
  static int log(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto level = luaL_checkinteger(L, 1);
      const auto tag = string_arg(L, 2, 1024), message = string_arg(L, 3, 16384);
      ++h.logs;
      if (h.session_diagnostic_sink)
        if (const auto kind = session_diagnostic(tag, message)) {
          const auto index = std::size_t(*kind);
          auto& count = h.session_diagnostic_counts[index];
          if (count < std::numeric_limits<std::uint64_t>::max()) ++count;
          if (h.session_diagnostic_total < h.session_diagnostic_limit &&
              h.session_diagnostic_emitted[index] < 16 &&
              (count <= 4 || (count & (count - 1)) == 0)) {
            ++h.session_diagnostic_total;
            ++h.session_diagnostic_emitted[index];
            const auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  Clock::now().time_since_epoch())
                                  .count();
            h.session_diagnostic_sink(
                {*kind, count, std::uint64_t(time), h.connected, h.foregrounded});
          }
        }
      if (h.log_sink) h.log_sink(level, tag, message);
      return 0;
    });
  }
  static int ready_to_foreground(lua_State* L) {
    self(L).foreground_ready_requested = true;
    return 0;
  }
  static int signpost_begin(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto name = string_arg(L, 1, 256);
      if (!valid_text(name) || h.signposts.size() >= 128 || h.next_signpost >= 9007199254740991LL)
        fail("invalid or excessive signpost interval");
      const auto id = h.next_signpost++;
      h.signposts.emplace(id, std::pair{Clock::now(), std::string(name)});
      lua_pushnumber(L, double(id));
      return 1;
    });
  }
  static int signpost_end(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto id = luaL_checkinteger(L, 1);
      const auto name = string_arg(L, 2, 256);
      if (!valid_text(name) || !h.signposts.contains(id)) fail("unknown signpost interval");
      h.signposts.erase(id);
      return 0;
    });
  }
  static int signpost_event(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto name = string_arg(L, 1, 256);
      if (!valid_text(name)) fail("invalid signpost event");
      if (h.log_sink) h.log_sink(2, "signpost", name);
      return 0;
    });
  }
  static int print(lua_State* L) {
    ++self(L).logs;
    return 0;
  }
  static int output_streams(lua_State* L) {
    const auto& d = self(L).display;
    lua_newtable(L);
    lua_newtable(L);
    lua_pushlstring(L, d.identifier.data(), d.identifier.size());
    lua_setfield(L, -2, "identifier");
    lua_pushnumber(L, d.width);
    lua_setfield(L, -2, "width");
    lua_pushnumber(L, d.height);
    lua_setfield(L, -2, "height");
    lua_pushnumber(L, d.frame_duration);
    lua_setfield(L, -2, "frameDuration");
    lua_rawseti(L, -2, 1);
    return 1;
  }
  static int input_streams(lua_State* L) {
    return boundary(L, [&](Impl& h) { return h.graphics->enumerate_inputs(L); });
  }
  static std::int64_t vehicle_id(lua_State* L, int index) {
    luaL_checktype(L, index, LUA_TNUMBER);
    const double value = lua_tonumber(L, index);
    if (!std::isfinite(value) || value < 0 || value > 9007199254740991.0 ||
        std::floor(value) != value)
      fail("invalid vehicle IID");
    return static_cast<std::int64_t>(value);
  }
  static std::vector<std::int64_t> vehicle_ids(lua_State* L, int index, bool dictionary = false) {
    luaL_checktype(L, index, LUA_TTABLE);
    index = lua_absindex(L, index);
    const auto n = lua_objlen(L, index);
    if (!dictionary && n > 512) fail("too many vehicle IDs");
    std::vector<std::int64_t> ids;
    lua_pushnil(L);
    std::size_t count = 0;
    while (lua_next(L, index)) {
      if (++count > 512) fail("too many vehicle IDs");
      const auto key = vehicle_id(L, -2);
      if (dictionary)
        ids.push_back(key);
      else if (key < 1 || key > n)
        fail("vehicle IDs must be a dense array");
      lua_pop(L, 1);
    }
    if (!dictionary) {
      if (count != std::size_t(n)) fail("vehicle IDs must be a dense array");
      for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, index, i);
        ids.push_back(vehicle_id(L, -1));
        lua_pop(L, 1);
      }
    }
    return ids;
  }
  void push_vehicle_value(lua_State* L, const opack::Value& value, unsigned depth,
                          std::size_t& nodes) {
    if (depth > 32 || ++nodes > 16384 || !lua_checkstack(L, 8))
      fail("script vehicle value exceeds limits");
    const auto& data = value.data;
    if (std::holds_alternative<std::monostate>(data))
      lua_getref(L, invalid_ref);
    else if (const auto* p = std::get_if<bool>(&data))
      lua_pushboolean(L, *p);
    else if (const auto* p = std::get_if<std::int64_t>(&data)) {
      if (*p < -9007199254740991LL || *p > 9007199254740991LL) fail("inexact Lua vehicle integer");
      lua_pushnumber(L, static_cast<double>(*p));
    } else if (const auto* p = std::get_if<double>(&data)) {
      if (!std::isfinite(*p)) fail("nonfinite vehicle number");
      lua_pushnumber(L, *p);
    } else if (const auto* p = std::get_if<std::string>(&data)) {
      if (p->size() > max_packet_bytes || !valid_text(*p)) fail("invalid vehicle string");
      lua_pushlstring(L, p->data(), p->size());
    } else if (const auto* p = std::get_if<Bytes>(&data))
      push_blob(L, *p);
    else if (const auto* p = std::get_if<opack::Array>(&data)) {
      if (p->size() > 16384) fail("vehicle array exceeds limit");
      lua_createtable(L, p->size(), 0);
      int i = 0;
      for (const auto& v : *p) {
        push_vehicle_value(L, v, depth + 1, nodes);
        lua_rawseti(L, -2, ++i);
      }
    } else if (const auto* p = std::get_if<opack::Dictionary>(&data)) {
      if (p->size() > 16384) fail("vehicle dictionary exceeds limit");
      lua_createtable(L, 0, p->size());
      for (const auto& [key, v] : *p) {
        if (!std::holds_alternative<std::string>(key.data) &&
            !std::holds_alternative<std::int64_t>(key.data))
          fail("unsupported vehicle dictionary key");
        push_vehicle_value(L, key, depth + 1, nodes);
        push_vehicle_value(L, v, depth + 1, nodes);
        lua_rawset(L, -3);
      }
    } else
      fail("unsupported script vehicle value");
  }
  int push_vehicle_read(lua_State* L, const vehicle::ScriptRead& reading) {
    std::size_t nodes = 0;
    lua_createtable(L, 0, reading.values.size());
    for (const auto& [id, value] : reading.values) {
      lua_pushnumber(L, static_cast<double>(id));
      push_vehicle_value(L, value, 0, nodes);
      lua_rawset(L, -3);
    }
    // The inspected CP consumers expect one IID -> value table. Keep detailed
    // native errors in ScriptRead rather than inventing a second Lua result.
    return 1;
  }
  static int vehicle_config(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto value = h.vehicle->configuration(h.vehicle_clock());
      std::size_t nodes = 0;
      h.push_vehicle_value(L, value, 0, nodes);
      return 1;
    });
  }
  static int vehicle_register(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto ids = vehicle_ids(L, 1);
      h.vehicle->subscribe(ids);
      return 0;
    });
  }
  static int vehicle_read(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto ids = vehicle_ids(L, 1);
      return h.push_vehicle_read(L, h.vehicle->read(ids, h.vehicle_clock()));
    });
  }
  static int vehicle_updates(lua_State* L) {
    return boundary(
        L, [&](Impl& h) { return h.push_vehicle_read(L, h.vehicle->updates(h.vehicle_clock())); });
  }
  static int vehicle_write(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto ids = vehicle_ids(L, 1, true);
      std::map<std::int64_t, opack::Value> values;
      for (const auto id : ids) {
        lua_pushnumber(L, static_cast<double>(id));
        lua_rawget(L, 1);
        // No recursive Lua-to-vehicle conversion: the only accepted operation
        // is the literal boolean false. Other values retain rejection behavior.
        values.emplace(id, lua_type(L, -1) == LUA_TBOOLEAN
                               ? opack::Value{bool(lua_toboolean(L, -1))}
                               : opack::Value{});
        lua_pop(L, 1);
      }
      return h.push_vehicle_read(L, h.vehicle->dismiss(values, h.vehicle_clock()));
    });
  }
  static int invalid_description(lua_State* L) {
    lua_pushliteral(L, "<invalid vehicle value>");
    return 1;
  }
  static void raw_field(lua_State* L, int index, const char* name) {
    index = lua_absindex(L, index);
    lua_pushstring(L, name);
    lua_rawget(L, index);
  }
  static std::int32_t region_integer(lua_State* L, const char* name, bool size) {
    raw_field(L, -1, name);
    luaL_checktype(L, -1, LUA_TNUMBER);
    const auto value = lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (!std::isfinite(value) || value < (size ? 0 : -32768) || value > 32768 ||
        std::floor(value) != value)
      fail("invalid verification region geometry");
    return static_cast<std::int32_t>(value);
  }
  static std::string region_text(lua_State* L, const char* name, std::size_t limit) {
    raw_field(L, -1, name);
    luaL_checktype(L, -1, LUA_TSTRING);
    std::size_t length = 0;
    const auto* value = lua_tolstring(L, -1, &length);
    if (!length || length > limit || !valid_text({value, length}))
      fail("invalid verification region text");
    std::string result(value, length);
    lua_pop(L, 1);
    return result;
  }
  static int update_verified_ui(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      if (!h.verification_reporter) fail("verified UI report receiver is not configured");
      luaL_checktype(L, 1, LUA_TTABLE);
      const auto count = lua_objlen(L, 1);
      if (count > 128) fail("too many verification regions");
      std::size_t entries = 0;
      lua_pushnil(L);
      while (lua_next(L, 1)) {
        if (++entries > 128 || lua_type(L, -2) != LUA_TNUMBER)
          fail("verification regions must be a dense array");
        const auto key = lua_tonumber(L, -2);
        if (!std::isfinite(key) || key < 1 || key > count || std::floor(key) != key)
          fail("verification regions must be a dense array");
        lua_pop(L, 1);
      }
      if (entries != std::size_t(count)) fail("verification regions must be a dense array");
      VerificationReport report{h.display.identifier, h.pts, {}};
      report.regions.reserve(count);
      for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, 1, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        std::size_t fields = 0;
        lua_pushnil(L);
        while (lua_next(L, -2)) {
          if (++fields > 6 || lua_type(L, -2) != LUA_TSTRING)
            fail("invalid verification region fields");
          const auto key = string_arg(L, -2, 32);
          if (key != "originX" && key != "originY" && key != "width" && key != "height" &&
              key != "identifier" && key != "presentationStyle")
            fail("unknown verification region field");
          lua_pop(L, 1);
        }
        if (fields != 6) fail("missing verification region field");
        VerificationRegion region;
        region.x = region_integer(L, "originX", false);
        region.y = region_integer(L, "originY", false);
        region.width = region_integer(L, "width", true);
        region.height = region_integer(L, "height", true);
        region.identifier = region_text(L, "identifier", 256);
        region.presentation_style = region_text(L, "presentationStyle", 64);
        report.regions.push_back(std::move(region));
        lua_pop(L, 1);
      }
      h.verification_reporter(report);
      return 0;
    });
  }
  static int property(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto name = string_arg(L, 2, 128);
      if (name == "PTS" && h.pts)
        lua_pushnumber(L, *h.pts);
      else if (name == "isForegrounded")
        lua_pushboolean(L, h.foregrounded && !h.faulted);
      else
        lua_pushnil(L);
      return 1;
    });
  }
  static int status(lua_State* L) {
    const auto& h = self(L);
    lua_pushboolean(L, h.connected && !h.incoming.empty());
    lua_pushboolean(
        L, h.connected && h.outgoing.size() < max_messages && h.outgoing_bytes < max_queued_bytes);
    if (!h.connected)
      lua_pushliteral(L, "closed");
    else
      lua_pushnil(L);
    return 3;
  }
  static int receive_message(lua_State* L) {
    auto& h = self(L);
    if (h.connected && !h.incoming.empty()) {
      const auto& bytes = h.incoming.front();
      push_blob(L, bytes);
      h.incoming_bytes -= bytes.size();
      h.incoming.pop_front();
      return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, h.connected ? "timeout" : "closed");
    return 2;
  }
  static int send_message(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto bytes = binary_arg(L, 1);
      if (bytes.empty() || bytes.size() > ui::max_payload_bytes)
        fail("invalid session message size");
      if (!h.connected || h.outgoing.size() >= max_messages ||
          bytes.size() > max_queued_bytes - h.outgoing_bytes) {
        lua_pushnil(L);
        lua_pushstring(L, h.connected ? "timeout" : "closed");
        return 2;
      }
      h.outgoing.emplace_back(bytes.begin(), bytes.end());
      h.outgoing_bytes += bytes.size();
      lua_pushboolean(L, 1);
      return 1;
    });
  }
  void clear_messages() {
    ++message_generation;
    incoming.clear();
    outgoing.clear();
    incoming_bytes = outgoing_bytes = 0;
  }
  static int reset(lua_State* L) {
    self(L).clear_messages();
    return 0;
  }
  static Task& task(lua_State* L) { return *static_cast<Task*>(luaL_checkudata(L, 1, task_type)); }
  static void task_destroy(lua_State*, void* memory) {
    auto& t = *static_cast<Task*>(memory);
    --t.owner->live_tasks;
    t.~Task();
  }
  static int task_create(lua_State* L) {
    auto& h = self(L);
    if (h.live_tasks >= h.limits.tasks) luaL_errorL(L, "too many live script tasks");
    auto* t = new (lua_newuserdatadtor(L, sizeof(Task), task_destroy)) Task{};
    t->owner = &h;
    t->id = h.next_task++;
    ++h.live_tasks;
    luaL_getmetatable(L, task_type);
    lua_setmetatable(L, -2);
    return 1;
  }
  void finish(Task& t) {
    t.busy = false;
    std::erase_if(scheduled, [&](const auto& e) { return e.id == t.id; });
    if (t.thread) lua_resetthread(t.thread);
    if (t.thread_ref != LUA_NOREF) lua_unref(state, t.thread_ref);
    if (t.self_ref != LUA_NOREF) lua_unref(state, t.self_ref);
    t.thread = nullptr;
    t.thread_ref = t.self_ref = LUA_NOREF;
  }
  static int task_run(lua_State* L) {
    auto& t = task(L);
    auto& h = self(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (t.running) luaL_errorL(L, "cannot restart a running task");
    h.finish(t);
    t.cancelled = false;
    ++t.generation;
    t.thread = lua_newthread(L);
    t.thread_ref = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_pushvalue(L, 2);
    lua_xmove(L, t.thread, 1);
    t.self_ref = lua_ref(L, 1);
    t.busy = true;
    h.scheduled.push_back({t.self_ref, t.id, t.generation});
    return 0;
  }
  static int task_stop(lua_State* L) {
    auto& t = task(L);
    t.cancelled = true;
    if (!t.running) self(L).finish(t);
    return 0;
  }
  static int task_busy(lua_State* L) {
    const auto& t = task(L);
    lua_pushboolean(L, t.busy && !t.cancelled);
    return 1;
  }
  static int task_name(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto name = string_arg(L, 2, 128);
      auto& t = task(L);
      if (name == "name") {
        lua_pushstring(L, t.name.data());
        return 1;
      }
      luaL_getmetatable(L, task_type);
      lua_pushvalue(L, 2);
      lua_rawget(L, -2);
      return 1;
    });
  }
  static int task_set(lua_State* L) {
    return boundary(L, [&](Impl&) {
      if (string_arg(L, 2, 128) != "name") fail("unknown task property");
      const auto name = string_arg(L, 3, 127);
      auto& t = task(L);
      t.name.fill(0);
      std::memcpy(t.name.data(), name.data(), name.size());
      return 0;
    });
  }
  static int task_class(lua_State* L) {
    lua_pushliteral(L, "DPLuaTask");
    return 1;
  }
  Impl(ResourceReader reader, Display d, Limits l, const TrustedDirectory* directory = nullptr)
      : display(std::move(d)),
        limits(l),
        root(directory ? ::open(directory->content_path.c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                       : -1),
        resource_reader(std::move(reader)) {
    if (!resource_reader && root.fd < 0) fail("cannot open trusted asset resources");
    if (display.identifier.empty() || display.identifier.size() > 256 ||
        display.identifier.find('\0') != display.identifier.npos || !display.width ||
        display.width > 16384 || !display.height || display.height > 16384 ||
        !std::isfinite(display.frame_duration) || display.frame_duration < .001 ||
        display.frame_duration > 1 || limits.memory_bytes < 1024 * 1024 ||
        limits.memory_bytes > 512 * 1024 * 1024 || !limits.tasks || limits.tasks > 1024 ||
        !limits.startup_ms || limits.startup_ms > 10000 || !limits.frame_ms ||
        limits.frame_ms > 1000)
      fail("invalid script runtime configuration");
    state = lua_newstate(allocate, this);
    if (!state) fail("cannot create asset VM");
    try {
      lua_callbacks(state)->userdata = this;
      lua_callbacks(state)->interrupt = interrupt;
      luaL_openlibs(state);
      for (const auto* name :
           {"os", "io", "package", "loadstring", "loadfile", "dofile", "require"}) {
        lua_pushnil(state);
        lua_setglobal(state, name);
      }
      luaL_newmetatable(state, blob_type);
      function(state, "__len", blob_length);
      function(state, "__tostring", blob_description);
      lua_pushliteral(state, "protected plist data");
      lua_setfield(state, -2, "__metatable");
      lua_setreadonly(state, -1, 1);
      lua_pop(state, 1);
      luaL_newmetatable(state, array_type);
      lua_setreadonly(state, -1, 1);
      lua_pop(state, 1);
      luaL_newmetatable(state, invalid_type);
      function(state, "__tostring", invalid_description);
      lua_pushliteral(state, "protected invalid vehicle value");
      lua_setfield(state, -2, "__metatable");
      lua_setreadonly(state, -1, 1);
      lua_pop(state, 1);
      (void)lua_newuserdata(state, 1);
      luaL_getmetatable(state, invalid_type);
      lua_setmetatable(state, -2);
      invalid_ref = lua_ref(state, -1);
      lua_pop(state, 1);
      luaL_newmetatable(state, task_type);
      function(state, "run", task_run);
      function(state, "stop", task_stop);
      function(state, "isBusy", task_busy);
      function(state, "getClassName", task_class);
      function(state, "__index", task_name);
      function(state, "__newindex", task_set);
      lua_setreadonly(state, -1, 1);
      lua_pop(state, 1);
      lua_newtable(state);
      function(state, "createAssetRAW", raw_asset);
      function(state, "parsePlist", parse);
      function(state, "createPlist", serialize);
      function(state, "getSystemUpTime", uptime);
      function(state, "getLogLevel", get_log);
      function(state, "setLogLevel", set_log);
      function(state, "log", log);
      function(state, "enumerateOutputStreams", output_streams);
      function(state, "enumerateInputStreams", input_streams);
      function(state, "signpostBeginInterval", signpost_begin);
      function(state, "signpostEndInterval", signpost_end);
      function(state, "signpostEmitEvent", signpost_event);
      function(state, "loadPersistentStorage", storage_load);
      function(state, "savePersistentStorage", storage_save);
      function(state, "readyToEnterForeground", ready_to_foreground);
      function(state, "getVehicleDataConfig", vehicle_config);
      function(state, "registerVehicleDataTypes", vehicle_register);
      function(state, "getVehicleData", vehicle_read);
      function(state, "getVehicleDataUpdates", vehicle_updates);
      function(state, "setVehicleData", vehicle_write);
      lua_getref(state, invalid_ref);
      lua_setfield(state, -2, "INVALIDVALUE");
      function(state, "UpdateVerifiedUIElements", update_verified_ui);
      function(state, "status", status);
      function(state, "receiveMessage", receive_message);
      function(state, "sendMessage", send_message);
      function(state, "reset", reset);
      graphics = std::make_unique<Graphics>(display.identifier,
                                            [this](std::string_view path) { return read(path); });
      graphics->install(state, -1);
      lua_pushliteral(state, "1.4.0");
      lua_setfield(state, -2, "version");  // Target host ABI; not a claim of finished conformance.
      lua_newtable(state);
      const std::array levels{"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "NONE"};
      for (std::size_t i = 0; i < levels.size(); ++i) {
        lua_pushinteger(state, i);
        lua_setfield(state, -2, levels[i]);
      }
      lua_setreadonly(state, -1, 1);
      lua_setfield(state, -2, "LOGLEVELS");
      lua_newtable(state);
      function(state, "create", task_create);
      lua_setreadonly(state, -1, 1);
      lua_setfield(state, -2, "DPLuaTask");
      lua_newtable(state);
      function(state, "__index", property);
      lua_setreadonly(state, -1, 1);
      lua_setmetatable(state, -2);
      lua_setreadonly(state, -1, 1);
      lua_setglobal(state, "CP");
      lua_pushcfunction(state, require, "require");
      lua_setglobal(state, "require");
      lua_pushcfunction(state, load_string, "loadstring");
      lua_setglobal(state, "loadstring");
      lua_pushcfunction(state, print, "print");
      lua_setglobal(state, "print");
      luaL_sandbox(state);
      script = lua_newthread(state);
      (void)lua_ref(state, -1);
      lua_pop(state, 1);
      luaL_sandboxthread(script);
      // The asset utility replaces _G's metatable when locking globals, so a
      // fallback __index is insufficient. Copy bindings into its private table;
      // the library tables and CP objects referenced here remain immutable.
      lua_pushnil(state);
      while (lua_next(state, LUA_GLOBALSINDEX)) {
        lua_pushvalue(state, -2);
        lua_pushvalue(state, -2);
        lua_xmove(state, script, 2);
        lua_rawset(script, LUA_GLOBALSINDEX);
        lua_pop(state, 1);
      }
      lua_pushvalue(script, LUA_GLOBALSINDEX);
      lua_setglobal(script, "_G");
      // Native userdata properties can change after subscription/presentation.
      // A global alias to a layer must not let GETIMPORT cache those properties
      // at chunk load time. Library/CP read-only flags remain in force; safeenv
      // only controls import and builtin fast-path assumptions, not the sandbox.
      lua_setsafeenv(script, LUA_GLOBALSINDEX, 0);
    } catch (...) {
      lua_close(state);
      state = nullptr;
      throw;
    }
  }
  ~Impl() {
    if (state) lua_close(state);
  }
  static std::uint64_t thread_cpu_ns() {
    timespec value{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return 0;
    return std::uint64_t(value.tv_sec) * 1000000000ULL + std::uint64_t(value.tv_nsec);
  }
  std::string failure_detail(const char* error) const {
    const auto wall =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - turn_started).count();
    const auto cpu = thread_cpu_ns();
    std::string frames;
    for (std::size_t i = 0; i < budget_frame_count; ++i) {
      if (i) frames += ',';
      frames += budget_frames[i].source.data();
      frames += ':';
      frames += std::to_string(budget_frames[i].line);
    }
    return std::string(error).substr(0, 4096) + " [turn wallUs=" + std::to_string(wall) +
           ";cpuUs=" +
           (turn_cpu_ns && cpu >= turn_cpu_ns ? std::to_string((cpu - turn_cpu_ns) / 1000)
                                              : "unavailable") +
           ";budgetMs=" + std::to_string(turn_budget_ms) +
           ";resumes=" + std::to_string(turn_resumes) +
           (frames.empty() ? "" : ";budgetStack=" + frames) + "]";
  }
  bool budget_expired(bool allow_early_wall_check = false) const {
    const auto now = Clock::now();
    if (now >= deadline) return true;
    if (!turn_cpu_ns) return false;  // Clock unavailable: retain the strict wall limit.
    // Thread CPU cannot consume the full budget in the first half of its
    // elapsed wall interval. Avoid a CPU-clock syscall at every VM loop/call
    // checkpoint during that conservative window. After it, check exact CPU
    // time at every checkpoint; completed/yielded VM turns always check it.
    if (allow_early_wall_check &&
        now - turn_started < std::chrono::milliseconds(turn_budget_ms / 2))
      return false;
    const auto cpu = thread_cpu_ns();
    return cpu >= turn_cpu_ns && cpu - turn_cpu_ns >= turn_budget_ms * 1000000ULL;
  }
  void begin(std::uint64_t milliseconds) {
    if (faulted) fail("asset VM is faulted");
    turn_started = Clock::now();
    turn_cpu_ns = thread_cpu_ns();
    turn_budget_ms = milliseconds;
    turn_resumes = 0;
    budget_frame_count = 0;
    // The Pi can deschedule this owner thread during a tiny Lua turn. CPU
    // time enforces the script budget; a 100 ms wall ceiling still bounds
    // blocked host callbacks and a missing CPU clock falls back to strict wall.
    deadline = turn_started +
               std::chrono::milliseconds(turn_cpu_ns ? std::max(milliseconds, std::uint64_t(100))
                                                     : milliseconds);
  }
  void check(int result, lua_State* L) {
    if (result != LUA_OK && result != LUA_YIELD) {
      faulted = true;
      const char* detail = lua_tostring(L, -1);
      throw std::runtime_error(detail ? std::string(detail).substr(0, 4096)
                                      : "asset script failed");
    }
    if (faulted || budget_expired()) {
      faulted = true;
      fail("asset execution budget exceeded");
    }
  }
};
Runtime::Runtime(ResourceReader reader, Display d, Limits l)
    : impl_(std::make_unique<Impl>(std::move(reader), std::move(d), l)) {}
Runtime::Runtime(const TrustedDirectory& a, Display d, Limits l)
    : impl_(std::make_unique<Impl>(ResourceReader{}, std::move(d), l, &a)) {}
void Runtime::select_remote_map_component() {
  auto& h = *impl_;
  if (h.faulted || h.evaluated) fail("remote map selection must precede asset evaluation");
  h.remote_map_selection = true;
}
void Runtime::select_startup_layout(std::string layout_id) {
  auto& h = *impl_;
  if (h.faulted || h.evaluated) fail("startup layout selection must precede asset evaluation");
  if (layout_id.size() != 36) fail("invalid startup layout identifier");
  for (std::size_t i = 0; i < layout_id.size(); ++i) {
    const auto c = layout_id[i];
    if ((i == 8 || i == 13 || i == 18 || i == 23)
            ? c != '-'
            : !(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'F'))
      fail("invalid startup layout identifier");
  }
  h.startup_layout_id = std::move(layout_id);
}
void Runtime::enable_scene_observation() {
  auto& h = *impl_;
  if (h.evaluated || h.faulted) fail("scene observation must precede asset evaluation");
  h.observe_scene = true;
}
Runtime::ScenePhase Runtime::scene_phase() {
  auto& h = *impl_;
  if (h.faulted) fail("cannot observe a faulted scene");
  if (!h.observe_scene || !h.evaluated || h.layout_controller_ref == LUA_NOREF)
    return ScenePhase::unavailable;
  const auto top = lua_gettop(h.script);
  h.begin(h.limits.frame_ms);
  try {
    if (h.scene_adapter_ref == LUA_NOREF) {
      h.compile(h.script, R"(
        return function(controller)
          local state = controller._workspace:getState()
          local phases = require('Workspace').STATE
          assert(type(state)=='table' and type(state.activeState)=='string' and #state.activeState<=64, 'invalid workspace state')
          assert(type(phases)=='table' and type(phases.LOGOS_TRANSITION)=='string' and type(phases.STARTUP)=='string' and type(phases.DRIVE)=='string', 'invalid workspace phases')
          assert(phases.LOGOS_TRANSITION~=phases.STARTUP and phases.STARTUP~=phases.DRIVE and phases.LOGOS_TRANSITION~=phases.DRIVE, 'ambiguous workspace phases')
          if state.activeState == phases.LOGOS_TRANSITION then return 1 end
          if state.activeState == phases.STARTUP then return 2 end
          if state.activeState == phases.DRIVE then return 3 end
          return 4
        end
      )",
                "host-scene-observer");
      h.check(lua_pcall(h.script, 0, 1, 0), h.script);
      h.scene_adapter_ref = lua_ref(h.script, -1);
      lua_pop(h.script, 1);
    }
    lua_getref(h.script, h.scene_adapter_ref);
    lua_getref(h.script, h.layout_controller_ref);
    h.check(lua_pcall(h.script, 1, 1, 0), h.script);
    if (!lua_isnumber(h.script, -1)) fail("invalid signed scene phase");
    const auto phase = lua_tonumber(h.script, -1);
    if (phase < 1 || phase > 4 || std::trunc(phase) != phase) fail("invalid signed scene phase");
    lua_settop(h.script, top);
    h.deadline = Clock::time_point::max();
    return static_cast<ScenePhase>(int(phase));
  } catch (const std::exception& error) {
    lua_settop(h.script, top);
    h.faulted = true;
    h.clear_messages();
    throw std::runtime_error(h.failure_detail(error.what()));
  }
}
void Runtime::enable_live_layout_selection() {
  auto& h = *impl_;
  if (h.evaluated || h.faulted) fail("live layout selection must precede asset evaluation");
  h.live_layout_selection = true;
}
LayoutSelection Runtime::layout_selection(std::optional<std::string_view> request) {
  return cluster_selection(
      request ? std::optional(ClusterSelectionRequest{std::string(*request), {}, {}, {}})
              : std::nullopt);
}
LayoutSelection Runtime::cluster_selection(const std::optional<ClusterSelectionRequest>& request) {
  auto& h = *impl_;
  LayoutSelection result;
  if (request) {
    for (const auto* id : {&request->layout, &request->zone, &request->component}) {
      if (id != &request->layout && id->empty()) continue;
      if (id->size() != 36) throw std::invalid_argument("invalid cluster identifier");
      for (std::size_t i = 0; i < id->size(); ++i) {
        const auto c = (*id)[i];
        if ((i == 8 || i == 13 || i == 18 || i == 23)
                ? c != '-'
                : !(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'F'))
          throw std::invalid_argument("invalid cluster identifier");
      }
    }
    if (request->zone.empty() != request->component.empty() || request->palette.size() > 64 ||
        request->palette.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") !=
            request->palette.npos)
      throw std::invalid_argument("invalid cluster appearance request");
  }
  if (!h.live_layout_selection || !h.evaluated || h.layout_controller_ref == LUA_NOREF)
    return result;
  const auto top = lua_gettop(h.script);
  h.begin(h.limits.frame_ms);
  try {
    lua_getref(h.script, h.layout_adapter_ref);
    lua_getref(h.script, h.layout_controller_ref);
    if (request)
      lua_pushlstring(h.script, request->layout.data(), request->layout.size());
    else
      lua_pushnil(h.script);
    lua_pushboolean(h.script, h.connected && h.foregrounded);
    for (const auto* option :
         {request ? &request->palette : nullptr, request ? &request->zone : nullptr,
          request ? &request->component : nullptr}) {
      if (option && !option->empty())
        lua_pushlstring(h.script, option->data(), option->size());
      else
        lua_pushnil(h.script);
    }
    h.check(lua_pcall(h.script, 6, 10, 0), h.script);
    const auto text = [&](int index, std::size_t limit) {
      if (lua_type(h.script, index) != LUA_TSTRING) fail("invalid layout text field");
      std::size_t size = 0;
      const auto* bytes = lua_tolstring(h.script, index, &size);
      if (!bytes || size > limit) fail("oversized layout text field");
      return std::string(bytes, size);
    };
    const auto catalog = top + 1;
    if (!lua_istable(h.script, catalog) || lua_objlen(h.script, catalog) > 64)
      fail("invalid layout catalogue");
    std::set<std::string> unique;
    for (int i = 1; i <= lua_objlen(h.script, catalog); ++i) {
      lua_rawgeti(h.script, catalog, i);
      if (!lua_istable(h.script, -1)) fail("invalid layout catalogue object");
      lua_getfield(h.script, -1, "id");
      const auto id = text(-1, 36);
      lua_pop(h.script, 1);
      lua_getfield(h.script, -1, "name");
      const auto name = text(-1, 256);
      lua_pop(h.script, 2);
      if (id.empty() || !valid_text(id) || !valid_text(name) || !unique.insert(id).second)
        fail("invalid layout catalogue entry");
      result.choices.push_back({id, name});
    }
    result.active = text(top + 2, 36);
    result.ready = lua_toboolean(h.script, top + 3);
    result.transitioning = lua_toboolean(h.script, top + 4);
    result.idle = lua_toboolean(h.script, top + 6);
    const auto outcome = lua_tointeger(h.script, top + 5);
    if (outcome < 0 || outcome > 4) fail("invalid layout selection result");
    result.result = static_cast<LayoutRequest>(outcome);
    result.active_palette = text(top + 9, 64);
    if (!lua_isnil(h.script, top + 10)) {
      if (!lua_istable(h.script, top + 10)) fail("invalid cluster video diagnostic");
      ClusterVideoState video;
      lua_getfield(h.script, top + 10, "visible");
      if (!lua_isboolean(h.script, -1)) fail("invalid cluster video visibility");
      video.visible = lua_toboolean(h.script, -1);
      lua_pop(h.script, 1);
      lua_getfield(h.script, top + 10, "hasCurrentPTS");
      if (!lua_isboolean(h.script, -1)) fail("invalid cluster video timestamp availability");
      video.has_current_pts = lua_toboolean(h.script, -1);
      lua_pop(h.script, 1);
      lua_getfield(h.script, top + 10, "opacity");
      if (!lua_isnumber(h.script, -1)) fail("invalid cluster video opacity");
      video.opacity = lua_tonumber(h.script, -1);
      lua_pop(h.script, 1);
      if (!std::isfinite(video.opacity) || video.opacity < 0 || video.opacity > 1)
        fail("invalid cluster video opacity bounds");
      result.phone_video = video;
    }
    for (const auto table : {top + 7, top + 8}) {
      if (!lua_istable(h.script, table) || lua_objlen(h.script, table) > 128)
        fail("invalid cluster option catalogue");
      std::set<std::string> identifiers;
      for (int i = 1; i <= lua_objlen(h.script, table); ++i) {
        lua_rawgeti(h.script, table, i);
        if (!lua_istable(h.script, -1)) fail("invalid cluster option");
        lua_getfield(h.script, -1, "id");
        const auto id = text(-1, table == top + 7 ? 64 : 36);
        lua_pop(h.script, 1);
        lua_getfield(h.script, -1, "name");
        const auto name = text(-1, 256);
        lua_pop(h.script, 1);
        if (id.empty() || !valid_text(id) || !valid_text(name)) fail("invalid cluster option text");
        if (table == top + 7) {
          if (!identifiers.insert(id).second) fail("duplicate cluster palette");
          result.palettes.push_back({id, name});
        } else {
          lua_getfield(h.script, -1, "zone");
          const auto zone = text(-1, 36);
          lua_pop(h.script, 1);
          lua_getfield(h.script, -1, "active");
          const bool active = lua_toboolean(h.script, -1);
          lua_pop(h.script, 1);
          lua_getfield(h.script, -1, "available");
          if (!lua_isboolean(h.script, -1)) fail("invalid cluster content availability");
          const bool available = lua_toboolean(h.script, -1);
          lua_pop(h.script, 1);
          if (zone.empty() || !identifiers.insert(zone + id).second)
            fail("duplicate cluster content");
          result.contents.push_back({zone, id, name, active, available});
        }
        lua_pop(h.script, 1);
      }
    }
    lua_settop(h.script, top);
    h.deadline = Clock::time_point::max();
    return result;
  } catch (const std::exception& error) {
    lua_settop(h.script, top);
    h.faulted = true;
    h.clear_messages();
    throw std::runtime_error(h.failure_detail(error.what()));
  }
}
void Runtime::bind_log_sink(std::function<void(int, std::string_view, std::string_view)> sink) {
  impl_->log_sink = std::move(sink);
}
void Runtime::bind_session_diagnostic_sink(std::function<void(const SessionDiagnostic&)> sink,
                                           std::size_t max_events) {
  auto& h = *impl_;
  if (h.evaluated || !sink || max_events == 0 || max_events > 256)
    throw std::invalid_argument(
        "session diagnostic sink requires pre-evaluation binding and a 1..256 event budget");
  h.session_diagnostic_sink = std::move(sink);
  h.session_diagnostic_limit = max_events;
}
void Runtime::bind_persistent_storage(
    std::function<std::optional<Bytes>(std::string_view)> read,
    std::function<void(std::string_view, std::span<const std::uint8_t>)> write) {
  auto& h = *impl_;
  if (h.evaluated || !read || !write) fail("persistent storage must be bound before evaluation");
  h.storage_read = std::move(read);
  h.storage_write = std::move(write);
}
void Runtime::bind_custom_image_provider(
    std::function<std::optional<Bytes>(std::string_view, int)> provider) {
  auto& h = *impl_;
  if (h.evaluated || !provider) fail("custom image provider must be bound before evaluation");
  h.graphics->bind_custom_image_provider(std::move(provider));
}
Runtime::~Runtime() = default;
void Runtime::evaluate(std::string_view source, std::string_view label) {
  auto& h = *impl_;
  h.begin(h.limits.startup_ms);
  h.evaluated = true;
  if (label.size() > 256 || label.find('\0') != label.npos) fail("invalid script label");
  try {
    // Compile the fixed host adapter once within the existing startup budget.
    // Catalogue/control turns only execute it; they retain their frame budget.
    if (h.live_layout_selection) h.prepare_layout_adapter(h.script);
    h.compile(h.script, source, std::string(label));
    h.check(lua_pcall(h.script, 0, 0, 0), h.script);
    lua_settop(h.script, 0);
    h.deadline = Clock::time_point::max();
  } catch (const std::exception& error) {
    h.faulted = true;
    h.clear_messages();
    throw std::runtime_error(h.failure_detail(error.what()));
  }
}
void Runtime::tick(std::optional<double> pts) {
  run_tasks(pts, false);
}
void Runtime::start_tasks(std::optional<double> pts) {
  run_tasks(pts, true);
}
void Runtime::run_tasks(std::optional<double> pts, bool startup) {
  auto& h = *impl_;
  if (startup && (!h.evaluated || h.tasks_started)) fail("startup task turn is unavailable");
  if (pts && (!std::isfinite(*pts) || *pts < 0 || (h.pts && *pts < *h.pts)))
    fail("invalid presented timestamp");
  if (h.evaluated) h.tasks_started = true;
  h.pts = pts;
  h.begin(startup ? h.limits.startup_ms : h.limits.frame_ms);
  const auto scheduled = h.scheduled;
  try {
    for (const auto& entry : scheduled) {
      lua_getref(h.state, entry.reference);
      auto* t = static_cast<Impl::Task*>(test_userdata(h.state, -1, task_type));
      if (t && t->id == entry.id && t->generation == entry.generation && t->busy && !t->cancelled) {
        t->running = true;
        ++h.turn_resumes;
        const auto result = lua_resume(t->thread, nullptr, 0);
        t->running = false;
        h.check(result, t->thread);
        if (result == LUA_OK || t->cancelled)
          h.finish(*t);
        else
          lua_settop(t->thread, 0);
      }
      lua_pop(h.state, 1);
    }
    h.deadline = Clock::time_point::max();
  } catch (const std::exception& error) {
    h.faulted = true;
    h.clear_messages();
    throw std::runtime_error(h.failure_detail(error.what()));
  }
}
void Runtime::set_connected(bool connected) {
  auto& h = *impl_;
  if (connected != h.connected) h.clear_messages();
  h.connected = connected;
}
void Runtime::set_foregrounded(bool visible) {
  auto& h = *impl_;
  if (h.faulted) fail("asset VM is faulted");
  h.foregrounded = visible;
}
bool Runtime::receive(Bytes bytes) {
  auto& h = *impl_;
  if (!h.connected || h.faulted || bytes.empty() || bytes.size() > ui::max_payload_bytes ||
      h.incoming.size() >= max_messages || bytes.size() > max_queued_bytes - h.incoming_bytes)
    return false;
  h.incoming_bytes += bytes.size();
  h.incoming.push_back(std::move(bytes));
  return true;
}
std::optional<Bytes> Runtime::take_outgoing() {
  auto& h = *impl_;
  if (h.faulted || h.outgoing.empty()) return {};
  auto bytes = std::move(h.outgoing.front());
  h.outgoing.pop_front();
  h.outgoing_bytes -= bytes.size();
  return bytes;
}
std::uint64_t Runtime::message_generation() const {
  return impl_->message_generation;
}
graphics::Image Runtime::render(graphics::RenderCosts* costs) {
  auto& h = *impl_;
  if (h.faulted) fail("asset VM is faulted");
  try {
    return graphics::render(h.graphics->root(), h.display.width, h.display.height, costs);
  } catch (...) {
    h.faulted = true;
    h.clear_messages();
    throw;
  }
}
Stats Runtime::stats() const {
  const auto& h = *impl_;
  return {h.memory, h.modules.size(), h.scheduled.size(),
          h.logs,   h.faulted,        h.foreground_ready_requested};
}
std::shared_ptr<const graphics::Layer> Runtime::scene_root() const {
  const auto& h = *impl_;
  if (h.faulted) fail("asset VM is faulted");
  return h.graphics->root();
}
std::shared_ptr<video::Stream> Runtime::add_video_input(video::Input input) {
  auto& h = *impl_;
  if (h.faulted) fail("asset VM is faulted");
  return h.graphics->add_video_input(std::move(input));
}
void Runtime::bind_vehicle(std::shared_ptr<vehicle::ReadOnlyService> source,
                           std::vector<vehicle::ScriptBinding> bindings,
                           std::function<std::uint64_t()> clock,
                           vehicle::ScriptView::Dismissals dismissals) {
  auto& h = *impl_;
  if (h.faulted || h.evaluated) fail("vehicle binding must precede asset evaluation");
  auto view = std::make_unique<vehicle::ScriptView>(std::move(source), std::move(bindings),
                                                    std::move(dismissals));
  h.vehicle = std::move(view);
  if (clock) h.vehicle_clock = std::move(clock);
}
void Runtime::bind_verification_reporter(std::function<void(const VerificationReport&)> reporter) {
  auto& h = *impl_;
  if (h.faulted || h.evaluated || !reporter)
    fail("verification receiver must be configured before asset evaluation");
  h.verification_reporter = std::move(reporter);
}
}  // namespace headunit::theme::script
