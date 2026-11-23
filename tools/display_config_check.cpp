// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/display_configuration.hpp>
#include <iostream>
int main(int argc, char** argv) try {
  bool ultra = false, json = false;
  if (argc < 2 || argc > 4)
    throw std::invalid_argument(
        "usage: headunit-display-config-check DISPLAYS.conf [--ultra] [--json]");
  for (int i = 2; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--ultra" && !ultra)
      ultra = true;
    else if (std::string_view(argv[i]) == "--json" && !json)
      json = true;
    else
      throw std::invalid_argument("invalid or duplicate display check option");
  }
  const auto displays = headunit::load_display_configuration(argv[1]);
  for (const auto& d : displays)
    if (ultra && (!d.size.width_mm || !d.size.height_mm))
      throw std::invalid_argument("Ultra requires positive physical dimensions for both panels");
  if (json) {
    std::cout << '[';
    for (std::size_t i = 0; i < displays.size(); ++i) {
      if (i) std::cout << ',';
      const auto& d = displays[i];
      // These names contain only parser-validated ASCII identifier characters.
      std::cout << "{\"width\":" << d.size.width << ",\"height\":" << d.size.height
                << ",\"wayland\":\"" << d.wayland_display << "\",\"output\":\"" << d.output_name
                << "\"}";
    }
    std::cout << "]\n";
  } else
    for (const auto& d : displays)
      std::cout << d.panel_name << ' ' << d.size.width << 'x' << d.size.height << '@'
                << d.size.max_fps << '\n';
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
