// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_cluster.hpp>
#include <headunit/drive_mode_profile.hpp>
#include <headunit/indicator_symbols.hpp>
#include <cairo.h>
#include <headunit/theme/brand_artwork.hpp>
#include <plist/plist.h>
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <sys/stat.h>
#include <unistd.h>

// Native instrument cluster. Shared-state readers come first, then a small
// drawing vocabulary, the components, and finally the frame composition.
// Geometry lives in cluster_layout (native_cluster.hpp); colors in `tone`.
namespace headunit {
namespace {
std::string state_file(const std::string& path, std::size_t maximum) {
  // A malformed shared-state path must not wait for a pipe writer or device
  // while this renderer is keeping the native instruments available.
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return {};
  struct Close {
    int fd;
    ~Close() { close(fd); }
  } owner{fd};
  struct stat before{}, after{}, named{};
  if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
      std::uint64_t(before.st_size) > maximum)
    return {};
  std::string data(std::size_t(before.st_size), '\0');
  std::size_t offset = 0;
  unsigned interrupted = 0;
  while (offset < data.size()) {
    const auto count = read(fd, data.data() + offset, data.size() - offset);
    if (count < 0 && errno == EINTR && ++interrupted <= 3) continue;
    if (count <= 0) return {};
    offset += std::size_t(count);
  }
  if (fstat(fd, &after) != 0 || lstat(path.c_str(), &named) != 0) return {};
  const auto same = [](const struct stat& a, const struct stat& b) {
    const bool identity = a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size;
#ifdef __APPLE__
    return identity && a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec &&
           a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
           a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec &&
           a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
#else
    return identity && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
           a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
           a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
#endif
  };
  if (!same(before, after) || !same(after, named) || !S_ISREG(named.st_mode)) return {};
  return data;
}

std::string string(plist_t root, const char* key) {
  const auto node = plist_dict_get_item(root, key);
  if (!node || plist_get_node_type(node) != PLIST_STRING) return {};
  // The allocating C-string accessor asserts on JSON strings containing an
  // escaped NUL. Keep the explicit length and validate before drawing or using
  // a filename, so malformed metadata cannot retire the instrument renderer.
  std::uint64_t length = 0;
  const auto* raw = plist_get_string_ptr(node, &length);
  if (!raw || length > 2048) return {};
  const std::string_view value(raw, std::size_t(length));
  for (std::size_t at = 0; at < value.size();) {
    const auto first = static_cast<unsigned char>(value[at]);
    const unsigned count = first < 128                      ? 1
                           : first >= 0xc2 && first <= 0xdf ? 2
                           : first >= 0xe0 && first <= 0xef ? 3
                           : first >= 0xf0 && first <= 0xf4 ? 4
                                                            : 0;
    if (!count || at + count > value.size() || first < 32 || first == 127) return {};
    std::uint32_t code = first & ((1u << (8 - count - 1)) - 1);
    for (unsigned i = 1; i < count; ++i) {
      const auto next = static_cast<unsigned char>(value[at + i]);
      if ((next & 0xc0) != 0x80) return {};
      code = (code << 6) | (next & 63);
    }
    if ((count == 3 && code < 0x800) || (count == 4 && code < 0x10000) || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff))
      return {};
    at += count;
  }
  return std::string(value);
}
struct MediaInfo {
  bool available = false;
  std::string title, artist, album, status;
  double progress = -1;
  cairo_surface_t* artwork = nullptr;  // Owned by the thread-local decoder cache.
};
MediaInfo load_media(const std::string& path, Clock::time_point now) {
  MediaInfo media;
  if (!path.empty()) {
    // Metadata is written at human/UI rates. Keep file I/O and artwork decode
    // out of the 20 Hz gauge update path except when a bounded cache expires.
    static thread_local std::string cached_path, cached_data;
    static thread_local Clock::time_point next_read{};
    bool refreshed = false;
    if (path != cached_path || now >= next_read) {
      cached_path = path;
      next_read = now + std::chrono::milliseconds(250);
      cached_data = state_file(path, 32768);
      refreshed = true;
    }
    const auto& data = cached_data;
    plist_t raw = nullptr;
    if (data.size() <= 32768) plist_from_json(data.data(), data.size(), &raw);
    const std::unique_ptr<void, decltype(&plist_free)> owner(raw, plist_free);
    if (raw && plist_get_node_type(raw) == PLIST_DICT) {
      uint8_t available = 0;
      uint64_t stamp = 0;
      const auto a = plist_dict_get_item(raw, "available"),
                 t = plist_dict_get_item(raw, "updated_monotonic_ms");
      if (a && plist_get_node_type(a) == PLIST_BOOLEAN) plist_get_bool_val(a, &available);
      if (t && plist_get_node_type(t) == PLIST_UINT) plist_get_uint_val(t, &stamp);
      const auto ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
      const auto title = string(raw, "title"), artist = string(raw, "artist");
      // Track text is optional; only the source's dated availability decides
      // whether playback information is current, including during startup.
      if (available && t && plist_get_node_type(t) == PLIST_UINT && ms >= 0 &&
          stamp <= uint64_t(ms) && uint64_t(ms) - stamp <= 3000) {
        media.available = true;
        media.title = title.empty() ? "Unknown title" : title;
        media.artist = artist;
        media.album = string(raw, "album");
        media.status = string(raw, "status");
        const auto duration = plist_dict_get_item(raw, "duration_ms"),
                   position = plist_dict_get_item(raw, "position_ms");
        if (duration && position && plist_get_node_type(duration) == PLIST_UINT &&
            plist_get_node_type(position) == PLIST_UINT) {
          uint64_t total = 0, elapsed = 0;
          plist_get_uint_val(duration, &total);
          plist_get_uint_val(position, &elapsed);
          if (total > 0) media.progress = std::clamp(double(elapsed) / double(total), 0., 1.);
        }
        const auto name = string(raw, "artwork");
        if (!name.empty() && std::filesystem::path(name).filename() == name && name != "." &&
            name != "..") {
          const auto file = std::filesystem::path(path).parent_path() / name;
          static thread_local std::string cached_art_key;
          static thread_local std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>
              cached_art(nullptr, cairo_surface_destroy);
          auto digest = string(raw, "artwork_id");
          const bool valid_digest =
              digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
              });
          for (auto& c : digest)
            if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a');
          const auto key = file.string() + digest;
          if (refreshed || key != cached_art_key) {
            const bool reusable = key == cached_art_key && cached_art &&
                                  cairo_surface_status(cached_art.get()) == CAIRO_STATUS_SUCCESS;
            cached_art_key = key;
            bool verified = false;
            // Pin one bounded file snapshot to its metadata hash. Decode those
            // same bytes, never a second pathname read that may have changed.
            const auto png = valid_digest ? state_file(file.string(), 1048576) : std::string{};
            const auto byte = [&](unsigned offset) {
              return static_cast<unsigned char>(png[offset]);
            };
            const auto number = [&](unsigned offset) {
              return (uint32_t(byte(offset)) << 24) | (uint32_t(byte(offset + 1)) << 16) |
                     (uint32_t(byte(offset + 2)) << 8) | byte(offset + 3);
            };
            if (png.size() >= 33 && png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0 &&
                number(8) == 13 && png.compare(12, 4, "IHDR") == 0 && number(16) > 0 &&
                number(16) <= 1024 && number(20) > 0 && number(20) <= 1024) {
              std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
              const bool hashed = SHA256(reinterpret_cast<const unsigned char*>(png.data()),
                                         png.size(), hash.data()) != nullptr;
              std::string actual;
              actual.reserve(64);
              for (const auto b : hash) {
                actual += "0123456789abcdef"[b >> 4];
                actual += "0123456789abcdef"[b & 15];
              }
              if (hashed && actual == digest) {
                verified = true;
                struct Reader {
                  const std::string& png;
                  std::size_t offset = 0;
                } reader{png};
                const auto read_png = [](void* closure, unsigned char* target,
                                         unsigned length) -> cairo_status_t {
                  auto& reader = *static_cast<Reader*>(closure);
                  if (length > reader.png.size() - reader.offset) return CAIRO_STATUS_READ_ERROR;
                  std::copy_n(
                      reinterpret_cast<const unsigned char*>(reader.png.data() + reader.offset),
                      length, target);
                  reader.offset += length;
                  return CAIRO_STATUS_SUCCESS;
                };
                if (!reusable)
                  cached_art.reset(cairo_image_surface_create_from_png_stream(read_png, &reader));
              }
            }
            if (!verified) cached_art.reset();
          }
          auto* art = cached_art.get();
          if (valid_digest && art && cairo_surface_status(art) == CAIRO_STATUS_SUCCESS &&
              cairo_image_surface_get_width(art) > 0 &&
              cairo_image_surface_get_width(art) <= 1024 &&
              cairo_image_surface_get_height(art) > 0 &&
              cairo_image_surface_get_height(art) <= 1024)
            media.artwork = art;
        }
      }
    }
  }
  return media;
}

