// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/lua_host.hpp"
#include "headunit/binary_plist.hpp"
#include "theme/script_graphics.hpp"
#include <lua.h>
#include <lualib.h>
#include <Luau/Compiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>

namespace headunit {
namespace {
constexpr std::size_t message_limit = 512 * 1024, queue_limit = 2 * 1024 * 1024;
constexpr const char* task_type = "headunit.task";
constexpr const char* array_type = "headunit.plist.array";
constexpr const char* blob_type = "headunit.bytes";
struct Blob {
  std::size_t size;
};  // Bytes follow this header, owned by Lua's allocator.
}  // namespace
struct LuaHost::Impl {
  ResourceReader reader;
  std::vector<OutputStream> outputs;
  std::unique_ptr<theme::script::Graphics> graphics;
  std::optional<double> pts;
  lua_State* state = nullptr;
  std::size_t used = 0, limit;
  double now = 0;
  bool connected = false, failed = false;
  std::size_t resets = 0, inbound_bytes = 0, outbound_bytes = 0;
  std::deque<Bytes> inbound;
  std::vector<Bytes> outbound;
  std::vector<std::string> logs;
  int log_level = 2;
  struct Task {
    lua_State* thread = nullptr;
    int ref = LUA_NOREF;
    bool active = false;
    bool collected = false;
  };
  struct TaskHandle {
    Impl* owner;
    std::uint64_t id;
  };
  std::map<std::uint64_t, Task> tasks;
  std::uint64_t next_task = 1;
  std::size_t safepoints = 0, dynamic_bytes = 0;
  Bytes scratch;
  std::string module_path, chunk_name, bytecode;
  char error[512]{};
  bool closing = false;

