// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/display_configuration.hpp>
#include <iostream>
#include <stdexcept>
using namespace headunit;
void check(bool ok) {
  if (!ok) throw std::runtime_error("display configuration check failed");
}
int main() try {
  const std::string input =
      "main.width=1280\nmain.height=720\nmain.fps=60\nmain.panel=Main\nmain.wayland=headunit-"
      "0\ncluster.width=1920\ncluster.height=720\ncluster.fps=30\ncluster.panel=Cluster\n";
  const auto d = parse_display_configuration(input);
  check(d[0].size.width == 1280 && d[1].size.width == 1920 && d[1].size.max_fps == 30);
  check(d[0].panel_name == "Main" && d[0].wayland_display == "headunit-0" &&
        d[1].wayland_display.empty());
  check(d[0].size.width_mm == 0 && d[0].size.height_mm == 0);
  const auto measured =
      parse_display_configuration(input + "main.width_mm=155\nmain.height_mm=87\n");
  check(measured[0].size.width_mm == 155 && measured[0].size.height_mm == 87);
  auto reject = [](const std::string& text) {
    bool rejected = false;
    try {
      (void)parse_display_configuration(text);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected);
  };
  reject("");
  reject(input + "main.width=1920\n");
  reject(input + "main.width_mm=155\n");
  reject(input + "unknown=1\n");
  reject(input + "main.asset_width=1920\n");
  check(parse_display_configuration(input + "main.asset_width=1920\nmain.asset_height=720\n")[0]
            .asset_width == 1920);
  const auto routed =
      parse_display_configuration(input + "main.output=HDMI-A-1\ncluster.output=HDMI-A-2\n");
  check(routed[0].output_name == "HDMI-A-1" && routed[1].output_name == "HDMI-A-2");
  reject(input + "main.output=../../wrong\n");
  reject(input + "cluster.wayland=../../socket\n");
  reject(input + "main.height_mm=-1\n");
  for (const auto& [from, to] : {std::pair{"1280", "8192"},
                                 {"fps=60", "fps=0"},
                                 {"fps=60", "fps=60x"},
                                 {"panel=Cluster", "panel=Main"}}) {
    auto bad = input;
    bad.replace(bad.find(from), std::string(from).size(), to);
    reject(bad);
  }
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