// ---------------------------------------------------------------------------
// Drawing vocabulary. Every color, type size and shape below is named once.
// ---------------------------------------------------------------------------
namespace layout = cluster_layout;
using Region = cluster_layout::Region;
constexpr double pi = 3.141592653589793;

struct Tone {
  double r, g, b, a = 1;
};
namespace tone {
constexpr Tone backdrop{.047, .051, .059}, card{.090, .098, .114}, card_edge{.150, .161, .180},
    glass{.055, .061, .071, .88}, glass_edge{1, 1, 1, .08}, scrim{.027, .031, .039},
    text{.953, .957, .965}, muted{.640, .659, .690}, faint{.420, .439, .471}, track{1, 1, 1, .13},
    redline{1., .36, .33}, green{.49, .84, .64}, amber{1., .72, .30}, red{1., .42, .42};
thread_local Tone speed{.62, .84, 1.}, power{1., .77, .47};
}  // namespace tone
void source(cairo_t* c, Tone t) {
  cairo_set_source_rgba(c, t.r, t.g, t.b, t.a);
}
Tone with_alpha(Tone t, double a) {
  t.a = a;
  return t;
}

enum class Weight { regular, bold };
void font(cairo_t* c, double size, Weight weight) {
  cairo_select_font_face(
      c, "sans", CAIRO_FONT_SLANT_NORMAL,
      weight == Weight::bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(c, size);
}
double advance(cairo_t* c, const std::string& value) {
  cairo_text_extents_t e{};
  cairo_text_extents(c, value.c_str(), &e);
  return e.x_advance;
}
enum class Align { left, center, right };
// Draws one line at a baseline; returns its advance width.
double text(cairo_t* c, double x, double y, double size, const std::string& value,
            Tone t = tone::text, Weight weight = Weight::regular, Align align = Align::left) {
  font(c, size, weight);
  source(c, t);
  const double width = advance(c, value);
  cairo_move_to(c,
                align == Align::left     ? x
                : align == Align::center ? x - width / 2
                                         : x - width,
                y);
  cairo_show_text(c, value.c_str());
  cairo_new_path(c);
  return width;
}
std::size_t fitting_prefix(cairo_t* c, const std::string& value, double width,
                           const std::string& suffix = {}) {
  // Metadata fields are bounded to 2048 UTF-8 bytes. Binary search whole
  // codepoints, so an ellipsis cannot hand a broken multibyte string to Cairo.
  cairo_text_extents_t full{};
  cairo_text_extents(c, (value + suffix).c_str(), &full);
  if (std::max(full.x_advance, full.x_bearing + full.width) <= width) return value.size();
  std::vector<std::size_t> ends{0};
  for (std::size_t i = 1; i < value.size(); ++i)
    if ((static_cast<unsigned char>(value[i]) & 0xc0) != 0x80) ends.push_back(i);
  ends.push_back(value.size());
  std::size_t low = 0, high = ends.size();
  while (low + 1 < high) {
    const auto middle = (low + high) / 2;
    cairo_text_extents_t extent{};
    const auto candidate = value.substr(0, ends[middle]) + suffix;
    cairo_text_extents(c, candidate.c_str(), &extent);
    if (std::max(extent.x_advance, extent.x_bearing + extent.width) <= width)
      low = middle;
    else
      high = middle;
  }
  return ends[low];
}
// Wraps at words into at most `lines`, ending with an explicit ellipsis; an
// unbroken maximum-size title still makes bounded progress. Returns lines used.
unsigned paragraph(cairo_t* c, double x, double y, double size, std::string value, double width,
                   unsigned lines, Tone t, Weight weight = Weight::regular,
                   double line_height = 0) {
  if (value.empty()) return 0;
  font(c, size, weight);
  if (line_height == 0) line_height = size * 1.2;
  for (unsigned line = 0; line < lines; ++line) {
    const auto fitted = fitting_prefix(c, value, width);
    if (fitted == value.size()) {
      text(c, x, y + line * line_height, size, value, t, weight);
      return line + 1;
    }
    const bool last = line + 1 == lines;
    auto end = last ? fitting_prefix(c, value, width, "…") : fitted;
    const auto space = value.find_last_of(' ', end);
    if (space != std::string::npos && space > 0) end = space;
    text(c, x, y + line * line_height, size, value.substr(0, end) + (last ? "…" : ""), t, weight);
    if (last) return lines;
    value.erase(0, end);
    value.erase(0, value.find_first_not_of(' '));
    font(c, size, weight);
  }
  return lines;
}
void rounded(cairo_t* c, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(c);
  cairo_arc(c, x + w - r, y + r, r, -pi / 2, 0);
  cairo_arc(c, x + w - r, y + h - r, r, 0, pi / 2);
  cairo_arc(c, x + r, y + h - r, r, pi / 2, pi);
  cairo_arc(c, x + r, y + r, r, pi, 3 * pi / 2);
  cairo_close_path(c);
}
void panel(cairo_t* c, double x, double y, double w, double h, double r, Tone fill,
           std::optional<Tone> edge = {}) {
  rounded(c, x, y, w, h, r);
  source(c, fill);
  if (edge) {
    cairo_fill_preserve(c);
    source(c, *edge);
    cairo_set_line_width(c, 1);
    cairo_stroke(c);
  } else
    cairo_fill(c);
}
void clip(cairo_t* c, Region r) {
  cairo_rectangle(c, r.x, r.y, r.width, r.height);
  cairo_clip(c);
}
std::string reading(std::optional<double> v, bool valid, const char* format = "%.0f") {
  if (!valid || !v) return "--";
  char b[32];
  std::snprintf(b, sizeof(b), format, *v);
  return b;
}

// Temperatures remain canonical Celsius in the admitted snapshot and gauge arcs.
// Only the existing numerical text/label follows this static display setting.
const char* temperature_label(const NativeClusterConfiguration& config) {
  return config.temperature_unit == NativeTemperatureUnit::fahrenheit ? "°F" : "°C";
}
std::string temperature_reading(std::optional<double> celsius, bool valid,
                                const NativeClusterConfiguration& config) {
  if (!valid || !celsius) return "--";
  const char* format =
      config.temperature_precision == NativeTemperaturePrecision::one_decimal ? "%.1f" : "%.0f";
  if (config.temperature_unit == NativeTemperatureUnit::celsius)
    return reading(celsius, true, format);
  return reading(*celsius * 1.8 + 32., true, format);
}

// ---------------------------------------------------------------------------
// Backdrops. Over the map, scrims darken only where instruments sit, so the
// map stays visible across the full width. Elsewhere the canvas is opaque.
// ---------------------------------------------------------------------------
void linear_scrim(cairo_t* c, double x0, double y0, double x1, double y1, double strength) {
  auto* g = cairo_pattern_create_linear(x0, y0, x1, y1);
  cairo_pattern_add_color_stop_rgba(g, 0, tone::scrim.r, tone::scrim.g, tone::scrim.b, strength);
  cairo_pattern_add_color_stop_rgba(g, .55, tone::scrim.r, tone::scrim.g, tone::scrim.b,
                                    strength * .55);
  cairo_pattern_add_color_stop_rgba(g, 1, tone::scrim.r, tone::scrim.g, tone::scrim.b, 0);
  cairo_set_source(c, g);
  cairo_rectangle(c, 0, 0, layout::width, layout::height);
  cairo_fill(c);
  cairo_pattern_destroy(g);
}
void map_scrims(cairo_t* c) {
  linear_scrim(c, 0, 0, 720, 0, .92);
  linear_scrim(c, layout::width, 0, layout::width - 720, 0, .92);
  linear_scrim(c, 0, layout::height, 0, layout::height - 200, .85);
  linear_scrim(c, 0, 0, 0, 140, .70);
}
void glow(cairo_t* c, double x, double y, double radius, Tone t) {
  auto* g = cairo_pattern_create_radial(x, y, 0, x, y, radius);
  cairo_pattern_add_color_stop_rgba(g, 0, t.r, t.g, t.b, t.a);
  cairo_pattern_add_color_stop_rgba(g, 1, t.r, t.g, t.b, 0);
  cairo_set_source(c, g);
  cairo_arc(c, x, y, radius, 0, 2 * pi);
  cairo_fill(c);
  cairo_pattern_destroy(g);
}
void opaque_backdrop(cairo_t* c) {
  auto* g = cairo_pattern_create_linear(0, 0, 0, layout::height);
  cairo_pattern_add_color_stop_rgb(g, 0, .063, .067, .078);
  cairo_pattern_add_color_stop_rgb(g, 1, tone::backdrop.r, tone::backdrop.g, tone::backdrop.b);
  cairo_set_source(c, g);
  cairo_rectangle(c, 0, 0, layout::width, layout::height);
  cairo_fill(c);
  cairo_pattern_destroy(g);
  glow(c, 300, 316, 330, with_alpha(tone::speed, .07));
  glow(c, 1620, 316, 330, with_alpha(tone::power, .06));
}

// ---------------------------------------------------------------------------
// Instruments: a 270° ring with a large digital reading in its center.
// ---------------------------------------------------------------------------
struct Ring {
  double x, y, radius;
};
constexpr Ring speed_ring{300, 316, 196}, power_ring{1620, 316, 196};
constexpr double sweep_start = pi * .75, sweep_end = pi * 2.25;

void ring(cairo_t* c, Ring r, std::optional<double> fraction, Tone accent,
          std::optional<double> redline_from = {}) {
  const auto at = [&](double f) {
    return sweep_start + (sweep_end - sweep_start) * f;
  };
  cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_width(c, 10);
  source(c, tone::track);
  cairo_arc(c, r.x, r.y, r.radius, sweep_start, sweep_end);
  cairo_stroke(c);
  if (redline_from) {
    cairo_set_line_cap(c, CAIRO_LINE_CAP_BUTT);
    source(c, with_alpha(tone::redline, .55));
    cairo_arc(c, r.x, r.y, r.radius, at(*redline_from), sweep_end);
    cairo_stroke(c);
    cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND);
  }
  for (int i = 0; i <= 10; ++i) {
    const double a = at(i / 10.), inner = r.radius + 16, outer = r.radius + (i % 5 ? 22 : 28);
    source(c, with_alpha(tone::text, i % 5 ? .22 : .42));
    cairo_set_line_width(c, i % 5 ? 2 : 3);
    cairo_move_to(c, r.x + inner * std::cos(a), r.y + inner * std::sin(a));
    cairo_line_to(c, r.x + outer * std::cos(a), r.y + outer * std::sin(a));
    cairo_stroke(c);
  }
  if (!fraction) return;
  const double f = std::clamp(*fraction, 0., 1.);
  if (f > 0) {
    cairo_set_line_width(c, 22);
    source(c, with_alpha(accent, .16));
    cairo_arc(c, r.x, r.y, r.radius, sweep_start, at(f));
    cairo_stroke(c);
    cairo_set_line_width(c, 10);
    source(c, accent);
    cairo_arc(c, r.x, r.y, r.radius, sweep_start, at(f));
    cairo_stroke(c);
  }
  const double a = at(f);
  source(c, tone::text);
  cairo_arc(c, r.x + r.radius * std::cos(a), r.y + r.radius * std::sin(a), 8, 0, 2 * pi);
  cairo_fill(c);
}
// A labelled value pill below an instrument, clipped to its own region.
void pill(cairo_t* c, Region r, const char* label, const std::string& value) {
  // Inset by half the outline so the stroke stays inside the region.
  panel(c, r.x + .5, r.y + .5, r.width - 1, r.height - 1, (r.height - 1) / 2., tone::glass,
        tone::glass_edge);
  cairo_save(c);
  clip(c, r);
  const double label_width = text(c, r.x + 18, r.y + 26, 14, label, tone::muted, Weight::bold);
  font(c, 20, Weight::bold);
  const double room = r.width - label_width - 46;
  auto shown = value.substr(0, fitting_prefix(c, value, room));
  if (shown.size() < value.size())
    shown = value.substr(0, fitting_prefix(c, value, room, "…")) + "…";
  text(c, r.x + r.width - 18, r.y + 28, 20, shown, tone::text, Weight::bold, Align::right);
  cairo_restore(c);
}
std::string drive_mode_label(const VehicleSample& sample) {
  if (!sample.drive_mode) return "--";
  std::string value(drive_mode_identifier_key(*sample.drive_mode));
  for (auto& character : value) {
    if (character >= 'a' && character <= 'z')
      character = char(character - 'a' + 'A');
    else if (character == '_' || character == '-')
      character = ' ';
  }
  return value;
}
// Formatting changes only the instantaneous digital speed, after unit conversion.
std::string formatted_speed(const VehicleSample& sample, bool valid,
                            const NativeClusterConfiguration& config) {
  if (!valid || !sample.speed_kmh) return "--";
  const auto speed = *sample.speed_kmh / (config.miles_per_hour ? 1.609344 : 1.);
  return reading(speed, true,
                 config.speed_precision == NativeSpeedPrecision::one_decimal ? "%.1f" : "%.0f");
}
double fitted_speed_size(cairo_t* c, const std::string& value, double x, double y, double maximum,
                         Region allocation, double bottom) {
  const auto fits = [&](double size) {
    font(c, size, Weight::bold);
    cairo_text_extents_t ink{};
    cairo_text_extents(c, value.c_str(), &ink);
    cairo_move_to(c, x - ink.x_advance / 2, y);
    double positioned_x = 0;
    double positioned_y = 0;
    cairo_get_current_point(c, &positioned_x, &positioned_y);
    const double left = positioned_x + ink.x_bearing;
    const double right = left + ink.width;
    const double top = positioned_y + ink.y_bearing;
    return left >= allocation.x && right <= allocation.x + allocation.width &&
           top >= allocation.y && top + ink.height <= bottom;
  };
  if (fits(maximum)) return maximum;
  constexpr double minimum = 124;
  if (!fits(minimum)) throw std::runtime_error("native speed text cannot fit at minimum size");
  double low = minimum;
  double high = maximum;
  // Keep only actually measured fitting sizes. Cairo hinting need not produce
  // perfectly linear metrics; the last accepted size remains inside the box.
  for (unsigned attempt = 0; attempt < 12; ++attempt) {
    const double middle = (low + high) / 2;
    if (fits(middle))
      low = middle;
    else
      high = middle;
  }
  return low;
}
void speed_text(cairo_t* c, const VehicleSample& sample, bool valid,
                const NativeClusterConfiguration& config, double x, double y, double maximum,
                Region allocation, double unit_y, double unit_size) {
  const auto value = formatted_speed(sample, valid, config);
  if (config.speed_precision == NativeSpeedPrecision::whole || value == "--") {
    text(c, x, y, maximum, value, tone::text, Weight::bold, Align::center);
    return;
  }
  // The unchanged following unit label reserves its actual ink, not an assumed
  // font ascent. Neither this measurement nor sizing changes its drawing path.
  cairo_save(c);
  font(c, unit_size, Weight::regular);
  cairo_text_extents_t unit{};
  cairo_text_extents(c, config.miles_per_hour ? "mph" : "km/h", &unit);
  cairo_restore(c);
  const double bottom = std::min(double(allocation.y + allocation.height), unit_y + unit.y_bearing);
  const double size = fitted_speed_size(c, value, x, y, maximum, allocation, bottom);
  text(c, x, y, size, value, tone::text, Weight::bold, Align::center);
}
void speed_instrument(cairo_t* c, const VehicleSample& sample, bool valid,
                      const NativeClusterConfiguration& config) {
  const bool known = valid && sample.speed_kmh;
  ring(c, speed_ring,
       known ? std::optional(*sample.speed_kmh / config.speed_maximum_kmh) : std::nullopt,
       tone::speed);
  speed_text(c, sample, valid, config, speed_ring.x, speed_ring.y + 42, 124, layout::speed,
             speed_ring.y + 88, 24);
  text(c, speed_ring.x, speed_ring.y + 88, 24, config.miles_per_hour ? "mph" : "km/h", tone::muted,
       Weight::regular, Align::center);
  if (sample.drive_mode_catalog && valid_drive_mode_catalog(*sample.drive_mode_catalog))
    pill(c, layout::drive_mode, "MODE", drive_mode_label(sample));
}
void power_instrument(cairo_t* c, const VehicleSample& sample, bool valid,
                      const NativeClusterConfiguration& config) {
  if (config.show_rpm) {
    const bool known = valid && sample.engine_rpm;
    ring(c, power_ring,
         known ? std::optional(*sample.engine_rpm / config.rpm_maximum) : std::nullopt, tone::power,
         config.rpm_redline / config.rpm_maximum);
    text(c, power_ring.x, power_ring.y + 42, 124,
         known ? reading(*sample.engine_rpm / 1000., true, "%.1f") : "--", tone::text, Weight::bold,
         Align::center);
    text(c, power_ring.x, power_ring.y + 88, 24, "× 1000 rpm", tone::muted, Weight::regular,
         Align::center);
  }
  // The shared source already filtered this coherent observation. Gear stays
  // separate from RPM and is shown only when the transmission reports it.
  if (const auto gear = transmission_display_label(sample.transmission); gear != "--")
    pill(c, layout::gear, "GEAR", gear);
}

