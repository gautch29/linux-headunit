// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/warning_overlay.hpp>
#include <headunit/indicator_symbols.hpp>
#include <headunit/native_cluster.hpp>
#include <headunit/vehicle_observation.hpp>
#include <headunit/theme/scene_viewport.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <filesystem>
#include "allocation_counter.hpp"
#include <new>

using namespace headunit;
namespace g = headunit::theme::graphics;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F f) {
  bool bad = false;
  try {
    f();
  } catch (const std::invalid_argument&) {
    bad = true;
  }
  check(bad, "bad overlay configuration accepted");
}
Clock::time_point at(int ms) {
  return Clock::time_point{std::chrono::milliseconds(ms)};
}
g::Image pixels(const std::shared_ptr<const g::Layer>& scene) {
  return g::render(std::const_pointer_cast<g::Layer>(scene), 1920, 720);
}
int main(int argc, char** argv) try {
  check(argc <= 3, "usage: warning-overlay-test [PREVIEW.png] [TURN_REVIEW_DIRECTORY]");
  std::ifstream input(std::string(HEADUNIT_SOURCE_DIR) + "/config/ultra-warnings.example.json");
  std::stringstream text;
  text << input.rdbuf();
  const auto config = g::decode_warning_overlay_configuration(text.str());
  check(config.hide_during_startup, "example does not hide startup lights");
  auto legacy = text.str();
  legacy.erase(legacy.find("  \"hideDuringStartup\": true,\n"),
               std::string("  \"hideDuringStartup\": true,\n").size());
  check(g::decode_warning_overlay_configuration(legacy).hide_during_startup,
        "legacy profile startup default is not clean");
  auto explicit_show = text.str();
  explicit_show.replace(explicit_show.find("\"hideDuringStartup\": true"),
                        std::string("\"hideDuringStartup\": true").size(),
                        "\"hideDuringStartup\": false");
  check(!g::decode_warning_overlay_configuration(explicit_show).hide_during_startup,
        "explicit startup show flag lost");
  for (const auto value : {"0", "\"true\"", "null"}) {
    auto invalid = text.str();
    invalid.replace(invalid.find("\"hideDuringStartup\": true"),
                    std::string("\"hideDuringStartup\": true").size(),
                    std::string("\"hideDuringStartup\": ") + value);
    rejects([&] { g::decode_warning_overlay_configuration(invalid); });
  }
  {
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 80, 60);
    auto* c = cairo_create(surface);
    cairo_translate(c, 3, 4);
    cairo_set_line_width(c, 9);
    cairo_matrix_t before{}, after{};
    cairo_get_matrix(c, &before);
    draw_indicator_symbol(c, Indicator::abs, 2, 2, 64, 40);
    cairo_set_source_rgba(c, .1, .2, .3, .4);
    auto* before_options = cairo_font_options_create();
    auto* after_options = cairo_font_options_create();
    cairo_get_font_options(c, before_options);
    draw_indicator_telltale(c, indicator_definitions[static_cast<std::size_t>(Indicator::abs)], 0,
                            0, 72, 56);
    cairo_get_font_options(c, after_options);
    check(cairo_font_options_equal(before_options, after_options),
          "telltale changed caller font options");
    cairo_font_options_destroy(before_options);
    cairo_font_options_destroy(after_options);
    double red = 0, green = 0, blue = 0, alpha = 0;
    check(cairo_pattern_get_rgba(cairo_get_source(c), &red, &green, &blue, &alpha) ==
                  CAIRO_STATUS_SUCCESS &&
              red == .1 && green == .2 && blue == .3 && alpha == .4,
          "telltale changed caller colour");
    cairo_get_matrix(c, &after);
    check(before.xx == after.xx && before.xy == after.xy && before.x0 == after.x0 &&
              before.yx == after.yx && before.yy == after.yy && before.y0 == after.y0 &&
              cairo_get_line_width(c) == 9,
          "symbol changed caller graphics state");
    rejects([&] { draw_indicator_symbol(c, Indicator::count, 0, 0, 64, 40); });
    rejects([&] { draw_indicator_symbol(c, Indicator::low_beam, 0, 0, 0, 40); });
    rejects([&] {
      draw_indicator_symbol(c, Indicator::low_beam, 0, 0, std::numeric_limits<double>::quiet_NaN(),
                            40);
    });
    rejects([&] { draw_turn_signal_lamp(nullptr, Indicator::left_turn, 0, 0, 64, 56); });
    rejects([&] { draw_turn_signal_lamp(c, Indicator::hazard, 0, 0, 64, 56); });
    rejects([&] { draw_turn_signal_lamp(c, Indicator::left_turn, 0, 0, 64, 56, 1.1); });
    rejects([&] { draw_turn_signal_unavailable(c, Indicator::count, 0, 0, 64, 56); });
    cairo_destroy(c);
    cairo_surface_destroy(surface);
  }
  check(config.indicators.size() == 24, "example omits a vehicle indicator");
  check(config.canvas.width == cluster_layout::width &&
            config.canvas.height == cluster_layout::height,
        "Ultra and native indicator canvases differ");
  check(cluster_layout::bottom_bar.y + cluster_layout::bottom_bar.height <
                cluster_layout::telltales.y &&
            cluster_layout::center.y + cluster_layout::center.height < cluster_layout::bottom_bar.y,
        "native content overlaps the reserved telltale row");
  for (const auto& placement : config.indicators) {
    const auto frame = cluster_layout::indicator_frame(placement.signal);
    check(placement.frame.origin.x == frame.x && placement.frame.origin.y == frame.y &&
              placement.frame.size.width == frame.width &&
              placement.frame.size.height == frame.height,
          "Ultra and native telltale positions or sizes differ");
    const bool direction =
        placement.signal == Indicator::left_turn || placement.signal == Indicator::right_turn;
    check(frame.y == (direction ? 16 : 648) && frame.y + frame.height <= cluster_layout::height,
          "direction arrows or bottom warnings have the wrong height");
    check(direction || frame.x + frame.width <= 860 || frame.x >= 1060,
          "lamp test obscures the signed cluster odometer");
  }
  rejects([&] { g::decode_warning_overlay_configuration(text.str() + "{}"); });
  rejects([&] {
    g::decode_warning_overlay_configuration("{\"schemaVersion\":1,\"schemaVersion\":1}");
  });
  rejects([&] {
    auto bad = config;
    bad.indicators.push_back(bad.indicators[0]);
    g::validate_warning_overlay_configuration(bad);
  });
  rejects([&] {
    auto bad = config;
    bad.indicators[0].frame.origin.y = 719;
    g::validate_warning_overlay_configuration(bad);
  });
  rejects([&] {
    auto bad = config;
    bad.indicators[0].frame = config.indicators[1].frame;
    g::validate_warning_overlay_configuration(bad);
  });
  rejects([&] {
    auto bad = config;
    bad.indicators[0].signal = Indicator::count;
    g::validate_warning_overlay_configuration(bad);
  });
  rejects([&] {
    auto bad = config;
    bad.canvas.width = std::numeric_limits<double>::infinity();
    g::validate_warning_overlay_configuration(bad);
  });
  rejects([&] {
    auto bad = config;
    bad.maximum_age = std::chrono::milliseconds(2001);
    g::validate_warning_overlay_configuration(bad);
  });
  for (const auto replacement : {"true", "1.0", "2"}) {
    auto bad = text.str();
    const auto pos = bad.find("\"schemaVersion\": 1");
    bad.replace(pos, 18, std::string("\"schemaVersion\": ") + replacement);
    rejects([&] { g::decode_warning_overlay_configuration(bad); });
  }
  // Each retained tree is immutable, including during lamp fades.
  VehicleSample sample;
  sample.sampled_at = at(2000);
  sample.indicators.fill(IndicatorState::off);
  for (const auto& definition : indicator_definitions) {
    if (definition.id == Indicator::hazard) continue;
    g::WarningOverlay overlay(config);
    auto active = sample;
    active.indicator(definition.id) = IndicatorState::on;
    overlay.scene(active, at(2000), config.canvas);
    const auto scene = overlay.scene(active, at(2040), config.canvas);
    check(scene && scene->children.size() == 1, "on lamp missing or other lights leaked");
    check(scene == overlay.scene(active, at(2041), config.canvas), "settled lamp rebuilt scene");
    const auto& node = scene->children[0];
    const auto frame = cluster_layout::indicator_frame(definition.id);
    check(node->frame().origin.x == frame.x && node->image, "slot moved or image lost");
    check(node->image->pixels.front() == 0 && node->image->pixels.back() == 0,
          "lamp acquired a card background");
    const auto native_off = render_native_cluster(sample, at(2040), 1920, 720);
    const auto native_on = render_native_cluster(active, at(2040), 1920, 720);
    auto background = std::make_shared<g::Layer>(g::Kind::image);
    background->set_frame({{0, 0}, config.canvas});
    background->image = std::make_shared<g::Image>();
    background->image->width = 1920;
    background->image->height = 720;
    for (const auto pixel : native_off.pixels)
      background->image->pixels.push_back(0xff000000u | (unsigned(pixel.r) << 16) |
                                          (unsigned(pixel.g) << 8) | pixel.b);
    const auto ultra_on = pixels(g::with_scene_overlay(background, scene));
    std::uint64_t error = 0;
    std::size_t changed = 0;
    for (int y = frame.y; y < frame.y + frame.height; ++y) {
      for (int x = frame.x; x < frame.x + frame.width; ++x) {
        const auto i = std::size_t(y) * 1920 + x;
        const auto n = native_on.pixels[i];
        const auto u = ultra_on.pixels[i];
        if (n == native_off.pixels[i] && u == background->image->pixels[i]) continue;
        error += std::abs(int(n.r) - int((u >> 16) & 255)) +
                 std::abs(int(n.g) - int((u >> 8) & 255)) + std::abs(int(n.b) - int(u & 255));
        ++changed;
      }
    }
    check(changed > 60 && double(error) / (changed * 3) < 10, "native/Ultra symbol mismatch");
    active.indicator(definition.id) = IndicatorState::off;
    overlay.scene(active, at(2050), config.canvas);
    check(!overlay.scene(active, at(2170), config.canvas), "settled off lamp retained pixels");
    check(scene->children.size() == 1 && scene->children[0] == node, "fade mutated held tree");
  }
  {
    SimulatedVehicle simulated(at(2000));
    const auto demonstration = simulated.sample(at(8000));
    check(demonstration.simulated &&
              demonstration.indicator(Indicator::hazard) == IndicatorState::flashing &&
              demonstration.indicator(Indicator::left_turn) == IndicatorState::flashing &&
              demonstration.indicator(Indicator::right_turn) == IndicatorState::flashing,
          "hazard simulator contradicts flashing with explicit off lamps");
  }
  TurnSignalAnimation animation;
  auto active = sample;
  active.indicator(Indicator::left_turn) = IndicatorState::on;
  check(animation.update(active, at(2000)).intensity[0] == 0, "rise hard popped");
  const auto rising = animation.update(active, at(2020));
  check(rising.intensity[0] == .5 && rising.animating, "40 ms rise easing incorrect");
  check(animation.update(active, at(2040)).intensity[0] == 1, "rise did not settle");
  active.indicator(Indicator::left_turn) = IndicatorState::off;
  check(animation.update(active, at(2050)).intensity[0] == 1, "decay hard popped");
  check(animation.update(active, at(2110)).intensity[0] == .5, "120 ms decay easing incorrect");
  const auto dark = animation.update(active, at(2170));
  check(dark.intensity[0] == 0 && !dark.animating, "settled off keeps animating");
  active.indicator(Indicator::left_turn) = IndicatorState::on;
  animation.update(active, at(2180));
  check(animation.update(active, at(2220)).intensity[0] == 1, "observed on follows fallback phase");
  const auto expired = animation.update(active, at(2501));
  check(expired.unavailable[0] && expired.intensity[0] == 0 && !expired.animating,
        "stale lamp decayed as if known off");
  check(active.sampled_at == at(2000), "animation renewed acquisition");
  active.indicator(Indicator::left_turn).reset();
  check(animation.update(active, at(2000)).unavailable[0], "unknown became known off");
  check(indicator_flash_visible(at(2333)) && !indicator_flash_visible(at(2334)) &&
            !indicator_flash_visible(at(2666)) && indicator_flash_visible(at(2667)) &&
            indicator_flash_visible(at(4000)),
        "90/min shared clock drifted");
  auto hazard = sample;
  hazard.indicators.fill(std::nullopt);
  hazard.indicator(Indicator::hazard) = IndicatorState::on;
  animation.reset();
  check(animation.update(hazard, at(2000)).intensity[0] == 0, "initial fallback hard popped");
  const auto paired = animation.update(hazard, at(2020));
  check(paired.intensity[0] == paired.intensity[1] && paired.intensity[0] > .49 &&
            paired.intensity[0] < .51 && paired.unavailable.none(),
        "hazard phase/rise differs");
  const auto decay = animation.update(hazard, at(2394));
  check(decay.intensity[0] == decay.intensity[1] && decay.intensity[0] > .48 &&
            decay.intensity[0] < .51,
        "hazard decay differs");
  TurnSignalAnimation first_dark;
  check(first_dark.update(hazard, at(2394)).intensity[0] == 0,
        "first dark fallback invented prior lamp energy");
  hazard.indicator(Indicator::left_turn) = IndicatorState::off;
  const auto observed_off = cluster_indicator_snapshot(hazard, at(2020));
  check(observed_off.visible.none() && observed_off.known.count() == 2 &&
            !observed_off.known[std::size_t(Indicator::right_turn)],
        "hazard summary overrode observed off or fabricated peer reading");
  hazard.indicator(Indicator::left_turn) = IndicatorState::on;
  check(cluster_indicator_snapshot(hazard, at(2400)).visible.count() == 2,
        "hazard summary overrode observed on");
  {
    VehicleAgePolicy ages;
    ages.indicators[std::size_t(Indicator::hazard)] = std::chrono::milliseconds(100);
    ages.indicators[std::size_t(Indicator::left_turn)] = std::chrono::seconds(2);
    VehicleObservationCache cache(ages);
    cache.begin_generation(1, at(2000), at(2000));
    const std::array observations{VehicleObservation{"hazard", IndicatorState::on, at(2000)},
                                  VehicleObservation{"left_turn", IndicatorState::on, at(2000)}};
    cache.accept(1, 1, observations, at(2000));
    const auto dated = cache.sample(at(2000));
    animation.reset();
    animation.update(dated, at(2000));
    const auto after_hazard = animation.update(dated, at(2100));
    check(after_hazard.intensity[0] == 1 && after_hazard.unavailable[1] &&
              after_hazard.intensity[1] == 0,
          "per-signal hazard expiry retained a lamp");
    check(animation.update(dated, at(4000)).unavailable.all(), "direction expiry revived lamp");
    check(dated.observation_timing->indicators[std::size_t(Indicator::hazard)].acquired_at ==
              at(2000),
          "animation changed original per-signal date");
    cache.begin_generation(2, at(4100), at(4100));
    cache.accept(2, 1, std::array{VehicleObservation{"left_turn", IndicatorState::on, at(4100)}},
                 at(4100));
    check(animation.update(cache.sample(at(4100)), at(4100)).intensity[0] == 0,
          "new source generation reused old brightness");
  }
  g::WarningOverlay overlay(config);
  active = sample;
  active.indicator(Indicator::left_turn) = IndicatorState::on;
  check(!overlay.scene(active, at(2000), config.canvas), "initial rise hard popped");
  const auto mid = overlay.scene(active, at(2020), config.canvas);
  const auto lit = overlay.scene(active, at(2040), config.canvas);
  check(mid && lit && mid != lit && mid->children[0]->image != lit->children[0]->image,
        "fade did not select distinct cached brightness tiles");
  active.indicator(Indicator::left_turn) = IndicatorState::off;
  overlay.scene(active, at(2050), config.canvas);
  check(overlay.scene(active, at(2110), config.canvas) == mid, "mid-fade rebuilt retained scene");
  check(!overlay.scene(active, at(2170), config.canvas), "off scene not empty");
  active.indicator(Indicator::left_turn) = IndicatorState::on;
  overlay.scene(active, at(2180), config.canvas);
  check(overlay.scene(active, at(2200), config.canvas) == mid, "second rise allocated scene");
  check(pixels(lit).pixels != pixels(mid).pixels, "fade intensity has no pixel effect");
  {
    auto fixture = sample;
    fixture.indicator(Indicator::left_turn) = IndicatorState::on;
    std::vector<std::uint32_t> frame(1920 * 720);
    draw_native_cluster({fixture, {}, {}, ClusterCenter::sport, false, {}, {}, &rising}, at(2020),
                        frame.data(), 1920, 720, 1920 * 4);
    const auto full =
        render_native_cluster(fixture, at(2020), 1920, 720, {}, {}, ClusterCenter::sport);
    const auto region = cluster_layout::indicator_frame(Indicator::left_turn);
    const auto i = std::size_t(region.y + region.height / 2) * 1920 + region.x + 42;
    check(((frame[i] >> 8) & 255) > 70 && ((frame[i] >> 8) & 255) < full.pixels[i].g,
          "native owner ignored animation intensity");
    draw_native_cluster({fixture, {}, {}, ClusterCenter::sport, false, {}, {}, &rising}, at(2501),
                        frame.data(), 1920, 720, 1920 * 4);
    const auto expired_native =
        render_native_cluster(fixture, at(2501), 1920, 720, {}, {}, ClusterCenter::sport);
    for (int y = region.y; y < region.y + region.height; ++y)
      for (int x = region.x; x < region.x + region.width; ++x) {
        const auto index = std::size_t(y) * 1920 + x;
        check(((frame[index] >> 8) & 255) == expired_native.pixels[index].g,
              "supplied animation revived stale native lamp");
      }
  }
  test_allocations::begin();
  auto* probe = ::operator new(16);
  check(test_allocations::end() == 1, "allocation counter did not observe allocation");
  ::operator delete(probe);
  test_allocations::begin();
  for (int tick = 0; tick < 100; ++tick) {
    active.sampled_at = at(2200 + tick);
    overlay.scene(active, at(2200 + tick), config.canvas);
  }
  check(test_allocations::end() == 0, "animation tick allocated C++ heap storage");
  active.sampled_at = at(2000);
  active.indicator(Indicator::left_turn).reset();
  const auto unknown = overlay.scene(active, at(2220), config.canvas);
  check(unknown && unknown->children.size() == 1 && pixels(unknown).pixels != pixels(lit).pixels,
        "unknown arrow missing or filled");
  for (auto pixel : unknown->children[0]->image->pixels)
    check(((pixel >> 8) & 255) < 65 && ((pixel >> 24) & 255) < 100, "unknown resembles lit green");
  check(overlay.scene(active, at(2501), config.canvas)->children.size() == 2,
        "stale sample lost dashed outlines");
  active = sample;
  active.indicators.fill(IndicatorState::on);
  active.simulated = true;
  overlay.scene(active, at(2000), config.canvas);
  const auto all = overlay.scene(active, at(2040), config.canvas);
  check(all && all->children.size() == 24, "all lamps/simulation badge missing");
  check(!overlay.scene(active, at(2041), config.canvas, true), "startup Hide leaked lamps");
  check(all->children.size() == 24, "startup hiding mutated held scene");
  overlay.set_hide_during_startup(false);
  overlay.scene(active, at(2042), config.canvas, true);
  check(overlay.scene(active, at(2082), config.canvas, true)->children.size() == 24,
        "startup Show did not restore lamps");
  const auto resized = overlay.scene(active, at(2083), {960, 360});
  check(resized == overlay.scene(active, at(2084), {960, 360}), "resized scene not cached");
  check(resized->transform.a == .5 && resized->transform.d == .5, "output mapping incorrect");
  rejects([&] { overlay.scene(active, at(2084), {0, 360}); });
  auto bad_sample = active;
  bad_sample.sampled_at = at(2100);
  const auto future = overlay.scene(bad_sample, at(2090), config.canvas);
  check(future && future->children.size() == 2, "future source retained lamps or badge");
  bad_sample.sampled_at = at(2000);
  bad_sample.speed_kmh = 301;
  const auto invalid = overlay.scene(bad_sample, at(2090), config.canvas);
  check(invalid && invalid->children.size() == 2, "invalid source retained lamps or badge");
  auto disabled = config;
  disabled.enabled = false;
  g::WarningOverlay off(disabled);
  check(!off.scene(active, at(2040), config.canvas), "disabled overlay rendered");
  auto subset = config;
  subset.indicators.resize(1);
  g::WarningOverlay limited(subset);
  limited.scene(active, at(2000), config.canvas);
  check(limited.scene(active, at(2040), config.canvas)->children.size() == 2,
        "subset leaked lamps");
  auto asset = std::make_shared<g::Layer>();
  asset->set_frame({{0, 0}, config.canvas});
  asset->opacity = 0;
  asset->clip = true;
  auto child = std::make_shared<g::Layer>();
  asset->insert(child, 0);
  check(pixels(g::with_scene_overlay(asset, all)).pixels == pixels(all).pixels,
        "signed scene opacity/clip hid overlay");
  check(child->parent.lock() == asset && all->parent.expired(), "composition changed parent links");
  check(g::with_scene_overlay(asset, {}) == asset, "absent overlay changed asset");
  if (argc >= 2) g::write_png(pixels(all), argv[1]);
  if (argc == 3) {
    std::filesystem::create_directories(argv[2]);
    for (const auto* choice : {"lit", "mid-fade", "off", "hazard", "unknown"}) {
      g::WarningOverlay review(config);
      auto fixture = sample;
      review.scene(fixture, at(2000), config.canvas);
      fixture.indicator(Indicator::left_turn) = IndicatorState::on;
      if (std::string_view(choice) == "hazard") {
        fixture.indicator(Indicator::right_turn) = IndicatorState::on;
        fixture.indicator(Indicator::hazard) = IndicatorState::on;
      }
      review.scene(fixture, at(2010), config.canvas);
      auto scene = review.scene(fixture, at(2050), config.canvas);
      if (std::string_view(choice) == "mid-fade" || std::string_view(choice) == "off") {
        fixture.indicator(Indicator::left_turn) = IndicatorState::off;
        review.scene(fixture, at(2060), config.canvas);
        scene = review.scene(fixture, at(std::string_view(choice) == "off" ? 2180 : 2120),
                             config.canvas);
      }
      if (std::string_view(choice) == "unknown") {
        fixture.indicator(Indicator::left_turn).reset();
        fixture.indicator(Indicator::right_turn).reset();
        scene = review.scene(fixture, at(2060), config.canvas);
      }
      g::write_png(pixels(scene),
                   (std::filesystem::path(argv[2]) / (std::string(choice) + ".png")).string());
    }
  }
  std::cout << "Shared ISO-style arrows, easing/bloom, observation priority, no-data outlines, "
               "cadence, freshness, immutable bounded animation cache and configuration passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
