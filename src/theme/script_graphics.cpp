// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "script_graphics.hpp"
#include <lua.h>
#include <lualib.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace headunit::theme::script {
namespace {
namespace g = graphics;
constexpr const char *point_type = "Headunit.Theme.DPPoint", *size_type = "Headunit.Theme.DPSize",
                     *rect_type = "Headunit.Theme.DPRect";
constexpr const char *color_type = "Headunit.Theme.DPColor",
                     *transform_type = "Headunit.Theme.DPAffineTransform",
                     *stop_type = "Headunit.Theme.DPGradientStop";
constexpr const char *layer_type = "Headunit.Theme.Layer", *path_type = "Headunit.Theme.Path",
                     *image_type = "Headunit.Theme.Image";
constexpr const char* filter_type = "Headunit.Theme.BlurFilter";
[[noreturn]] void fail(const char* reason) {
  throw std::invalid_argument(reason);
}
double number(lua_State* L, int index, double minimum = -1e7, double maximum = 1e7) {
  const auto n = luaL_checknumber(L, index);
  if (!std::isfinite(n) || n < minimum || n > maximum) fail("drawing number outside allowed range");
  return n;
}
bool boolean(lua_State* L, int index) {
  luaL_checktype(L, index, LUA_TBOOLEAN);
  return lua_toboolean(L, index);
}
std::string_view string(lua_State* L, int index) {
  luaL_checktype(L, index, LUA_TSTRING);
  std::size_t n = 0;
  const auto* s = lua_tolstring(L, index, &n);
  if (n > 1024 || std::memchr(s, 0, n)) fail("invalid drawing string");
  return {s, n};
}
template <class T>
T& value(lua_State* L, int index, const char* type) {
  return *static_cast<T*>(luaL_checkudata(L, index, type));
}
template <class T>
void push_value(lua_State* L, const char* type, T data) {
  new (lua_newuserdata(L, sizeof(T))) T(data);
  luaL_getmetatable(L, type);
  lua_setmetatable(L, -2);
}
template <class T>
struct Handle {
  std::shared_ptr<T> ptr;
};
template <class T>
std::shared_ptr<T>& handle(lua_State* L, int index, const char* type) {
  return value<Handle<T>>(L, index, type).ptr;
}
template <class T>
void push_handle(lua_State* L, const char* type, std::shared_ptr<T> ptr) {
  new (lua_newuserdatadtor(L, sizeof(Handle<T>), [](lua_State*, void* p) {
    static_cast<Handle<T>*>(p)->~Handle<T>();
  })) Handle<T>{std::move(ptr)};
  luaL_getmetatable(L, type);
  lua_setmetatable(L, -2);
}
// Script cap values are a wire ABI, not Cairo's enum order. The reference
// asset uses 2 for flush-cut wide tracks and 0 for rounded thin ornaments.
// Keep this conversion at the binding; native paths retain their own enum.
enum class ScriptCap { round = 0, square = 1, butt = 2 };
g::Cap native_cap(ScriptCap cap) {
  switch (cap) {
    case ScriptCap::round:
      return g::Cap::round;
    case ScriptCap::square:
      return g::Cap::square;
    case ScriptCap::butt:
      return g::Cap::butt;
  }
  fail("invalid drawing line cap");
}
ScriptCap script_cap(g::Cap cap) {
  switch (cap) {
    case g::Cap::round:
      return ScriptCap::round;
    case g::Cap::square:
      return ScriptCap::square;
    case g::Cap::butt:
      return ScriptCap::butt;
  }
  fail("invalid native line cap");
}
template <class T>
T enumeration(lua_State* L, int index, int max) {
  const auto n = number(L, index, 0, max);
  if (std::floor(n) != n) fail("invalid drawing enumeration");
  return static_cast<T>(static_cast<int>(n));
}
}  // namespace
struct Graphics::Impl {
  struct Budget {
    std::size_t objects = 0, image_bytes = 0, path_segments = 0;
  };
  std::string display;
  std::function<std::string(std::string_view)> read;
  std::function<std::optional<Bytes>(std::string_view, int)> custom_images;
  std::shared_ptr<Budget> budget = std::make_shared<Budget>();
  std::shared_ptr<g::Layer> root;
  std::shared_ptr<video::Memory> video_memory = std::make_shared<video::Memory>();
  std::map<std::string, std::shared_ptr<video::Stream>> inputs;
  static Impl& self(lua_State* L) {
    return *static_cast<Impl*>(lua_touserdata(L, lua_upvalueindex(1)));
  }
  template <class F>
  static int boundary(lua_State* L, F fn) {
    try {
      return fn(self(L));
    }
    // Let Luau's own protected-call exception propagate unchanged. Catch only
    // errors owned by this native binding, so script type errors retain their
    // original message and VM allocation failures keep their failure semantics.
    catch (const std::runtime_error& e) {
      luaL_errorL(L, "%s", e.what());
    } catch (const std::logic_error& e) {
      luaL_errorL(L, "%s", e.what());
    } catch (const std::bad_alloc&) {
      luaL_errorL(L, "native drawing allocation failed");
    }
  }
  template <class T, class... A>
  std::shared_ptr<T> create(A&&... args) {
    if (budget->objects >= 4096) fail("native graphics object limit exceeded");
    auto* object = new T(std::forward<A>(args)...);
    ++budget->objects;
    return std::shared_ptr<T>(object, [b = budget](T* p) {
      if constexpr (std::is_same_v<T, g::Path>) b->path_segments -= p->segments.size();
      delete p;
      --b->objects;
    });
  }
  void function(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, fn, name, 1);
    lua_setfield(L, -2, name);
  }
  template <class T>
  void enums(lua_State* L, const char* name,
             std::initializer_list<std::pair<const char*, T>> entries) {
    lua_newtable(L);
    for (const auto& [key, item] : entries) {
      lua_pushinteger(L, static_cast<int>(item));
      lua_setfield(L, -2, key);
    }
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, name);
  }
  void metatable(lua_State* L, const char* type, lua_CFunction index, lua_CFunction set = nullptr) {
    luaL_newmetatable(L, type);
    function(L, "__index", index);
    if (set) function(L, "__newindex", set);
    lua_pushstring(L, type);
    lua_setfield(L, -2, "__metatable");
  }
  static int point_create(lua_State* L) {
    return boundary(L, [&](Impl&) {
      push_value(L, point_type, g::Point{number(L, 2), number(L, 3)});
      return 1;
    });
  }
  static int size_create(lua_State* L) {
    return boundary(L, [&](Impl&) {
      push_value(L, size_type, g::Size{number(L, 2, 0), number(L, 3, 0)});
      return 1;
    });
  }
  static int rect_create(lua_State* L) {
    return boundary(L, [&](Impl&) {
      push_value(L, rect_type,
                 g::Rect{{number(L, 2), number(L, 3)}, {number(L, 4, 0), number(L, 5, 0)}});
      return 1;
    });
  }
  static int color_create(lua_State* L) {
    return boundary(L, [&](Impl&) {
      push_value(
          L, color_type,
          g::Color{number(L, 2, 0, 1), number(L, 3, 0, 1), number(L, 4, 0, 1), number(L, 5, 0, 1)});
      return 1;
    });
  }
  static int transform_create(lua_State* L) {
    return boundary(L, [&](Impl&) {
      push_value(L, transform_type,
                 g::Transform{number(L, 2), number(L, 3), number(L, 4), number(L, 5), number(L, 6),
                              number(L, 7)});
      return 1;
    });
  }
  static int stop_create(lua_State* L) {
    return boundary(L, [&](Impl&) {
      push_value(L, stop_type, g::Stop{value<g::Color>(L, 2, color_type), number(L, 3, 0, 1)});
      return 1;
    });
  }
  static int point_index(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto& v = value<g::Point>(L, 1, point_type);
      const auto key = string(L, 2);
      if (key == "x")
        lua_pushnumber(L, v.x);
      else if (key == "y")
        lua_pushnumber(L, v.y);
      else
        lua_pushnil(L);
      return 1;
    });
  }
  static int size_index(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto& v = value<g::Size>(L, 1, size_type);
      const auto key = string(L, 2);
      if (key == "width")
        lua_pushnumber(L, v.width);
      else if (key == "height")
        lua_pushnumber(L, v.height);
      else
        lua_pushnil(L);
      return 1;
    });
  }
  static int rect_index(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto& v = value<g::Rect>(L, 1, rect_type);
      const auto key = string(L, 2);
      if (key == "origin")
        push_value(L, point_type, v.origin);
      else if (key == "size")
        push_value(L, size_type, v.size);
      else {
        luaL_getmetatable(L, rect_type);
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
      }
      return 1;
    });
  }
  static int color_index(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto& v = value<g::Color>(L, 1, color_type);
      const auto key = string(L, 2);
      if (key == "r")
        lua_pushnumber(L, v.r);
      else if (key == "g")
        lua_pushnumber(L, v.g);
      else if (key == "b")
        lua_pushnumber(L, v.b);
      else if (key == "a")
        lua_pushnumber(L, v.a);
      else
        lua_pushnil(L);
      return 1;
    });
  }
  static int transform_index(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto& v = value<g::Transform>(L, 1, transform_type);
      const auto key = string(L, 2);
      if (key == "a")
        lua_pushnumber(L, v.a);
      else if (key == "b")
        lua_pushnumber(L, v.b);
      else if (key == "c")
        lua_pushnumber(L, v.c);
      else if (key == "d")
        lua_pushnumber(L, v.d);
      else if (key == "tx")
        lua_pushnumber(L, v.tx);
      else if (key == "ty")
        lua_pushnumber(L, v.ty);
      else {
        luaL_getmetatable(L, transform_type);
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
      }
      return 1;
    });
  }
  static int stop_index(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto& v = value<g::Stop>(L, 1, stop_type);
      const auto key = string(L, 2);
      if (key == "color")
        push_value(L, color_type, v.color);
      else if (key == "location")
        lua_pushnumber(L, v.location);
      else
        lua_pushnil(L);
      return 1;
    });
  }
  static int rect_transform(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto r = value<g::Rect>(L, 1, rect_type);
      const auto t = value<g::Transform>(L, 2, transform_type);
      std::array points{t.apply(r.origin), t.apply({r.origin.x + r.size.width, r.origin.y}),
                        t.apply({r.origin.x, r.origin.y + r.size.height}),
                        t.apply({r.origin.x + r.size.width, r.origin.y + r.size.height})};
      auto lo = points[0], hi = lo;
      for (const auto p : points) {
        lo.x = std::min(lo.x, p.x);
        lo.y = std::min(lo.y, p.y);
        hi.x = std::max(hi.x, p.x);
        hi.y = std::max(hi.y, p.y);
      }
      push_value(L, rect_type, g::Rect{lo, {hi.x - lo.x, hi.y - lo.y}});
      return 1;
    });
  }
  static int rect_size(lua_State* L) {
    return boundary(L, [&](Impl&) {
      push_value(L, rect_type, g::Rect{{}, value<g::Size>(L, 1, size_type)});
      return 1;
    });
  }
  static int translate(lua_State* L) {
    return boundary(L, [&](Impl&) {
      push_value(L, transform_type, g::Transform{1, 0, 0, 1, number(L, 1), number(L, 2)});
      return 1;
    });
  }
  static int scale(lua_State* L) {
    return boundary(L, [&](Impl&) {
      push_value(L, transform_type, g::Transform{number(L, 1), 0, 0, number(L, 2), 0, 0});
      return 1;
    });
  }
  static int rotate(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto angle = number(L, 1);
      const auto c = std::cos(angle), s = std::sin(angle);
      push_value(L, transform_type, g::Transform{c, s, -s, c, 0, 0});
      return 1;
    });
  }
  static int transform_add(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto t = value<g::Transform>(L, 1, transform_type);
      push_value(L, transform_type, t.multiply({1, 0, 0, 1, number(L, 2), number(L, 3)}));
      return 1;
    });
  }
  static int transform_multiply(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto a = value<g::Transform>(L, 1, transform_type),
                 b = value<g::Transform>(L, 2, transform_type);
      push_value(L, transform_type, b.multiply(a));
      return 1;  // CoreGraphics concatenation order.
    });
  }
  static int path_create(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      push_handle(L, path_type, h.create<g::Path>());
      return 1;
    });
  }
  static int path_index(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto& p = handle<g::Path>(L, 1, path_type);
      const auto key = string(L, 2);
      if (key == "numPoints")
        lua_pushnumber(L, p->point_count);
      else {
        luaL_getmetatable(L, path_type);
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
      }
      return 1;
    });
  }
  template <g::SegmentType Type>
  static int path_segment(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      auto& path = handle<g::Path>(L, 1, path_type);
      g::Segment s{Type};
      if constexpr (Type == g::SegmentType::ellipse) {
        const auto r = value<g::Rect>(L, 2, rect_type);
        s.p = r.origin;
        s.c1 = {r.size.width, r.size.height};
      } else if constexpr (Type != g::SegmentType::close) {
        s.p = value<g::Point>(L, 2, point_type);
        if constexpr (Type == g::SegmentType::quad || Type == g::SegmentType::cubic)
          s.c1 = value<g::Point>(L, 3, point_type);
        if constexpr (Type == g::SegmentType::cubic) s.c2 = value<g::Point>(L, 4, point_type);
        if constexpr (Type == g::SegmentType::arc) {
          s.radius = number(L, 3, 0);
          // DP arc angles use an upward-positive angular axis, while our scene
          // and Cairo/Skia points use downward-positive y. Convert at the ABI
          // boundary; ordinary native scene paths keep their existing convention.
          s.start = -number(L, 4);
          s.end = -number(L, 5);
          s.clockwise = boolean(L, 6);
        }
      }
      if (h.budget->path_segments >= 262144) fail("native path budget exceeded");
      path->append(s);
      ++h.budget->path_segments;
      return 0;
    });
  }
  template <g::Kind Kind>
  static int layer_create(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      push_handle(L, layer_type, h.create<g::Layer>(Kind));
      return 1;
    });
  }
  static int filter_create(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      push_handle(L, filter_type, h.create<g::BlurFilter>());
      return 1;
    });
  }
  static int filter_index(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto& filter = *handle<g::BlurFilter>(L, 1, filter_type);
      const auto key = string(L, 2);
      if (key == "radius")
        lua_pushnumber(L, filter.radius);
      else if (key == "enabled")
        lua_pushboolean(L, filter.enabled);
      else if (key == "hardEdges")
        lua_pushboolean(L, filter.hard_edges);
      else if (key == "normalizedEdges")
        lua_pushboolean(L, filter.normalized_edges);
      else {
        luaL_getmetatable(L, filter_type);
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
      }
      return 1;
    });
  }
  static int filter_set(lua_State* L) {
    return boundary(L, [&](Impl&) {
      auto& filter = *handle<g::BlurFilter>(L, 1, filter_type);
      const auto key = string(L, 2);
      if (key == "radius")
        filter.radius = number(L, 3, 0, 128);
      else if (key == "enabled")
        filter.enabled = boolean(L, 3);
      else if (key == "hardEdges")
        filter.hard_edges = boolean(L, 3);
      else if (key == "normalizedEdges")
        filter.normalized_edges = boolean(L, 3);
      else
        fail("unknown native filter property");
      return 0;
    });
  }
  static int filter_detach(lua_State* L) {
    return boundary(L, [&](Impl&) {
      handle<g::BlurFilter>(L, 1, filter_type)->detach();
      return 0;
    });
  }
  static int filter_class(lua_State* L) {
    (void)handle<g::BlurFilter>(L, 1, filter_type);
    lua_pushliteral(L, "DPBlurFilter");
    return 1;
  }
  static int layer_filter(lua_State* L) {
    return boundary(L, [&](Impl&) {
      handle<g::Layer>(L, 1, layer_type)->add_filter(handle<g::BlurFilter>(L, 2, filter_type));
      return 0;
    });
  }
  static int video_create(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto found = h.inputs.find(std::string(string(L, 1)));
      if (found == h.inputs.end()) fail("unknown negotiated video input");
      auto layer = h.create<g::Layer>(g::Kind::video);
      layer->video = h.create<video::View>();
      layer->video->stream = found->second;
      push_handle(L, layer_type, std::move(layer));
      return 1;
    });
  }
  static video::View& video_view(lua_State* L) {
    const auto& layer = handle<g::Layer>(L, 1, layer_type);
    if (layer->kind != g::Kind::video || !layer->video) fail("operation requires a video layer");
    return *layer->video;
  }
  template <bool Subscribe>
  static int video_subscribe(lua_State* L) {
    return boundary(L, [&](Impl&) {
      auto& view = video_view(L);
      if constexpr (Subscribe)
        if (view.stream->closed()) fail("video input is closed");
      view.subscribed = Subscribe;
      return 0;
    });
  }
  template <bool Next>
  static int video_pts(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto& view = video_view(L);
      const auto pts = view.active() ? (Next ? view.stream->next_pts() : view.stream->current_pts())
                                     : std::nullopt;
      if (pts)
        lua_pushnumber(L, *pts);
      else
        lua_pushnil(L);
      if (view.active())
        lua_pushnil(L);
      else
        lua_pushstring(L, view.stream->closed() ? "closed" : "unsubscribed");
      return 2;
    });
  }
  static int video_flush(lua_State* L) {
    return boundary(L, [&](Impl&) {
      auto& view = video_view(L);
      if (view.stream->closed()) fail("video input is closed");
      view.stream->flush();
      return 0;
    });
  }
  static int video_unsupported(lua_State* L) {
    return boundary(L, [&](Impl&) -> int {
      (void)video_view(L);
      fail("video deferral ABI is not implemented");
    });
  }
  static int layer_class(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto& p = handle<g::Layer>(L, 1, layer_type);
      const std::array names{"DPLayer",      "DPShapeLayer", "DPGradientLayer",
                             "DPImageLayer", "DPVideoLayer", "DPBackdropLayer"};
      lua_pushstring(L, names[static_cast<unsigned>(p->kind)]);
      return 1;
    });
  }
  static int layer_equal(lua_State* L) {
    lua_pushboolean(L, handle<g::Layer>(L, 1, layer_type) == handle<g::Layer>(L, 2, layer_type));
    return 1;
  }
  static int layer_children(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto& p = handle<g::Layer>(L, 1, layer_type);
      lua_createtable(L, p->children.size(), 0);
      int i = 0;
      for (const auto& child : p->children) {
        push_handle(L, layer_type, child);
        lua_rawseti(L, -2, ++i);
      }
      return 1;
    });
  }
  static int layer_add(lua_State* L) {
    return boundary(L, [&](Impl&) {
      auto p = handle<g::Layer>(L, 1, layer_type), c = handle<g::Layer>(L, 2, layer_type);
      p->insert(c, p->children.size());
      return 0;
    });
  }
  static int layer_insert(lua_State* L) {
    return boundary(L, [&](Impl&) {
      auto p = handle<g::Layer>(L, 1, layer_type), c = handle<g::Layer>(L, 2, layer_type);
      const auto i = number(L, 3, 1, p->children.size() + 1);
      if (std::floor(i) != i) fail("invalid sublayer index");
      p->insert(c, static_cast<std::size_t>(i) - 1);
      return 0;
    });
  }
  static int layer_below(lua_State* L) {
    return boundary(L, [&](Impl&) {
      auto p = handle<g::Layer>(L, 1, layer_type), c = handle<g::Layer>(L, 2, layer_type),
           relative = handle<g::Layer>(L, 3, layer_type);
      auto i = std::find(p->children.begin(), p->children.end(), relative);
      if (i == p->children.end()) fail("relative layer is not a child");
      if (c != relative) p->insert(c, i - p->children.begin());
      return 0;
    });
  }
  static int layer_replace(lua_State* L) {
    return boundary(L, [&](Impl&) {
      auto p = handle<g::Layer>(L, 1, layer_type), old = handle<g::Layer>(L, 2, layer_type),
           replacement = handle<g::Layer>(L, 3, layer_type);
      auto i = std::find(p->children.begin(), p->children.end(), old);
      if (i == p->children.end()) fail("replaced layer is not a child");
      if (old != replacement) {
        p->insert(replacement, i - p->children.begin());
        p->remove(old);
      }
      return 0;
    });
  }
  static int layer_mask(lua_State* L) {
    return boundary(L, [&](Impl&) {
      handle<g::Layer>(L, 1, layer_type)
          ->set_mask(lua_isnil(L, 2) ? nullptr : handle<g::Layer>(L, 2, layer_type));
      return 0;
    });
  }
  static int layer_transform(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto result =
          handle<g::Layer>(L, 1, layer_type)->transform_to(handle<g::Layer>(L, 2, layer_type));
      if (result)
        push_value(L, transform_type, *result);
      else
        lua_pushnil(L);
      return 1;
    });
  }
  static int layer_detach(lua_State* L) {
    return boundary(L, [&](Impl&) {
      handle<g::Layer>(L, 1, layer_type)->detach();
      return 0;
    });
  }
  static int layer_remove(lua_State* L) {
    return boundary(L, [&](Impl&) {
      handle<g::Layer>(L, 1, layer_type)->remove(handle<g::Layer>(L, 2, layer_type));
      return 0;
    });
  }
  static int layer_clear(lua_State* L) {
    return boundary(L, [&](Impl&) {
      handle<g::Layer>(L, 1, layer_type)->clear();
      return 0;
    });
  }
  static int set_root(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      if (string(L, 1) != h.display) fail("root layer belongs to a different display");
      h.root = lua_isnil(L, 2) ? nullptr : handle<g::Layer>(L, 2, layer_type);
      if (h.root) h.root->detach();
      return 0;
    });
  }
  static int layer_index(lua_State* L) {
    return boundary(L, [&](Impl&) {
      const auto& p = *handle<g::Layer>(L, 1, layer_type);
      const auto key = string(L, 2);
      if (key == "position")
        push_value(L, point_type, p.position);
      else if (key == "anchorPoint")
        push_value(L, point_type, p.anchor);
      else if (key == "bounds")
        push_value(L, rect_type, p.bounds);
      else if (key == "frame")
        push_value(L, rect_type, p.frame());
      else if (key == "transform")
        push_value(L, transform_type, p.transform);
      else if (key == "backgroundColor")
        push_value(L, color_type, p.background);
      else if (key == "borderColor")
        push_value(L, color_type, p.border);
      else if (key == "fillColor")
        push_value(L, color_type, p.fill);
      else if (key == "strokeColor")
        push_value(L, color_type, p.stroke);
      else if (key == "tint")
        push_value(L, color_type, p.tint);
      else if (key == "opacity")
        lua_pushnumber(L, p.opacity);
      else if (key == "borderWidth")
        lua_pushnumber(L, p.border_width);
      else if (key == "cornerRadius")
        lua_pushnumber(L, p.corner_radius);
      else if (key == "lineWidth")
        lua_pushnumber(L, p.line_width);
      else if (key == "miterLimit")
        lua_pushnumber(L, p.miter_limit);
      else if (key == "strokeStart")
        lua_pushnumber(L, p.stroke_start);
      else if (key == "strokeEnd")
        lua_pushnumber(L, p.stroke_end);
      else if (key == "hidden")
        lua_pushboolean(L, p.hidden);
      else if (key == "maskToBounds")
        lua_pushboolean(L, p.clip);
      else if (key == "groupBlending")
        lua_pushboolean(L, p.group_blending);
      else if (key == "shouldRasterize")
        lua_pushboolean(L, p.rasterize);
      else if (key == "antialiased")
        lua_pushboolean(L, p.antialiased);
      else if (key == "cornerCurve")
        lua_pushinteger(L, p.continuous_corners ? 1 : 0);
      else if (key == "blendMode")
        lua_pushinteger(L, static_cast<int>(p.blend));
      else if (key == "fillRule")
        lua_pushinteger(L, static_cast<int>(p.fill_rule));
      else if (key == "lineCap")
        lua_pushinteger(L, static_cast<int>(script_cap(p.cap)));
      else if (key == "lineJoin")
        lua_pushinteger(L, static_cast<int>(p.join));
      else if (key == "type")
        lua_pushinteger(L, static_cast<int>(p.gradient_kind));
      else if (key == "startPoint")
        push_value(L, point_type, p.gradient_start);
      else if (key == "endPoint")
        push_value(L, point_type, p.gradient_end);
      else if (key == "numStops")
        lua_pushnumber(L, p.stops.size());
      else if (key == "name")
        lua_pushlstring(L, p.name.data(), p.name.size());
      else if (key == "path") {
        if (p.path)
          push_handle(L, path_type, p.path);
        else
          lua_pushnil(L);
      } else if (key == "texture") {
        if (p.image)
          push_handle(L, image_type, p.image);
        else
          lua_pushnil(L);
      } else if (key == "active" && p.video)
        lua_pushboolean(L, p.video->active());
      else if (key == "streamState" && p.video)
        lua_pushinteger(L, static_cast<int>(p.video->state()));
      else {
        luaL_getmetatable(L, layer_type);
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
      }
      return 1;
    });
  }
  static int layer_set(lua_State* L) {
    return boundary(L, [&](Impl&) {
      auto& p = *handle<g::Layer>(L, 1, layer_type);
      const auto key = string(L, 2);
      if (key == "position")
        p.position = value<g::Point>(L, 3, point_type);
      else if (key == "anchorPoint")
        p.anchor = value<g::Point>(L, 3, point_type);
      else if (key == "bounds")
        p.bounds = value<g::Rect>(L, 3, rect_type);
      else if (key == "frame")
        p.set_frame(value<g::Rect>(L, 3, rect_type));
      else if (key == "transform")
        p.transform = value<g::Transform>(L, 3, transform_type);
      else if (key == "backgroundColor")
        p.background = value<g::Color>(L, 3, color_type);
      else if (key == "borderColor")
        p.border = value<g::Color>(L, 3, color_type);
      else if (key == "fillColor")
        p.fill = value<g::Color>(L, 3, color_type);
      else if (key == "strokeColor")
        p.stroke = value<g::Color>(L, 3, color_type);
      else if (key == "tint")
        p.tint = value<g::Color>(L, 3, color_type);
      else if (key == "opacity")
        p.opacity = number(L, 3, 0, 1);
      else if (key == "borderWidth")
        p.border_width = number(L, 3, 0, 8192);
      else if (key == "cornerRadius")
        p.corner_radius = number(L, 3, 0, 8192);
      else if (key == "lineWidth")
        p.line_width = number(L, 3, 0, 8192);
      else if (key == "miterLimit")
        p.miter_limit = number(L, 3, 0, 8192);
      else if (key == "strokeStart")
        p.stroke_start = number(L, 3);
      else if (key == "strokeEnd")
        p.stroke_end = number(L, 3);
      else if (key == "hidden")
        p.hidden = boolean(L, 3);
      else if (key == "maskToBounds")
        p.clip = boolean(L, 3);
      else if (key == "groupBlending")
        p.group_blending = boolean(L, 3);
      else if (key == "shouldRasterize")
        p.rasterize = boolean(L, 3);
      else if (key == "antialiased")
        p.antialiased = boolean(L, 3);
      else if (key == "cornerCurve")
        p.continuous_corners = enumeration<int>(L, 3, 1) != 0;
      else if (key == "blendMode")
        p.blend = enumeration<g::Blend>(L, 3, 18);
      else if (key == "fillRule")
        p.fill_rule = enumeration<g::FillRule>(L, 3, 1);
      else if (key == "lineCap")
        p.cap = native_cap(enumeration<ScriptCap>(L, 3, 2));
      else if (key == "lineJoin")
        p.join = enumeration<g::Join>(L, 3, 2);
      else if (key == "type")
        p.gradient_kind = enumeration<g::GradientKind>(L, 3, 2);
      else if (key == "startPoint")
        p.gradient_start = value<g::Point>(L, 3, point_type);
      else if (key == "endPoint")
        p.gradient_end = value<g::Point>(L, 3, point_type);
      else if (key == "name")
        p.name = string(L, 3);
      else if (key == "path")
        p.path = lua_isnil(L, 3) ? nullptr : handle<g::Path>(L, 3, path_type);
      else if (key == "texture")
        p.image = lua_isnil(L, 3) ? nullptr : handle<g::Image>(L, 3, image_type);
      else
        fail("unknown native layer property");
      return 0;
    });
  }
  static int stops(lua_State* L) {
    return boundary(L, [&](Impl&) {
      auto& p = *handle<g::Layer>(L, 1, layer_type);
      luaL_checktype(L, 2, LUA_TTABLE);
      const int n = lua_objlen(L, 2);
      if (n > 256) fail("too many gradient stops");
      std::vector<g::Stop> stops;
      stops.reserve(n);
      for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        stops.push_back(value<g::Stop>(L, -1, stop_type));
        lua_pop(L, 1);
      }
      std::stable_sort(stops.begin(), stops.end(),
                       [](const auto& a, const auto& b) { return a.location < b.location; });
      p.stops = std::move(stops);
      return 0;
    });
  }
  static int colors(lua_State* L) {
    return boundary(L, [&](Impl&) {
      auto& p = *handle<g::Layer>(L, 1, layer_type);
      luaL_checktype(L, 2, LUA_TTABLE);
      const int n = lua_objlen(L, 2);
      if (n > 256) fail("too many gradient colors");
      std::vector<g::Stop> stops;
      stops.reserve(n);
      for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        stops.push_back({value<g::Color>(L, -1, color_type), n > 1 ? double(i - 1) / (n - 1) : 0});
        lua_pop(L, 1);
      }
      p.stops = std::move(stops);
      return 0;
    });
  }
  static int image_load(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto bytes = h.read(string(L, 1));
      auto image =
          g::decode_png({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
      const auto size = image->pixels.size() * sizeof(std::uint32_t);
      if (size > 128 * 1024 * 1024 - h.budget->image_bytes || h.budget->objects >= 4096)
        fail("native image budget exceeded");
      auto* raw = new g::Image(std::move(*image));
      h.budget->image_bytes += size;
      ++h.budget->objects;
      push_handle(L, image_type, std::shared_ptr<g::Image>(raw, [b = h.budget, size](g::Image* p) {
                    delete p;
                    b->image_bytes -= size;
                    --b->objects;
                  }));
      return 1;
    });
  }
  static int custom_png(lua_State* L) {
    return boundary(L, [&](Impl& h) {
      const auto name = string(L, 1);
      const auto appearance = lua_isnoneornil(L, 2) ? 0 : luaL_checkinteger(L, 2);
      if (appearance < 0 || appearance > 255) fail("invalid custom image appearance");
      if (!h.custom_images) {
        lua_pushnil(L);
        return 1;
      }
      auto bytes = h.custom_images(name, appearance);
      if (!bytes) {
        lua_pushnil(L);
        return 1;
      }
      if (bytes->empty() || bytes->size() > 16 * 1024 * 1024) fail("invalid custom image size");
      auto image = g::decode_png(*bytes);
      const auto size = image->pixels.size() * sizeof(std::uint32_t);
      if (size > 128 * 1024 * 1024 - h.budget->image_bytes || h.budget->objects >= 4096)
        fail("native image budget exceeded");
      auto* raw = new g::Image(std::move(*image));
      h.budget->image_bytes += size;
      ++h.budget->objects;
      push_handle(L, image_type, std::shared_ptr<g::Image>(raw, [b = h.budget, size](g::Image* p) {
                    delete p;
                    b->image_bytes -= size;
                    --b->objects;
                  }));
      return 1;
    });
  }
  static int image_size(lua_State* L) {
    const auto& p = handle<g::Image>(L, 1, image_type);
    lua_pushnumber(L, p->width);
    lua_pushnumber(L, p->height);
    return 2;
  }
  static int image_data(lua_State* L) {
    const auto& p = handle<g::Image>(L, 1, image_type);
    lua_pushvalue(L, 1);  // Opaque owner of the actual pixel buffer; no raw address exposed.
    lua_pushnumber(L, p->pixels.size() * 4);
    return 2;
  }
  static int image_description(lua_State* L) {
    lua_pushliteral(L, "<native image data>");
    return 1;
  }
  static int image_texture(lua_State* L) {
    (void)handle<g::Image>(L, 1, image_type);
    lua_pushvalue(L, 1);
    return 1;
  }
  static int image_index(lua_State* L) {
    luaL_getmetatable(L, image_type);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    return 1;
  }
  void constructor(lua_State* L, const char* name, lua_CFunction fn) {
    lua_newtable(L);
    lua_newtable(L);
    function(L, "__call", fn);
    lua_setreadonly(L, -1, 1);
    lua_setmetatable(L, -2);
    if (std::string_view(name) == "DPRect") function(L, "withSize", rect_size);
    if (std::string_view(name) == "DPAffineTransform") {
      function(L, "translate", translate);
      function(L, "scale", scale);
      function(L, "rotate", rotate);
    }
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, name);
  }
  void install(lua_State* L, int cp) {
    cp = lua_absindex(L, cp);
    for (const auto& [type, index] :
         std::initializer_list<std::pair<const char*, lua_CFunction>>{{point_type, point_index},
                                                                      {size_type, size_index},
                                                                      {rect_type, rect_index},
                                                                      {color_type, color_index},
                                                                      {stop_type, stop_index}}) {
      metatable(L, type, index);
      if (std::string_view(type) == rect_type) function(L, "applyTransform", rect_transform);
      lua_setreadonly(L, -1, 1);
      lua_pop(L, 1);
    }
    metatable(L, transform_type, transform_index);
    function(L, "add", transform_add);
    function(L, "multiply", transform_multiply);
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);
    metatable(L, path_type, path_index);
    function(L, "moveTo", path_segment<g::SegmentType::move>);
    function(L, "lineTo", path_segment<g::SegmentType::line>);
    function(L, "quadTo", path_segment<g::SegmentType::quad>);
    function(L, "cubeTo", path_segment<g::SegmentType::cubic>);
    function(L, "arcTo", path_segment<g::SegmentType::arc>);
    function(L, "ellipseInRect", path_segment<g::SegmentType::ellipse>);
    function(L, "closeSubpath", path_segment<g::SegmentType::close>);
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);
    metatable(L, layer_type, layer_index, layer_set);
    function(L, "setMask", layer_mask);
    function(L, "getTransformTo", layer_transform);
    function(L, "getClassName", layer_class);
    function(L, "__eq", layer_equal);
    function(L, "getSublayers", layer_children);
    function(L, "addSublayer", layer_add);
    function(L, "insertSublayerAtIndex", layer_insert);
    function(L, "insertSublayerBelow", layer_below);
    function(L, "replaceSublayer", layer_replace);
    function(L, "removeFromSuperlayer", layer_detach);
    function(L, "removeSublayer", layer_remove);
    function(L, "removeSublayers", layer_clear);
    function(L, "setStops", stops);
    function(L, "setColors", colors);
    function(L, "addFilter", layer_filter);
    function(L, "subscribe", video_subscribe<true>);
    function(L, "unsubscribe", video_subscribe<false>);
    function(L, "getCurrentPTS", video_pts<false>);
    function(L, "getNextPTS", video_pts<true>);
    function(L, "deferNextPTS", video_unsupported);
    function(L, "flush", video_flush);
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);
    metatable(L, filter_type, filter_index, filter_set);
    function(L, "removeFromLayer", filter_detach);
    function(L, "getClassName", filter_class);
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);
    metatable(L, image_type, image_index);
    function(L, "getSize", image_size);
    function(L, "getData", image_data);
    function(L, "__tostring", image_description);
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);
    lua_pushvalue(L, cp);
    constructor(L, "DPPoint", point_create);
    constructor(L, "DPSize", size_create);
    constructor(L, "DPRect", rect_create);
    constructor(L, "DPColor", color_create);
    constructor(L, "DPAffineTransform", transform_create);
    constructor(L, "DPGradientStop", stop_create);
    function(L, "setRootLayer", set_root);
    function(L, "createAssetImage", image_load);
    function(L, "createCustomPNG", custom_png);
    lua_newtable(L);
    function(L, "create", path_create);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "DPPath");
    lua_newtable(L);
    function(L, "createFromImage", image_texture);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "DPTexture");
    lua_newtable(L);
    function(L, "create", layer_create<g::Kind::layer>);
    enums<g::Blend>(L, "BLEND",
                    {{"COPY", g::Blend::copy},
                     {"CLEAR", g::Blend::clear},
                     {"SOURCEOVER", g::Blend::source_over},
                     {"SOURCEIN", g::Blend::source_in},
                     {"SOURCEOUT", g::Blend::source_out},
                     {"SOURCEATOP", g::Blend::source_atop},
                     {"DESTOVER", g::Blend::dest_over},
                     {"DESTIN", g::Blend::dest_in},
                     {"DESTOUT", g::Blend::dest_out},
                     {"DESTATOP", g::Blend::dest_atop},
                     {"ADDITIVE", g::Blend::add},
                     {"PLUSLIGHTER", g::Blend::add},
                     {"EXCLUSIVEOR", g::Blend::xor_},
                     {"XOR", g::Blend::xor_},
                     {"SCREEN", g::Blend::screen},
                     {"EXCLUSION", g::Blend::exclusion},
                     {"MAXIMUM", g::Blend::maximum},
                     {"MINIMUM", g::Blend::minimum},
                     {"SUBTRACTSRC", g::Blend::subtract_source},
                     {"SUBTRACTDEST", g::Blend::subtract_dest},
                     {"COMPONENTMASK", g::Blend::component_mask},
                     {"NORMAL", g::Blend::source_over}});
    enums<int>(L, "CURVE", {{"CIRCULAR", 0}, {"CONTINUOUS", 1}});
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "DPLayer");
    lua_newtable(L);
    function(L, "create", layer_create<g::Kind::shape>);
    enums<g::FillRule>(L, "FILLRULE",
                       {{"NONZERO", g::FillRule::nonzero}, {"EVENODD", g::FillRule::evenodd}});
    enums<ScriptCap>(
        L, "LINECAP",
        {{"BUTT", ScriptCap::butt}, {"ROUND", ScriptCap::round}, {"SQUARE", ScriptCap::square}});
    enums<g::Join>(
        L, "LINEJOIN",
        {{"MITER", g::Join::miter}, {"ROUND", g::Join::round}, {"BEVEL", g::Join::bevel}});
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "DPShapeLayer");
    lua_newtable(L);
    function(L, "create", layer_create<g::Kind::gradient>);
    enums<g::GradientKind>(L, "KIND",
                           {{"LINEAR", g::GradientKind::linear},
                            {"RADIAL", g::GradientKind::radial},
                            {"CONICAL", g::GradientKind::conical}});
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "DPGradientLayer");
    lua_newtable(L);
    function(L, "create", layer_create<g::Kind::image>);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "DPImageLayer");
    lua_newtable(L);
    function(L, "create", layer_create<g::Kind::backdrop>);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "DPBackdropLayer");
    lua_newtable(L);
    function(L, "create", filter_create);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "DPBlurFilter");
    lua_newtable(L);
    function(L, "create", video_create);
    enums<video::State>(L, "STREAMSTATE",
                        {{"CLOSED", video::State::closed},
                         {"SUBSCRIBED", video::State::subscribed},
                         {"DATAAVAILABLE", video::State::data_available},
                         {"UNSUBSCRIBED", video::State::unsubscribed},
                         {"INVALID", video::State::invalid}});
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "DPVideoLayer");
    lua_pop(L, 1);
  }
};
Graphics::Graphics(std::string display, std::function<std::string(std::string_view)> read)
    : impl_(std::make_unique<Impl>()) {
  impl_->display = std::move(display);
  impl_->read = std::move(read);
}
Graphics::~Graphics() = default;
void Graphics::install(lua_State* L, int cp) {
  impl_->install(L, cp);
}
std::shared_ptr<g::Layer> Graphics::root() const {
  return impl_->root;
}
std::shared_ptr<video::Stream> Graphics::add_video_input(video::Input input) {
  auto& h = *impl_;
  if (h.inputs.size() >= 8 || h.inputs.contains(input.identifier))
    fail("duplicate or excessive video inputs");
  auto stream = std::make_shared<video::Stream>(std::move(input), h.video_memory);
  h.inputs.emplace(stream->input().identifier, stream);
  return stream;
}
int Graphics::enumerate_inputs(lua_State* L) {
  lua_createtable(L, impl_->inputs.size(), 0);
  int index = 0;
  for (const auto& [id, stream] : impl_->inputs) {
    const auto& input = stream->input();
    lua_newtable(L);
    lua_pushlstring(L, id.data(), id.size());
    lua_setfield(L, -2, "identifier");
    lua_pushnumber(L, input.width);
    lua_setfield(L, -2, "width");
    lua_pushnumber(L, input.height);
    lua_setfield(L, -2, "height");
    lua_pushnumber(L, input.frame_duration);
    lua_setfield(L, -2, "frameDuration");
    // This implementation exposes buffered incoming local video. The source
    // geometry is fixed; layer transforms remain available for composition.
    for (const auto& [key, value] :
         std::initializer_list<std::pair<const char*, int>>{{"streamMode", 0},
                                                            {"parameterMode", 1},
                                                            {"type", int(input.type)},
                                                            {"opacityCapability", 0},
                                                            {"positionCapability", 0},
                                                            {"sizeCapability", 0}}) {
      lua_pushinteger(L, value);
      lua_setfield(L, -2, key);
    }
    lua_rawseti(L, -2, ++index);
  }
  return 1;
}
void Graphics::bind_custom_image_provider(
    std::function<std::optional<Bytes>(std::string_view, int)> provider) {
  impl_->custom_images = std::move(provider);
}
}  // namespace headunit::theme::script