// ---------------------------------------------------------------------------
// Status: active telltales only, and an explicit simulated/unavailable marker.
// ---------------------------------------------------------------------------
void telltales(cairo_t* c, const VehicleSample& sample, Clock::time_point now,
               const TurnSignalDisplay* animated) {
  // Off and dark flashing phases are empty. Unavailable direction data has
  // a dashed outline; other unknown warnings remain hidden.
  const auto indicators = cluster_indicator_snapshot(sample, now);
  for (const auto& definition : indicator_definitions) {
    const auto frame = layout::indicator_frame(definition.id);
    if (animated &&
        (definition.id == Indicator::left_turn || definition.id == Indicator::right_turn)) {
      const std::size_t i = definition.id == Indicator::right_turn ? 1 : 0;
      // Always recheck freshness: a supplied presentation cannot revive a lamp.
      if (indicators.unavailable_turns[i])
        draw_turn_signal_unavailable(c, definition.id, frame.x, frame.y, frame.width, frame.height);
      else if (animated->intensity[i] > 0)
        draw_turn_signal_lamp(c, definition.id, frame.x, frame.y, frame.width, frame.height,
                              animated->intensity[i]);
    } else if (indicators.visible.test(static_cast<std::size_t>(definition.id)))
      draw_indicator_telltale(c, definition, frame.x, frame.y, frame.width, frame.height);
    else if ((definition.id == Indicator::left_turn && indicators.unavailable_turns[0]) ||
             (definition.id == Indicator::right_turn && indicators.unavailable_turns[1]))
      draw_turn_signal_unavailable(c, definition.id, frame.x, frame.y, frame.width, frame.height);
  }
}
void data_status(cairo_t* c, const VehicleSample& sample, bool valid) {
  if (valid && !sample.simulated) return;
  const auto label = valid ? "SIMULATED VEHICLE DATA" : "VEHICLE DATA UNAVAILABLE";
  const auto t = valid ? tone::amber : tone::red;
  const auto r = layout::data_status;
  font(c, 14, Weight::bold);
  const double width = std::min(double(r.width), advance(c, label) + 28);
  panel(c, r.x, r.y, width, r.height, r.height / 2., tone::glass, with_alpha(t, .30));
  text(c, r.x + 14, r.y + 21, 14, label, with_alpha(t, .9), Weight::bold);
}