  explicit Impl(ResourceReader resources, std::size_t maximum, std::vector<OutputStream> streams)
      : reader(std::move(resources)), outputs(std::move(streams)), limit(maximum) {
    if (!reader || limit < 1024 * 1024 || limit > 256 * 1024 * 1024)
      throw std::invalid_argument("invalid Lua host configuration");
    if (outputs.size() > 1) throw std::invalid_argument("too many theme outputs");
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      const auto& output = outputs[i];
      if (output.identifier.empty() || output.identifier.size() > 128 ||
          output.identifier.find('\0') != std::string::npos || !output.width ||
          output.width > 8192 || !output.height || output.height > 8192 ||
          !std::isfinite(output.frame_duration) || output.frame_duration < 0.001 ||
          output.frame_duration > 1)
        throw std::invalid_argument("invalid theme output descriptor");
      for (std::size_t j = 0; j < i; ++j)
        if (outputs[j].identifier == output.identifier)
          throw std::invalid_argument("duplicate theme output");
    }
    graphics = std::make_unique<theme::script::Graphics>(
        outputs.empty() ? "" : outputs[0].identifier, [this](std::string_view name) {
          const auto bytes = reader(std::string(name));
          return std::string(bytes.begin(), bytes.end());
        });
    state = lua_newstate(allocate, this);
    if (!state) throw std::runtime_error("cannot create Lua state");
    lua_setthreaddata(state, this);
    lua_callbacks(state)->userthread = [](lua_State* parent, lua_State* child) {
      if (parent) lua_setthreaddata(child, lua_getthreaddata(parent));
    };
    lua_pushcfunction(state, initialize, "initialize");
    if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
      lua_close(state);
      state = nullptr;
      throw std::runtime_error("Lua host initialization failed");
    }
    lua_callbacks(state)->interrupt = interrupt;
  }
  ~Impl() {
    if (state) {
      closing = true;
      lua_close(state);
    }
  }
  static void* allocate(void* context, void* pointer, std::size_t old_size, std::size_t new_size) {
    auto& self = *static_cast<Impl*>(context);
    if (!pointer) old_size = 0;  // Lua uses osize as a type tag for fresh allocations.
    if (!new_size) {
      std::free(pointer);
      self.used -= old_size;
      return nullptr;
    }
    if (old_size > self.used || new_size > self.limit - (self.used - old_size)) return nullptr;
    void* result = std::realloc(pointer, new_size);
    if (result) self.used = self.used - old_size + new_size;
    return result;
  }
  static Impl& host(lua_State* lua) { return *static_cast<Impl*>(lua_getthreaddata(lua)); }
  static void interrupt(lua_State* lua, int gc) {
    if (gc >= 0) return;  // Never throw while collecting userdata.
    auto& self = host(lua);
    if (!self.safepoints) luaL_errorL(lua, "Luau execution budget exceeded");
    --self.safepoints;
  }
  void budget(std::size_t count) { safepoints = count; }
  int load(lua_State* lua, std::string_view source, const char* name) {
    // Compile verified text ourselves; external/precompiled bytecode is never loaded.
    bytecode = Luau::compile(std::string(source));
    return luau_load(lua, name, bytecode.data(), bytecode.size(), 0);
  }
  static void task_destroy(lua_State*, void* pointer) {
    auto* handle = static_cast<TaskHandle*>(pointer);
    if (handle->owner->closing) return;
    const auto found = handle->owner->tasks.find(handle->id);
    if (found != handle->owner->tasks.end()) {
      found->second.collected = true;
      found->second.active = false;
    }
  }
  void cleanup() {
    for (auto it = tasks.begin(); it != tasks.end();) {
      if (it->second.collected) {
        stop(it->first);
        it = tasks.erase(it);
      } else
        ++it;
    }
  }
  static void blob(lua_State* lua, const Bytes& bytes) {
    auto* value = static_cast<Blob*>(lua_newuserdatatagged(lua, sizeof(Blob) + bytes.size(), 1));
    value->size = bytes.size();
    if (!bytes.empty()) std::memcpy(value + 1, bytes.data(), bytes.size());
    luaL_getmetatable(lua, blob_type);
    lua_setmetatable(lua, -2);
  }
  static std::span<const std::uint8_t> bytes(lua_State* lua, int index) {
    if (auto* value = static_cast<Blob*>(lua_touserdatatagged(lua, index, 1)))
      return {reinterpret_cast<const std::uint8_t*>(value + 1), value->size};
    if (lua_type(lua, index) != LUA_TSTRING) throw std::invalid_argument("expected binary message");
    std::size_t size = 0;
    const auto* value = lua_tolstring(lua, index, &size);
    return {reinterpret_cast<const std::uint8_t*>(value), size};
  }
  struct Conversion {
    std::size_t nodes = 0, bytes = 0;
    std::vector<const void*> path;
    void charge(std::size_t size) {
      if (size > plist_expansion_limit - bytes)
        throw std::invalid_argument("plist conversion limit exceeded");
      bytes += size;
    }
    void add(unsigned depth, std::size_t size = 0) {
      if (depth > plist_depth_limit || ++nodes > plist_node_limit)
        throw std::invalid_argument("plist conversion limit exceeded");
      charge(size);
    }
  };
  PlistValue from_lua(lua_State* lua, int index, unsigned depth, Conversion& limits) {
    limits.add(depth);
    index = lua_absindex(lua, index);
    if (!lua_checkstack(lua, 6)) throw std::runtime_error("plist Lua stack exhausted");
    switch (lua_type(lua, index)) {
      case LUA_TBOOLEAN:
        return PlistValue{lua_toboolean(lua, index) != 0};
      case LUA_TNUMBER: {
        const double value = lua_tonumber(lua, index);
        if (!std::isfinite(value)) throw std::invalid_argument("nonfinite plist number");
        if (std::trunc(value) == value) {
          if (std::abs(value) > 9007199254740991.0)
            throw std::invalid_argument("plist integer exceeds exact Lua range");
          return PlistValue{static_cast<std::int64_t>(value)};
        }
        return PlistValue{value};
      }
      case LUA_TSTRING: {
        std::size_t size = 0;
        const char* text = lua_tolstring(lua, index, &size);
        limits.charge(size);
        return PlistValue{std::string(text, size)};
      }
      case LUA_TUSERDATA: {
        const auto data = bytes(lua, index);
        limits.charge(data.size());
        return PlistValue{PlistValue::Bytes(data.begin(), data.end())};
      }
      case LUA_TTABLE: {
        const void* identity = lua_topointer(lua, index);
        if (std::find(limits.path.begin(), limits.path.end(), identity) != limits.path.end())
          throw std::invalid_argument("cyclic plist table");
        limits.path.push_back(identity);
        std::size_t integers = 0, strings = 0, maximum = 0;
        bool array = false;
        if (lua_getmetatable(lua, index)) {
          luaL_getmetatable(lua, array_type);
          array = lua_rawequal(lua, -1, -2);
          lua_pop(lua, 2);
        }
        lua_pushnil(lua);
        while (lua_next(lua, index)) {
          if (lua_type(lua, -2) == LUA_TNUMBER) {
            const double key = lua_tonumber(lua, -2);
            if (!std::isfinite(key) || key < 1 || key > plist_node_limit || std::trunc(key) != key)
              throw std::invalid_argument("invalid plist array index");
            ++integers;
            maximum = std::max(maximum, static_cast<std::size_t>(key));
          } else if (lua_type(lua, -2) == LUA_TSTRING)
            ++strings;
          else
            throw std::invalid_argument("invalid plist dictionary key");
          if (integers + strings > plist_node_limit)
            throw std::invalid_argument("too many plist entries");
          lua_pop(lua, 1);
        }
        if ((integers && strings) || (array && strings) || maximum != integers)
          throw std::invalid_argument("mixed or sparse plist table");
        if (array || integers) {
          PlistValue::Array result;
          result.reserve(integers);
          for (std::size_t i = 1; i <= integers; ++i) {
            lua_rawgeti(lua, index, static_cast<int>(i));
            result.push_back(from_lua(lua, -1, depth + 1, limits));
            lua_pop(lua, 1);
          }
          limits.path.pop_back();
          return PlistValue{std::move(result)};
        }
        PlistValue::Dictionary result;
        lua_pushnil(lua);
        while (lua_next(lua, index)) {
          std::size_t size = 0;
          const char* key = lua_tolstring(lua, -2, &size);
          limits.add(depth + 1, size);
          result.emplace(std::string(key, size), from_lua(lua, -1, depth + 1, limits));
          lua_pop(lua, 1);
        }
        limits.path.pop_back();
        return PlistValue{std::move(result)};
      }
      default:
        throw std::invalid_argument("unsupported plist Lua value");
    }
  }
  void to_lua(lua_State* lua, const PlistValue& value) {
    if (!lua_checkstack(lua, 6)) throw std::runtime_error("plist Lua stack exhausted");
    if (const auto* v = std::get_if<bool>(&value.value))
      lua_pushboolean(lua, *v);
    else if (const auto* v = std::get_if<std::int64_t>(&value.value)) {
      if (*v < -9007199254740991LL || *v > 9007199254740991LL)
        throw std::invalid_argument("plist integer exceeds exact Lua range");
      lua_pushnumber(lua, static_cast<double>(*v));
    } else if (const auto* v = std::get_if<double>(&value.value))
      lua_pushnumber(lua, *v);
    else if (const auto* v = std::get_if<std::string>(&value.value))
      lua_pushlstring(lua, v->data(), v->size());
    else if (const auto* v = std::get_if<PlistValue::Bytes>(&value.value))
      blob(lua, *v);
    else if (const auto* v = std::get_if<PlistValue::Array>(&value.value)) {
      lua_createtable(lua, static_cast<int>(v->size()), 0);
      for (std::size_t i = 0; i < v->size(); ++i) {
        to_lua(lua, (*v)[i]);
        lua_rawseti(lua, -2, static_cast<int>(i + 1));
      }
      luaL_getmetatable(lua, array_type);
      lua_setmetatable(lua, -2);
    } else if (const auto* v = std::get_if<PlistValue::Dictionary>(&value.value)) {
      lua_createtable(lua, 0, static_cast<int>(v->size()));
      for (const auto& [key, child] : *v) {
        lua_pushlstring(lua, key.data(), key.size());
        to_lua(lua, child);
        lua_rawset(lua, -3);
      }
    }
  }
  static int raw_length(lua_State* lua) {
    auto* value = static_cast<Blob*>(luaL_checkudata(lua, 1, blob_type));
    lua_pushinteger(lua, static_cast<lua_Integer>(value->size));
    return 1;
  }
  static int missing(lua_State* lua) {
    const auto* name = lua_tostring(lua, 2);
    if (name && std::strcmp(name, "PTS") == 0) {
      const auto pts = host(lua).pts;
      if (pts)
        lua_pushnumber(lua, *pts);
      else
        lua_pushnil(lua);
      return 1;
    }
    luaL_errorL(lua, "unsupported CP API: %s", name ? name : "unknown");
  }
  enum class Call {
    resource,
    uptime,
    status,
    receive,
    send,
    reset,
    log,
    log_get,
    log_set,
    task_create,
    task_run,
    task_stop,
    task_busy,
    search,
    plist_create,
    plist_parse,
    output_streams,
    input_streams,
    load_string
  };
  static void add(lua_State* lua, const char* name, Call call) {
    lua_pushinteger(lua, static_cast<lua_Integer>(call));
    lua_pushcclosure(lua, dispatch, name, 1);
    lua_setfield(lua, -2, name);
  }
  static int dispatch(lua_State* lua) {
    auto& self = host(lua);
    try {
      return self.call(lua, static_cast<Call>(lua_tointeger(lua, lua_upvalueindex(1))));
    } catch (const std::exception& error) {
      std::snprintf(self.error, sizeof(self.error), "%s", error.what());
    }
    // Convert resource/queue exceptions into a protected script error.
    luaL_errorL(lua, "%s", self.error);
  }
  static int initialize(lua_State* lua) {
    luaL_openlibs(lua);
    for (const char* name :
         {"io", "os", "dofile", "loadfile", "load", "loadstring", "print", "collectgarbage"}) {
      lua_pushnil(lua);
      lua_setglobal(lua, name);
    }
    lua_pushinteger(lua, static_cast<int>(Call::load_string));
    lua_pushcclosure(lua, dispatch, "loadstring", 1);
    lua_setglobal(lua, "loadstring");
    lua_newtable(lua);
    lua_setfield(lua, LUA_REGISTRYINDEX, "headunit.modules");
    lua_pushinteger(lua, static_cast<int>(Call::search));
    lua_pushcclosure(lua, dispatch, "require", 1);
    lua_setglobal(lua, "require");
    luaL_newmetatable(lua, array_type);
    lua_pushliteral(lua, "plist array");
    lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    luaL_newmetatable(lua, blob_type);
    lua_pushcfunction(lua, raw_length, "bytes.length");
    lua_setfield(lua, -2, "__len");
    lua_pushliteral(lua, "binary data");
    lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    luaL_newmetatable(lua, task_type);
    add(lua, "run", Call::task_run);
    add(lua, "stop", Call::task_stop);
    add(lua, "isBusy", Call::task_busy);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, -2, "__index");
    lua_pushliteral(lua, "task");
    lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    lua_newtable(lua);
    add(lua, "enumerateOutputStreams", Call::output_streams);
    add(lua, "enumerateInputStreams", Call::input_streams);
    add(lua, "createPlist", Call::plist_create);
    add(lua, "parsePlist", Call::plist_parse);
    add(lua, "createAssetRAW", Call::resource);
    add(lua, "getSystemUpTime", Call::uptime);
    add(lua, "status", Call::status);
    add(lua, "receiveMessage", Call::receive);
    add(lua, "sendMessage", Call::send);
    add(lua, "reset", Call::reset);
    add(lua, "log", Call::log);
    add(lua, "getLogLevel", Call::log_get);
    add(lua, "setLogLevel", Call::log_set);
    lua_newtable(lua);
    add(lua, "create", Call::task_create);
    lua_setfield(lua, -2, "DPLuaTask");
    lua_newtable(lua);
    int index = 0;
    for (const char* name : {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "NONE"}) {
      lua_pushinteger(lua, index++);
      lua_setfield(lua, -2, name);
    }
    lua_setfield(lua, -2, "LOGLEVELS");
    host(lua).graphics->install(lua, -1);
    lua_newtable(lua);
    lua_pushcfunction(lua, missing, "CP.missing");
    lua_setfield(lua, -2, "__index");
    lua_setmetatable(lua, -2);
    lua_setglobal(lua, "CP");
    luaL_sandbox(lua);
    lua_pushvalue(lua, LUA_GLOBALSINDEX);
    const int original_globals = lua_gettop(lua);
    luaL_sandboxthread(lua);
    // Our earlier full runtime supports the theme's replacement of _G's
    // metatable. Copy immutable bindings into its writable private globals.
    lua_pushnil(lua);
    while (lua_next(lua, original_globals)) {
      lua_pushvalue(lua, -2);
      lua_pushvalue(lua, -2);
      lua_rawset(lua, LUA_GLOBALSINDEX);
      lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
    lua_pushvalue(lua, LUA_GLOBALSINDEX);
    lua_setglobal(lua, "_G");
    // Theme globals and userdata properties change between callbacks. Do not
    // cache a mutable lookup (for example movie.active) as a constant import.
    lua_setsafeenv(lua, LUA_GLOBALSINDEX, false);
    return 0;
  }
  std::uint64_t task_id(lua_State* lua) {
    return static_cast<TaskHandle*>(luaL_checkudata(lua, 1, task_type))->id;
  }
  void stop(std::uint64_t id) {
    if (auto found = tasks.find(id); found != tasks.end()) {
      found->second.active = false;
      if (found->second.ref != LUA_NOREF) lua_unref(state, found->second.ref);
      found->second.ref = LUA_NOREF;
      found->second.thread = nullptr;
    }
  }
  int call(lua_State* lua, Call which) {
    switch (which) {
      case Call::load_string: {
        // Reused from our earlier full runtime: the theme's animation helpers
        // compile easing expressions. This accepts bounded source, not bytecode.
        std::size_t size = 0;
        const char* source = luaL_checklstring(lua, 1, &size);
        if (size > 65536 || size > 1024 * 1024 - dynamic_bytes)
          throw std::invalid_argument("dynamic source budget exceeded");
        dynamic_bytes += size;
        const char* label = lua_isnoneornil(lua, 2) ? "asset-expression" : luaL_checkstring(lua, 2);
        if (std::strlen(label) > 256) throw std::invalid_argument("dynamic source label too long");
        if (load(lua, {source, size}, label) == LUA_OK) return 1;
        lua_pushnil(lua);
        lua_insert(lua, -2);
        return 2;
      }
      case Call::input_streams:
        return graphics->enumerate_inputs(lua);
      case Call::output_streams: {
        lua_createtable(lua, static_cast<int>(outputs.size()), 0);
        for (std::size_t i = 0; i < outputs.size(); ++i) {
          const auto& output = outputs[i];
          lua_createtable(lua, 0, 4);
          lua_pushlstring(lua, output.identifier.data(), output.identifier.size());
          lua_setfield(lua, -2, "identifier");
          lua_pushinteger(lua, static_cast<int>(output.width));
          lua_setfield(lua, -2, "width");
          lua_pushinteger(lua, static_cast<int>(output.height));
          lua_setfield(lua, -2, "height");
          lua_pushnumber(lua, output.frame_duration);
          lua_setfield(lua, -2, "frameDuration");
          lua_rawseti(lua, -2, static_cast<int>(i + 1));
        }
        return 1;
      }
      case Call::plist_create: {
        Conversion limits;
        auto value = from_lua(lua, 1, 0, limits);
        auto encoded = encode_binary_plist(value);
        blob(lua, encoded);
        return 1;
      }
      case Call::plist_parse: {
        try {
          auto value = decode_binary_plist(bytes(lua, 1));
          to_lua(lua, value);
          return 1;
        } catch (const std::invalid_argument&) {
          lua_settop(lua, 1);
          lua_pushnil(lua);
          lua_pushliteral(lua, "invalid plist");
          return 2;
        }
      }
      case Call::search: {
        std::size_t size = 0;
        const char* name = luaL_checklstring(lua, 1, &size);
        if (!size || size > 512 || std::memchr(name, '\0', size))
          throw std::invalid_argument("invalid module name");
        module_path.assign(name, size);
        if (module_path.find('/') == std::string::npos)
          std::replace(module_path.begin(), module_path.end(), '.', '/');
        module_path += ".lua";
        lua_settop(lua, 1);
        lua_pushlstring(lua, module_path.data(),
                        module_path.size());  // 2: canonical name survives nested require.
        lua_getfield(lua, LUA_REGISTRYINDEX, "headunit.modules");  // 3: cache
        lua_pushvalue(lua, 2);
        lua_rawget(lua, 3);
        if (lua_islightuserdata(lua, -1)) throw std::invalid_argument("cyclic module dependency");
        if (!lua_isnil(lua, -1)) return 1;
        lua_pop(lua, 1);
        chunk_name = "@" + module_path;
        scratch = reader(module_path);
        if (scratch.empty() || scratch.size() > 1024 * 1024)
          throw std::invalid_argument("module exceeds source limit");
        if (load(lua, {reinterpret_cast<const char*>(scratch.data()), scratch.size()},
                 chunk_name.c_str()) != LUA_OK)
          lua_error(lua);
        lua_pushvalue(lua, 2);
        lua_pushlightuserdata(lua, this);
        lua_rawset(lua, 3);
        if (lua_pcall(lua, 0, 1, 0) != LUA_OK) {
          lua_pushvalue(lua, 2);
          lua_pushnil(lua);
          lua_rawset(lua, 3);
          lua_error(lua);
        }
        if (lua_isnil(lua, -1)) {
          lua_pop(lua, 1);
          lua_pushboolean(lua, true);
        }
        lua_pushvalue(lua, 2);
        lua_pushvalue(lua, -2);
        lua_rawset(lua, 3);
        return 1;
      }
      case Call::resource: {
        std::size_t size = 0;
        const char* name = luaL_checklstring(lua, 1, &size);
        if (!size || size > 1024 || std::memchr(name, '\0', size))
          throw std::invalid_argument("invalid resource name");
        module_path.assign(name, size);
        scratch = reader(module_path);
        if (scratch.size() > 16 * 1024 * 1024) throw std::invalid_argument("raw asset too large");
        blob(lua, scratch);
        return 1;
      }
      case Call::uptime:
        lua_pushnumber(lua, now);
        return 1;
      case Call::status:
        lua_pushboolean(lua, connected && !inbound.empty());
        lua_pushboolean(lua, connected && outbound.size() < 64 && outbound_bytes < queue_limit);
        if (!connected)
          lua_pushliteral(lua, "closed");
        else
          lua_pushnil(lua);
        return 3;
      case Call::receive:
        if (!inbound.empty() && connected) {
          blob(lua, inbound.front());
          inbound_bytes -= inbound.front().size();
          inbound.pop_front();
          return 1;
        }
        lua_pushnil(lua);
        lua_pushstring(lua, connected ? "timeout" : "closed");
        return 2;
      case Call::send: {
        const auto value = bytes(lua, 1);
        if (value.empty() || value.size() > message_limit)
          throw std::invalid_argument("UI message size rejected");
        if (!connected || outbound.size() >= 64 || value.size() > queue_limit - outbound_bytes) {
          lua_pushnil(lua);
          lua_pushstring(lua, connected ? "timeout" : "closed");
          return 2;
        }
        outbound.emplace_back(value.begin(), value.end());
        outbound_bytes += value.size();
        lua_pushboolean(lua, true);
        return 1;
      }
      case Call::reset:
        connected = false;
        pts.reset();
        inbound.clear();
        outbound.clear();
        inbound_bytes = outbound_bytes = 0;
        ++resets;
        return 0;
      case Call::log_get:
        lua_pushinteger(lua, log_level);
        return 1;
      case Call::log_set:
        log_level = static_cast<int>(luaL_checkinteger(lua, 1));
        return 0;
      case Call::log: {
        const auto level = luaL_checkinteger(lua, 1);
        std::size_t size = 0;
        const char* message = luaL_checklstring(lua, 3, &size);
        if (level >= log_level && logs.size() < 128)
          logs.emplace_back(message, std::min<std::size_t>(size, 4096));
        return 0;
      }
      case Call::task_create: {
        cleanup();
        if (tasks.size() >= 512) throw std::invalid_argument("too many Lua tasks");
        auto* handle =
            static_cast<TaskHandle*>(lua_newuserdatadtor(lua, sizeof(TaskHandle), task_destroy));
        handle->owner = this;
        handle->id = next_task++;
        luaL_getmetatable(lua, task_type);
        lua_setmetatable(lua, -2);
        tasks.emplace(handle->id, Task{});
        return 1;
      }
      case Call::task_run: {
        const auto id = task_id(lua);
        luaL_checktype(lua, 2, LUA_TFUNCTION);
        stop(id);
        auto found = tasks.find(id);
        if (found == tasks.end()) throw std::invalid_argument("unknown Lua task");
        auto* thread = lua_newthread(lua);
        lua_setthreaddata(thread, this);
        const int ref = lua_ref(lua, -1);
        lua_pop(lua, 1);
        lua_pushvalue(lua, 2);
        lua_xmove(lua, thread, 1);
        found->second = {thread, ref, true};
        return 0;
      }
      case Call::task_stop:
        stop(task_id(lua));
        return 0;
      case Call::task_busy: {
        const auto found = tasks.find(task_id(lua));
        lua_pushboolean(lua, found != tasks.end() && found->second.active);
        return 1;
      }
    }
    return 0;
  }
  void error_from(lua_State* lua) {
    const char* detail = lua_type(lua, -1) == LUA_TSTRING ? lua_tostring(lua, -1) : nullptr;
    std::snprintf(error, sizeof(error), "%s", detail ? detail : "Lua execution failed");
    failed = true;
    throw std::runtime_error(error);
  }
};
LuaHost::LuaHost(ResourceReader reader, std::size_t limit, std::vector<OutputStream> outputs)
    : impl_(std::make_unique<Impl>(std::move(reader), limit, std::move(outputs))) {}