// ---------------------------------------------------------------------------
// Center pages. Map leaves the center to the phone; the others are cards.
// ---------------------------------------------------------------------------
void artwork(cairo_t* c, double x, double y, double size, double radius, const MediaInfo& media) {
  panel(c, x, y, size, size, radius, tone::card, tone::card_edge);
  if (media.artwork) {
    const int w = cairo_image_surface_get_width(media.artwork),
              h = cairo_image_surface_get_height(media.artwork);
    cairo_save(c);
    rounded(c, x, y, size, size, radius);
    cairo_clip(c);
    cairo_translate(c, x, y);
    const double scale = std::max(size / w, size / h);
    cairo_scale(c, scale, scale);
    cairo_set_source_surface(c, media.artwork, (size / scale - w) / 2, (size / scale - h) / 2);
    cairo_paint(c);
    cairo_restore(c);
  } else {
    text(c, x + size / 2, y + size * .66, size * .42, "♪", tone::faint, Weight::regular,
         Align::center);
  }
}
std::string playback_status(const MediaInfo& media) {
  return media.status == "playing"  ? "PLAYING"
         : media.status == "paused" ? "PAUSED"
                                    : "MEDIA CONNECTED";
}
void media_page(cairo_t* c, const MediaInfo& media, bool phone_connected) {
  const auto r = layout::center;
  const double art = 300, x = r.x + art + 60, width = r.x + r.width - x;
  artwork(c, r.x + 20, r.y + 60, art, 24, media);
  text(c, x, r.y + 92, 16, media.available ? "NOW PLAYING" : "MUSIC", tone::muted, Weight::bold);
  const auto title = media.available   ? media.title
                     : phone_connected ? "Nothing playing"
                                       : "Connect your phone";
  const auto title_lines =
      paragraph(c, x, r.y + 146, 34, title, width, 2, tone::text, Weight::bold, 42);
  const double artist_y = r.y + 146 + title_lines * 42 + 10;
  const auto artist = media.available   ? media.artist
                      : phone_connected ? "Choose music on your phone"
                                        : "Your instruments remain available";
  const auto artist_lines =
      paragraph(c, x, artist_y, 24, artist, width, 2, tone::muted, Weight::regular, 30);
  if (media.available)
    paragraph(c, x, artist_y + artist_lines * 30 + 4, 20, media.album, width, 1, tone::faint);
  if (media.progress >= 0) {
    panel(c, x, r.y + 336, width, 6, 3, tone::track);
    panel(c, x, r.y + 336, width * media.progress, 6, 3, tone::speed);
  }
  text(c, x, r.y + 380, 15,
       media.available   ? playback_status(media)
       : phone_connected ? "CARPLAY CONNECTED"
                         : "NO PHONE CONNECTED",
       tone::faint, Weight::bold);
}
struct Card {
  std::string name, value, unit, detail;
};
void card_grid(cairo_t* c, const char* heading, const std::string& title,
               const std::array<Card, 6>& cards) {
  const auto r = layout::center;
  text(c, r.x, r.y + 28, 16, heading, tone::muted, Weight::bold);
  text(c, r.x, r.y + 66, 28, title, tone::text, Weight::bold);
  constexpr double gap = 24, height = 158;
  const double width = (r.width - 2 * gap) / 3;
  for (std::size_t i = 0; i < cards.size(); ++i) {
    const auto& card = cards[i];
    const double x = r.x + (i % 3) * (width + gap), y = r.y + 96 + (i / 3) * (height + gap);
    panel(c, x, y, width, height, 18, tone::card, tone::card_edge);
    cairo_save(c);
    cairo_rectangle(c, x + 16, y + 8, width - 32, height - 16);
    cairo_clip(c);
    text(c, x + 22, y + 38, 15, card.name, tone::muted, Weight::bold);
    // Fit the complete supplied value and unit together. Clipping a bounded
    // trip duration or large distance can hide its distinguishing last digit.
    font(c, 42, Weight::bold);
    cairo_text_extents_t value_extent{};
    cairo_text_extents(c, card.value.c_str(), &value_extent);
    font(c, 19, Weight::regular);
    cairo_text_extents_t unit_extent{};
    cairo_text_extents(c, card.unit.c_str(), &unit_extent);
    const double value_room =
                     std::max(value_extent.x_advance, value_extent.x_bearing + value_extent.width),
                 unit_room =
                     std::max(unit_extent.x_advance, unit_extent.x_bearing + unit_extent.width),
                 gap = card.unit.empty() ? 0 : 10,
                 scale = std::min(1., (width - 44 - gap) / (value_room + unit_room));
    const double value_width =
        text(c, x + 22, y + 100, 42 * scale, card.value, tone::text, Weight::bold);
    text(c, x + 22 + gap + value_width, y + 100, 19 * scale, card.unit, tone::muted);
    text(c, x + 22, y + 134, 14, card.detail, tone::faint);
    cairo_restore(c);
  }
}
// International mile: exactly 1609.344 metres. Convert display text only;
// canonical values, source timing and authored gauge scales stay in kilometres.
std::optional<double> displayed_distance(std::optional<double> kilometers,
                                         const NativeClusterConfiguration& config) {
  if (!kilometers || config.distance_unit == NativeDistanceUnit::kilometers) return kilometers;
  return *kilometers / 1.609344;
}
const char* distance_label(const NativeClusterConfiguration& config) {
  return config.distance_unit == NativeDistanceUnit::miles ? "mi" : "km";
}
const char* pressure_label(const NativeClusterConfiguration& config) {
  switch (config.pressure_unit) {
    case NativePressureUnit::kpa:
      return "kPa";
    case NativePressureUnit::bar:
      return "bar";
    case NativePressureUnit::psi:
      return "psi";
  }
  throw std::invalid_argument("unknown native pressure unit");
}
std::string pressure_reading(std::optional<double> kpa, bool valid,
                             const NativeClusterConfiguration& config) {
  if (!valid || !kpa) return "--";
  // NIST SP 811 Appendix B.8 and footnotes 22/23: bar = 100 kPa,
  // inch = 0.0254 m, pound = 0.45359237 kg, standard gravity = 9.80665 m/s².
  // https://www.nist.gov/pml/special-publication-811/nist-guide-si-footnotes
  // Exact definitions, evaluated in binary64; display text only, no calibration.
  if (config.pressure_unit == NativePressureUnit::bar) return reading(*kpa / 100., true, "%.1f");
  if (config.pressure_unit == NativePressureUnit::psi) {
    constexpr double pascals_per_psi = (0.45359237 * 9.80665) / (0.0254 * 0.0254);
    return reading(*kpa * 1000. / pascals_per_psi, true);
  }
  return reading(kpa, valid);
}
void vehicle_page(cairo_t* c, const VehicleSample& sample, bool valid,
                  const NativeClusterConfiguration& config) {
  const auto value = [&](std::optional<double> v, const char* format = "%.0f") {
    return reading(v, valid, format);
  };
  card_grid(
      c, "VEHICLE", valid ? "Vehicle readings" : "Vehicle data unavailable",
      {{{"FUEL", value(sample.fuel_percent), "%", ""},
        {"COOLANT", temperature_reading(sample.coolant_c, valid, config), temperature_label(config),
         ""},
        {"OIL TEMPERATURE", temperature_reading(sample.oil_c, valid, config),
         temperature_label(config), ""},
        {"OIL PRESSURE", pressure_reading(sample.oil_pressure_kpa, valid, config),
         pressure_label(config), ""},
        {"RANGE", value(displayed_distance(sample.range_km, config)), distance_label(config), ""},
        {"BATTERY", value(sample.battery_voltage, "%.1f"), "V", ""}}});
}
// Native formats share the dated sample, units, ranges and reserved lamps.
void minimal_format(cairo_t* c, const VehicleSample& sample, bool valid,
                    const NativeClusterConfiguration& config) {
  cairo_save(c);
  clip(c, layout::minimal);
  speed_text(c, sample, valid, config, 960, 330, 192, layout::minimal, 382, 28);
  text(c, 960, 382, 28, config.miles_per_hour ? "mph" : "km/h", tone::muted, Weight::regular,
       Align::center);
  pill(c, {730, 430, 210, 44}, "GEAR", transmission_display_label(sample.transmission));
  if (sample.drive_mode_catalog) pill(c, {960, 430, 240, 44}, "MODE", drive_mode_label(sample));
  if (config.show_rpm)
    text(c, 960, 514, 20, reading(sample.engine_rpm, valid) + " rpm", tone::muted, Weight::regular,
         Align::center);
  cairo_restore(c);
}
void sport_format(cairo_t* c, const VehicleSample& sample, bool valid,
                  const NativeClusterConfiguration& config) {
  cairo_save(c);
  clip(c, layout::sport);
  const Ring tach{1170, 294, 168};
  const Region speed_allocation{layout::sport.x, layout::sport.y,
                                int(tach.x - tach.radius) - layout::sport.x, layout::sport.height};
  speed_text(c, sample, valid, config, 700, 326, 158, speed_allocation, 372, 26);
  text(c, 700, 372, 26, config.miles_per_hour ? "mph" : "km/h", tone::muted, Weight::regular,
       Align::center);
  if (config.show_rpm) {
    ring(c, tach,
         valid && sample.engine_rpm ? std::optional(*sample.engine_rpm / config.rpm_maximum)
                                    : std::nullopt,
         tone::power, config.rpm_redline / config.rpm_maximum);
    text(c, tach.x, tach.y + 26, 84, reading(sample.engine_rpm, valid), tone::text, Weight::bold,
         Align::center);
    text(c, tach.x, tach.y + 68, 22, "rpm", tone::muted, Weight::regular, Align::center);
  }
  pill(c, {600, 442, 200, 44}, "GEAR", transmission_display_label(sample.transmission));
  if (sample.drive_mode_catalog) pill(c, {1040, 442, 260, 44}, "MODE", drive_mode_label(sample));
  cairo_restore(c);
}
void gauges_format(cairo_t* c, const VehicleSample& sample, bool valid,
                   const NativeClusterConfiguration& config) {
  struct Gauge {
    const char* label;
    std::optional<double> value;
    double maximum;
    const char* unit;
    const char* format;
    bool temperature = false;
  };
  // Authored display scales only; these are not warning thresholds or readings.
  const std::array gauges{
      Gauge{"FUEL", sample.fuel_percent, 100, "%", "%.0f"},
      Gauge{"COOLANT", sample.coolant_c, 150, temperature_label(config), "%.0f", true},
      Gauge{"OIL TEMP", sample.oil_c, 180, temperature_label(config), "%.0f", true},
      Gauge{"OIL PRESSURE", sample.oil_pressure_kpa, 1000, pressure_label(config), "%.0f"},
      Gauge{"RANGE", sample.range_km, 1000, distance_label(config), "%.0f"},
      Gauge{"BATTERY", sample.battery_voltage, 18, "V", "%.1f"}};
  cairo_save(c);
  clip(c, layout::center);
  for (std::size_t i = 0; i < gauges.size(); ++i) {
    const auto& gauge = gauges[i];
    const Ring dial{layout::center.x + 132. + (i % 3) * 268.,
                    layout::center.y + 112. + (i / 3) * 214., 72};
    ring(c, dial, valid && gauge.value ? std::optional(*gauge.value / gauge.maximum) : std::nullopt,
         tone::speed);
    const auto displayed = i == 4 ? displayed_distance(gauge.value, config) : gauge.value;
    text(c, dial.x, dial.y + 8, 32,
         gauge.temperature ? temperature_reading(gauge.value, valid, config)
         : i == 3          ? pressure_reading(gauge.value, valid, config)
                           : reading(displayed, valid, gauge.format),
         tone::text, Weight::bold, Align::center);
    text(c, dial.x, dial.y + 32, 16, gauge.unit, tone::muted, Weight::regular, Align::center);
    text(c, dial.x, dial.y + 99, 14, gauge.label, tone::muted, Weight::bold, Align::center);
  }
  cairo_restore(c);
}
std::string trip_duration(std::optional<double> seconds) {
  if (!seconds) return "--";
  const auto count = std::uint64_t(*seconds);
  char value[48];
  std::snprintf(value, sizeof(value), "%llu:%02llu:%02llu",
                static_cast<unsigned long long>(count / 3600),
                static_cast<unsigned long long>((count / 60) % 60),
                static_cast<unsigned long long>(count % 60));
  return value;
}
// Exact gallon definitions: NIST SP 811 Appendix B.9 (Imperial 4.54609 L),
// https://www.nist.gov/pml/special-publication-811/nist-guide-si-appendix-b-conversion-factors/nist-guide-si-appendix-b9
// NIST HB44 Appendix C: US liquid gallon = 231 cubic inches exactly;
// https://www.nist.gov/system/files/documents/2024/04/09/NIST%20HB44%202024%20Appendix%20C.pdf
// exact inch = 0.0254 m, international mile = 1.609344 km. Display only;
// the supplied whole-trip mean and every acquisition/policy stay canonical.
const char* consumption_label(const NativeClusterConfiguration& config) {
  switch (config.fuel_consumption_unit) {
    case NativeFuelConsumptionUnit::us_mpg:
      return "mpg US";
    case NativeFuelConsumptionUnit::imperial_mpg:
      return "mpg Imperial";
    case NativeFuelConsumptionUnit::liters_per_100km:
      return "L/100 km";
  }
  throw std::invalid_argument("unknown native fuel consumption unit");
}
std::string consumption_reading(std::optional<double> liters_per_100km,
                                const NativeClusterConfiguration& config) {
  if (!liters_per_100km) return "--";
  if (config.fuel_consumption_unit == NativeFuelConsumptionUnit::liters_per_100km)
    return reading(liters_per_100km, true, "%.1f");
  // Zero consumption has an unbounded inverse, not an unavailable or zero MPG.
  // No division by zero and no nonfinite scalar/JSON is created.
  if (*liters_per_100km == 0) return "∞";
  constexpr double inch_meters = 0.0254;
  constexpr double us_gallon_liters = 231 * inch_meters * inch_meters * inch_meters * 1000;
  constexpr double imperial_gallon_liters = 4.54609;
  const double gallon_liters = config.fuel_consumption_unit == NativeFuelConsumptionUnit::us_mpg
                                   ? us_gallon_liters
                                   : imperial_gallon_liters;
  const double factor = 100 * gallon_liters / 1.609344;
  if (*liters_per_100km > factor / 10000) return reading(factor / *liters_per_100km, true, "%.1f");
  // Every positive finite canonical double has a finite mathematical inverse,
  // including subnormals whose MPG exceeds double's range. Form its bounded
  // decimal mantissa/exponent without computing that overflowing quotient.
  const double logarithm = std::log10(factor) - std::log10(*liters_per_100km);
  int exponent = int(std::floor(logarithm));
  double mantissa = std::pow(10., logarithm - exponent);
  if (mantissa >= 9.995) {
    mantissa /= 10;
    ++exponent;
  }
  char text[32];
  std::snprintf(text, sizeof(text), "%.2fe+%02d", mantissa, exponent);
  return text;
}
void trip_page(cairo_t* c, const VehicleSample& sample, const NativeClusterConfiguration& config) {
  const auto& trip = sample.trip;
  const auto number = [](std::optional<double> value) {
    return reading(value, true, "%.1f");
  };
  std::string heading = "--", heading_detail = "Heading unavailable";
  if (sample.heading) {
    constexpr std::array directions{"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const auto& value = *sample.heading;
    const auto sector = std::size_t(std::floor((value.degrees + 22.5) / 45.)) % directions.size();
    heading = std::string(directions[sector]) + " " +
              std::to_string(std::lround(value.degrees) % 360) + "°";
    heading_detail =
        value.reference == HeadingReference::true_north ? "True north" : "Magnetic north";
    heading_detail +=
        value.source == HeadingSource::vehicle_orientation ? " · Vehicle" : " · Course";
  }
  const auto average_detail = !trip || !trip->average_speed_basis ? ""
                              : *trip->average_speed_basis == TripAverageSpeedBasis::moving
                                  ? "Moving time basis"
                                  : "Total time basis";
  auto average_speed = trip ? trip->average_speed_kmh : std::nullopt;
  if (average_speed && config.miles_per_hour) *average_speed /= 1.609344;
  card_grid(
      c, "CURRENT TRIP", trip ? "Since last reset" : "Trip data unavailable",
      {{{"DISTANCE", number(displayed_distance(trip ? trip->distance_km : std::nullopt, config)),
         distance_label(config), ""},
        {"ELAPSED TIME", trip_duration(trip ? trip->elapsed_seconds : std::nullopt), "",
         "Hours : minutes : seconds"},
        {"MOVING TIME", trip_duration(trip ? trip->moving_seconds : std::nullopt), "",
         "Hours : minutes : seconds"},
        {"AVERAGE SPEED", number(average_speed), config.miles_per_hour ? "mph" : "km/h",
         average_detail},
        {"CONSUMPTION",
         consumption_reading(trip ? trip->fuel_efficiency_l_per_100km : std::nullopt, config),
         consumption_label(config), "Whole-trip mean"},
        {"HEADING", heading, "", heading_detail}}});
}

// ---------------------------------------------------------------------------
// Bottom bar: media (or phone status), guidance, and key readings.
// ---------------------------------------------------------------------------
void labelled(cairo_t* c, Region r, const char* label, const std::string& value,
              const std::string& detail = {}) {
  cairo_save(c);
  clip(c, r);
  text(c, r.x, r.y + 26, 13, label, tone::muted, Weight::bold);
  paragraph(c, r.x, r.y + 58, 24, value, r.width, 1, tone::text, Weight::bold);
  paragraph(c, r.x, r.y + 82, 16, detail, r.width, 1, tone::muted);
  cairo_restore(c);
}
void phone_status(cairo_t* c, Region r, bool phone_connected) {
  labelled(c, r, "PHONE", phone_connected ? "CarPlay connected" : "Awaiting connection");
}
void media_summary(cairo_t* c, const MediaInfo& media, bool phone_connected) {
  const auto r = layout::media;
  artwork(c, r.x, r.y + 8, 72, 14, media);
  const Region details{r.x + 92, r.y, r.width - 92, r.height};
  if (media.available) {
    const auto detail =
        media.artist + (media.artist.empty() || media.album.empty() ? "" : "  ·  ") + media.album;
    labelled(c, details, "NOW PLAYING", media.title, detail);
  } else
    labelled(c, details, "MUSIC", phone_connected ? "Nothing playing" : "No phone connected");
}
void arrow_head(cairo_t* c, double x, double y, double dx, double dy) {
  const double length = std::hypot(dx, dy), ux = dx / length, uy = dy / length;
  cairo_move_to(c, x - 15 * ux + 9 * uy, y - 15 * uy - 9 * ux);
  cairo_line_to(c, x, y);
  cairo_line_to(c, x - 15 * ux - 9 * uy, y - 15 * uy + 9 * ux);
}
// Drawn about a bottom-centre origin, at most 48 units wide each side and 78 tall.
void navigation_glyph(cairo_t* c, const NavigationSnapshot& navigation) {
  cairo_new_path(c);
  source(c, tone::speed);
  cairo_set_line_width(c, 4);
  cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND);
  const auto state = navigation.route_state.value_or(0);
  const auto type = (state == 1 || state == 6) && navigation.maneuver
                        ? navigation.maneuver->type.value_or(255)
                        : 255;
  if (type == 1 || type == 2) {
    const double d = type == 1 ? -1 : 1;
    cairo_move_to(c, 0, 0);
    cairo_line_to(c, 0, -46);
    cairo_curve_to(c, 0, -58, 7 * d, -62, 20 * d, -62);
    cairo_line_to(c, 48 * d, -62);
    arrow_head(c, 48 * d, -62, d, 0);
    cairo_stroke(c);
  } else if (type == 13 || type == 14) {
    const double d = type == 13 ? -1 : 1;
    // A generic fork distinguishes Keep from a full turn. The muted branch
    // describes only the symbol, not phone-supplied lane/junction geometry.
    source(c, tone::faint);
    cairo_move_to(c, 0, -30);
    cairo_curve_to(c, 0, -45, -22 * d, -48, -22 * d, -72);
    cairo_stroke(c);
    source(c, tone::speed);
    cairo_move_to(c, 0, 0);
    cairo_line_to(c, 0, -30);
    cairo_curve_to(c, 0, -45, 22 * d, -48, 22 * d, -72);
    arrow_head(c, 22 * d, -72, 0, -1);
    cairo_stroke(c);
  } else if (type == 49 || type == 50) {
    const double d = type == 49 ? -1 : 1;
    cairo_move_to(c, 0, 0);
    cairo_line_to(c, 0, -28);
    cairo_curve_to(c, 0, -39, 17 * d, -50, 32 * d, -72);
    arrow_head(c, 32 * d, -72, d, -1.5);
    cairo_stroke(c);
  } else if (type == 4) {
    cairo_move_to(c, 17, 0);
    cairo_line_to(c, 17, -54);
    cairo_curve_to(c, 17, -77, -17, -77, -17, -54);
    cairo_line_to(c, -17, -16);
    arrow_head(c, -17, -16, 0, 1);
    cairo_stroke(c);
  } else if (type == 0 || type == 3 || type == 5 || type == 11) {
    cairo_move_to(c, 0, 0);
    cairo_line_to(c, 0, -72);
    arrow_head(c, 0, -72, 0, -1);
    cairo_stroke(c);
    if (type == 11) {
      cairo_arc(c, 0, -5, 7, 0, 2 * pi);
      cairo_fill(c);
    }
  } else if (type == 10 || type == 12 || state == 2) {
    cairo_move_to(c, -14, 0);
    cairo_line_to(c, -14, -72);
    cairo_stroke(c);
    cairo_move_to(c, -14, -72);
    cairo_line_to(c, 25, -64);
    cairo_line_to(c, 25, -40);
    cairo_line_to(c, -14, -48);
    cairo_close_path(c);
    cairo_fill(c);
  } else {
    // Roundabout/exit direction and unknown extensions stay neutral until
    // their geometry is established. Route-status cards carry no turn arrow.
    cairo_arc(c, 0, -35, 24, 0, 2 * pi);
    cairo_stroke(c);
  }
}
const char* navigation_label(const NavigationSnapshot& navigation) {
  switch (navigation.route_state.value_or(0)) {
    case 1:
    case 6:
      return "NEXT TURN";
    case 2:
      return "ARRIVED";
    case 3:
      return "LOADING ROUTE";
    case 4:
      return "FINDING LOCATION";
    case 5:
      return "UPDATING ROUTE";
    default:
      return "NAVIGATION";
  }
}
void guidance(cairo_t* c, const NavigationSnapshot& navigation) {
  const auto r = layout::guidance;
  if (!navigation.available) {
    labelled(c, r, "NAVIGATION", "No active route");
    return;
  }
  cairo_save(c);
  clip(c, r);
  cairo_translate(c, r.x + 40, r.y + r.height - 10);
  cairo_scale(c, .85, .85);
  navigation_glyph(c, navigation);
  cairo_restore(c);
  const bool active_route = navigation.route_state == 1 || navigation.route_state == 6;
  const auto detail =
      navigation.route_state == 2 && !navigation.destination.empty() ? navigation.destination
      : active_route && navigation.maneuver
          ? (!navigation.maneuver->instruction.empty() ? navigation.maneuver->instruction
                                                       : navigation.maneuver->road)
          : navigation.current_road;
  labelled(c, {r.x + 96, r.y, r.width - 96, r.height}, navigation_label(navigation),
           navigation_prompt(navigation), detail);
}
void readings(cairo_t* c, const VehicleSample& sample, bool valid,
              const NativeClusterConfiguration& config) {
  const std::array<std::tuple<const char*, std::optional<double>, const char*, const char*>, 4>
      values{{{"FUEL", sample.fuel_percent, "%", "%.0f"},
              {"COOLANT", sample.coolant_c, temperature_label(config), "%.0f"},
              {"OIL", sample.oil_c, temperature_label(config), "%.0f"},
              {"BATTERY", sample.battery_voltage, "V", "%.1f"}}};
  const auto r = layout::readings;
  const double column = r.width / double(values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto [name, value, unit, format] = values[i];
    const double x = r.x + i * column;
    text(c, x, r.y + 26, 13, name, tone::muted, Weight::bold);
    const double width = text(c, x, r.y + 60, 28,
                              i == 1 || i == 2 ? temperature_reading(value, valid, config)
                                               : reading(value, valid, format),
                              tone::text, Weight::bold);
    text(c, x + width + 6, r.y + 60, 15, unit, tone::muted);
    if (i == 0) {
      panel(c, x, r.y + 72, column - 28, 5, 2.5, tone::track);
      if (valid && value)
        panel(c, x, r.y + 72, (column - 28) * std::clamp(*value / 100., 0., 1.), 5, 2.5,
              tone::green);
    }
  }
}
void bottom_bar(cairo_t* c, const ClusterContent& content, const MediaInfo& media, bool valid,
                bool map) {
  const auto r = layout::bottom_bar;
  panel(c, r.x, r.y, r.width, r.height, 22, map ? tone::glass : tone::card,
        map ? tone::glass_edge : tone::card_edge);
  // The Media page already shows the track, so its bar shows phone status
  // in the media slot and leaves the middle slot empty.
  const bool middle = map || content.center != ClusterCenter::media;
  source(c, map ? tone::glass_edge : tone::card_edge);
  cairo_set_line_width(c, 1);
  for (const auto divider : {layout::guidance.x - 18, layout::readings.x - 20}) {
    if (divider == layout::guidance.x - 18 && !middle) continue;
    cairo_move_to(c, divider + .5, r.y + 20);
    cairo_line_to(c, divider + .5, r.y + r.height - 20);
    cairo_stroke(c);
  }
  if (content.center == ClusterCenter::media)
    phone_status(c, layout::media, content.phone_connected);
  else
    media_summary(c, media, content.phone_connected);
  if (map)
    guidance(c, content.navigation);
  else if (middle)
    phone_status(c, layout::guidance, content.phone_connected);
  readings(c, content.vehicle, valid, content.configuration);
}

// ---------------------------------------------------------------------------
// Frame composition.
// ---------------------------------------------------------------------------
struct Fit {
  double scale, x, y;
};
Fit fit(unsigned w, unsigned h) {
  const double s = std::min(w / double(layout::width), h / double(layout::height));
  return {s, (w - layout::width * s) / 2, (h - layout::height * s) / 2};
}
void fit_canvas(cairo_t* c, unsigned width, unsigned height) {
  const auto f = fit(width, height);
  cairo_translate(c, f.x, f.y);
  cairo_scale(c, f.scale, f.scale);
  cairo_rectangle(c, 0, 0, layout::width, layout::height);
  cairo_clip(c);
}
// The backdrop never changes for a given output size: full-canvas gradients
// cost more than everything else in a frame, so draw them once per thread.
// Map's layer is transparent apart from its scrims; the others are opaque.
cairo_surface_t* backdrop_layer(unsigned width, unsigned height, bool map) {
  using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
  struct Cached {
    unsigned width = 0, height = 0;
    Surface surface{nullptr, cairo_surface_destroy};
  };
  thread_local std::array<Cached, 2> cache;
  auto& entry = cache[map];
  if (entry.surface && entry.width == width && entry.height == height) return entry.surface.get();
  Surface surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(width), int(height)),
                  cairo_surface_destroy);
  const std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context(cairo_create(surface.get()),
                                                                   cairo_destroy);
  auto* c = context.get();
  cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(c, 0, 0, 0, map ? 0 : 1);
  cairo_paint(c);
  cairo_set_operator(c, CAIRO_OPERATOR_OVER);
  fit_canvas(c, width, height);
  if (map)
    map_scrims(c);
  else
    opaque_backdrop(c);
  if (cairo_status(c) != CAIRO_STATUS_SUCCESS)
    throw std::runtime_error("native cluster backdrop failed");
  cairo_surface_flush(surface.get());
  entry = {width, height, std::move(surface)};
  return entry.surface.get();
}
void greeting(cairo_t* c, const GreetingFrame& frame) {
  if (frame.phase == GreetingPhase::inactive || frame.variant == GreetingVariant::off ||
      !std::isfinite(frame.opacity) || !std::isfinite(frame.reveal) || frame.opacity <= 0 ||
      frame.opacity > 1 || frame.reveal < 0 || frame.reveal > 1)
    return;
  cairo_save(c);
  cairo_translate(c, 874, 170);
  cairo_scale(c, .4, .4);
  if (frame.variant == GreetingVariant::brand) {
    for (const auto& points : theme::graphics::brand_polygons) {
      cairo_move_to(c, points.front().x, points.front().y);
      for (std::size_t i = 1; i < points.size(); ++i)
        cairo_line_to(c, points[i].x, points[i].y);
      cairo_close_path(c);
      cairo_set_source_rgba(c, .8, .9, 1, frame.opacity * frame.reveal);
      cairo_fill_preserve(c);
      cairo_set_source_rgba(c, .4, .82, 1, frame.opacity * (1 - frame.reveal));
      cairo_set_line_width(c, 2);
      cairo_stroke(c);
    }
  } else {
    cairo_move_to(c, 215 - 200 * frame.reveal, 152);
    cairo_line_to(c, 215 + 200 * frame.reveal, 152);
    cairo_set_source_rgba(c, .4, .82, 1, frame.opacity);
    cairo_set_line_width(c, 3);
    cairo_stroke(c);
  }
  cairo_restore(c);
}
void compose(cairo_t* c, const ClusterContent& input, Clock::time_point now, unsigned width,
             unsigned height) {
  auto content = input;
  const auto sample = current_vehicle_sample(content.vehicle, now);
  DriveModePresentation presentation;
  if (!content.configuration.drive_mode_profiles_file.empty()) {
    static thread_local std::string path;
    static thread_local std::unique_ptr<DriveModeProfilesFile> profiles;
    static thread_local Clock::time_point next_status{};
    if (path != content.configuration.drive_mode_profiles_file) {
      path = content.configuration.drive_mode_profiles_file;
      profiles = std::make_unique<DriveModeProfilesFile>(path);
      next_status = {};
    }
    presentation = profiles->poll(content.vehicle, now);
    if (now >= next_status) {
      next_status = now + std::chrono::milliseconds(250);
      publish_drive_mode_status(path + ".native.status", profiles->scope(), presentation,
                                presentation.profile ? "native-profile" : "unavailable",
                                profiles->error(), now);
    }
  }
  struct Accent {
    Tone speed = tone::speed, power = tone::power;
    ~Accent() {
      tone::speed = speed;
      tone::power = power;
    }
  } previous_accent;
  if (presentation.profile) {
    const auto& format = presentation.profile->format;
    if (format == "minimal")
      content.center = ClusterCenter::minimal;
    else if (format == "sport")
      content.center = ClusterCenter::sport;
    else if (format == "gauges")
      content.center = ClusterCenter::gauges;
    else if (format == "vehicle")
      content.center = ClusterCenter::vehicle;
    else if (format == "trip")
      content.center = ClusterCenter::trip;
    else if (format == "media")
      content.center = ClusterCenter::media;
    else if (format == "map" || format == "maps_media")
      content.center = content.phone_connected ? ClusterCenter::navigation : ClusterCenter::media;
    const auto rgb = std::stoul(presentation.profile->accent, nullptr, 16);
    tone::speed = {double((rgb >> 16) & 255) / 255., double((rgb >> 8) & 255) / 255.,
                   double(rgb & 255) / 255.};
    tone::power = tone::speed;
  }
  const bool valid =
      fresh(sample, now, std::chrono::milliseconds(500)) && valid_vehicle_sample(sample);
  const bool map = content.center == ClusterCenter::navigation;
  // Map leaves letterbox and center transparent for the map surface beneath;
  // every other page is opaque, so nothing behind it can show through.
  cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(c, backdrop_layer(width, height, map), 0, 0);
  cairo_paint(c);
  cairo_set_operator(c, CAIRO_OPERATOR_OVER);
  auto* options = cairo_font_options_create();
  cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
  cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
  cairo_set_font_options(c, options);
  cairo_font_options_destroy(options);
  fit_canvas(c, width, height);
  if (presentation.profile && !map && presentation.profile->wallpaper != "phone") {
    source(c, {0, 0, 0});
    cairo_paint(c);
    if (presentation.profile->wallpaper == "blue")
      glow(c, 960, 360, 1000, {.15, .35, .70, .35});
    else if (presentation.profile->wallpaper == "red")
      glow(c, 960, 360, 1000, {.70, .15, .10, .35});
  }
  const auto media = load_media(content.media_file, now);
  if (content.center == ClusterCenter::media)
    media_page(c, media, content.phone_connected);
  else if (content.center == ClusterCenter::vehicle)
    vehicle_page(c, sample, valid, content.configuration);
  else if (content.center == ClusterCenter::trip)
    trip_page(c, sample, content.configuration);
  greeting(c, content.greeting);
  if (content.center == ClusterCenter::minimal)
    minimal_format(c, sample, valid, content.configuration);
  else if (content.center == ClusterCenter::sport)
    sport_format(c, sample, valid, content.configuration);
  else {
    if (content.center == ClusterCenter::gauges)
      gauges_format(c, sample, valid, content.configuration);
    speed_instrument(c, sample, valid, content.configuration);
    power_instrument(c, sample, valid, content.configuration);
  }
  if (presentation.profile) {
    pill(c, {1440, 24, 440, 40},
         presentation.source == "user"         ? "DISPLAY PROFILE"
         : presentation.source == "simulation" ? "SIMULATED MODE"
                                               : "OBSERVED MODE",
         presentation.profile->name);
  }
  telltales(c, sample, now, content.turn_signals);
  data_status(c, sample, valid);
  bottom_bar(c,
             {sample, content.media_file, content.navigation, content.center,
              content.phone_connected, content.configuration},
             media, valid, map);
}
}  // namespace