LuaHost::~LuaHost() = default;
void LuaHost::execute(std::string_view source, std::string_view name) {
  auto& self = *impl_;
  if (self.failed || source.size() > 1024 * 1024 || name.size() > 128)
    throw std::invalid_argument("Lua host failed or source exceeds limit");
  self.chunk_name.assign(name);
  if (self.load(self.state, source, self.chunk_name.c_str()) != LUA_OK) self.error_from(self.state);
  self.budget(5 * 1000 * 1000);
  if (lua_pcall(self.state, 0, 0, 0) != LUA_OK) self.error_from(self.state);
}
void LuaHost::tick(double now) {
  auto& self = *impl_;
  if (self.failed || !std::isfinite(now) || now < self.now)
    throw std::invalid_argument("Lua host failed or clock moved backwards");
  self.now = now;
  self.cleanup();
  std::vector<std::uint64_t> scheduled;
  for (const auto& [id, task] : self.tasks)
    if (task.active) scheduled.push_back(id);
  for (auto id : scheduled) {
    auto found = self.tasks.find(id);
    if (found == self.tasks.end() || !found->second.active) continue;
    auto* thread = found->second.thread;
    lua_rawgeti(self.state, LUA_REGISTRYINDEX, found->second.ref);  // Pin during self-stop/GC.
    self.budget(200000);
    const auto status = lua_resume(thread, self.state, 0);
    if (status != LUA_OK && status != LUA_YIELD) self.error_from(thread);
    lua_settop(thread, 0);
    const auto current = self.tasks.find(id);
    if (status == LUA_OK && current != self.tasks.end() && current->second.thread == thread)
      self.stop(id);
    lua_pop(self.state, 1);
  }
}
void LuaHost::connected(bool value) {
  auto& self = *impl_;
  self.connected = value;
  if (!value) {
    self.pts.reset();
    self.inbound.clear();
    self.outbound.clear();
    self.inbound_bytes = self.outbound_bytes = 0;
  }
}
bool LuaHost::receive(std::span<const std::uint8_t> value) {
  auto& self = *impl_;
  if (self.failed || !self.connected || value.empty() || value.size() > message_limit ||
      self.inbound.size() >= 64 || value.size() > queue_limit - self.inbound_bytes)
    return false;
  self.inbound.emplace_back(value.begin(), value.end());
  self.inbound_bytes += value.size();
  return true;
}
std::vector<LuaHost::Bytes> LuaHost::take_outbound() {
  auto result = std::move(impl_->outbound);
  impl_->outbound.clear();
  impl_->outbound_bytes = 0;
  return result;
}
std::size_t LuaHost::active_tasks() const {
  return static_cast<std::size_t>(
      std::count_if(impl_->tasks.begin(), impl_->tasks.end(),
                    [](const auto& item) { return item.second.active; }));
}
std::size_t LuaHost::memory_used() const {
  return impl_->used;
}
std::size_t LuaHost::reset_count() const {
  return impl_->resets;
}
std::vector<std::string> LuaHost::take_logs() {
  auto result = std::move(impl_->logs);
  impl_->logs.clear();
  return result;
}
std::shared_ptr<theme::graphics::Layer> LuaHost::scene_root() const {
  return impl_->graphics->root();
}
std::shared_ptr<theme::video::Stream> LuaHost::add_video_input(theme::video::Input input) {
  if (impl_->failed) throw std::runtime_error("Lua host failed");
  return impl_->graphics->add_video_input(std::move(input));
}
void LuaHost::bind_custom_image_provider(
    std::function<std::optional<Bytes>(std::string_view, int)> provider) {
  impl_->graphics->bind_custom_image_provider(std::move(provider));
}
theme::graphics::Image LuaHost::render_reference() {
  auto& self = *impl_;
  if (self.failed || self.outputs.empty()) throw std::runtime_error("no usable theme output");
  try {
    return theme::graphics::render(self.graphics->root(), self.outputs[0].width,
                                   self.outputs[0].height);
  } catch (...) {
    self.failed = true;
    connected(false);
    throw;
  }
}
void LuaHost::presented(double pts) {
  if (!std::isfinite(pts) || (impl_->pts && pts < *impl_->pts))
    throw std::invalid_argument("invalid theme presentation timestamp");
  impl_->pts = pts;
}
bool LuaHost::faulted() const {
  return impl_->failed;
}
}  // namespace headunit