ClusterCenter selected_cluster_center(const std::string& path, bool map_available) {
  if (path.empty()) return map_available ? ClusterCenter::navigation : ClusterCenter::media;
  const auto choice = state_file(path, 16);
  const auto trimmed = choice.substr(0, choice.find_first_of("\r\n"));
  if (trimmed == "minimal") return ClusterCenter::minimal;
  if (trimmed == "sport") return ClusterCenter::sport;
  if (trimmed == "gauges") return ClusterCenter::gauges;
  if (trimmed == "vehicle") return ClusterCenter::vehicle;
  if (trimmed == "trip") return ClusterCenter::trip;
  if (trimmed == "media") return ClusterCenter::media;
  if (trimmed == "map" && map_available) return ClusterCenter::navigation;
  return map_available ? ClusterCenter::navigation : ClusterCenter::media;
}
ClusterMapRect native_cluster_map(unsigned width, unsigned height) {
  const auto f = fit(width, height);
  return {int(std::lround(f.x)), int(std::lround(f.y)), int(std::lround(layout::width * f.scale)),
          int(std::lround(layout::height * f.scale))};
}
void draw_native_cluster(const ClusterContent& content, Clock::time_point now,
                         std::uint32_t* pixels, unsigned width, unsigned height,
                         std::size_t stride) {
  if (!valid_native_cluster_configuration(content.configuration))
    throw std::invalid_argument("invalid native cluster gauge configuration");
  if (width < 640 || width > 4096 || height < 240 || height > 2160)
    throw std::invalid_argument("native cluster size outside bounds");
  if (!pixels || stride < std::size_t(width) * 4 || stride % 4 || stride > std::size_t(width) * 16)
    throw std::invalid_argument("invalid native cluster frame");
  const std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(
      cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(pixels),
                                          CAIRO_FORMAT_ARGB32, int(width), int(height),
                                          int(stride)),
      cairo_surface_destroy);
  const std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context(cairo_create(surface.get()),
                                                                   cairo_destroy);
  if (cairo_status(context.get()) != CAIRO_STATUS_SUCCESS)
    throw std::runtime_error("native cluster context failed");
  compose(context.get(), content, now, width, height);
  if (cairo_status(context.get()) != CAIRO_STATUS_SUCCESS)
    throw std::runtime_error("native cluster drawing failed");
  cairo_surface_flush(surface.get());
}
Image render_native_cluster(const VehicleSample& sample, Clock::time_point now, unsigned width,
                            unsigned height, const std::string& media_file,
                            const NavigationSnapshot& navigation, ClusterCenter center,
                            bool phone_connected, const NativeClusterConfiguration& configuration) {
  std::vector<std::uint32_t> frame(std::size_t(width) * height);
  draw_native_cluster({sample, media_file, navigation, center, phone_connected, configuration}, now,
                      frame.data(), width, height, std::size_t(width) * 4);
  // Premultiplied pixels over black are exactly their color channels.
  Image result{width, height, std::vector<Rgb>(frame.size())};
  for (std::size_t i = 0; i < frame.size(); ++i)
    result.pixels[i] = {std::uint8_t(frame[i] >> 16), std::uint8_t(frame[i] >> 8),
                        std::uint8_t(frame[i])};
  return result;
}
}  // namespace headunit
