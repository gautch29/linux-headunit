// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/local_cluster_selection.hpp>
#include <stdexcept>

namespace headunit::theme::script {
namespace {
bool uuid(std::string_view text) {
  if (text.size() != 36) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = text[i];
    if ((i == 8 || i == 13 || i == 18 || i == 23)
            ? c != '-'
            : !(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'F'))
      return false;
  }
  return true;
}
bool palette(std::string_view text) {
  return !text.empty() && text.size() <= 64 &&
         text.find_first_not_of(
             "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") == text.npos;
}
}  // namespace
LocalClusterSelection local_cluster_selection(ResourceReader reader, std::string configured,
                                              std::optional<std::string_view> saved) {
  if (!configured.empty() && !uuid(configured))
    throw std::invalid_argument("invalid configured local cluster layout");
  std::string candidate, colour;
  if (saved && saved->size() >= 37 && saved->size() <= 102 && (*saved)[36] == '\n' &&
      saved->back() == '\n' && uuid(saved->substr(0, 36))) {
    if (saved->size() == 37) {
      candidate = saved->substr(0, 36);
    } else if (saved->size() >= 39 && palette(saved->substr(37, saved->size() - 38))) {
      candidate = saved->substr(0, 36);
      colour = saved->substr(37, saved->size() - 38);
    }
  }
  LocalClusterSelection selection;
  Runtime metadata(std::move(reader), {"Cluster_Display", 1920, 720});
  metadata.bind_log_sink([&](int, std::string_view tag, std::string_view value) {
    if (tag == "localLayout") selection.layout = value;
    if (tag == "localPalette") selection.palette = value;
  });
  // Only immutable metadata modules execute here. A rejected cached selection
  // must not fault or start tasks in the VM that will own the display.
  metadata.evaluate("local configured='" + configured + "'; local saved='" + candidate +
                        "'; local colour='" + colour + R"(';
      local layouts = {}
      local displays = require('r.PackageData').displays
      assert(type(displays)=='table' and #displays<=8, 'invalid local displays')
      for _, display in ipairs(displays) do
        if display.id=='Cluster_Display' then
          assert(type(display.layouts)=='table' and #display.layouts<=64,
                 'invalid local layout catalogue')
          for _, layout in ipairs(display.layouts) do
            if layout.layoutType=='primary' then layouts[layout.id]=true end
          end
        end
      end
      assert(configured=='' or layouts[configured], 'configured local layout is not declared')
      local selected = layouts[saved] and saved or configured
      if selected~='' then CP.log(2, 'localLayout', selected) end
      if selected==saved and colour~='' then
        local p=require('r.ThemeData').palettes[colour]
        if type(p)=='table' and p.id==colour then CP.log(2, 'localPalette', colour) end
      end
    )",
                    "local-cluster-preferences");
  return selection;
}
}  // namespace headunit::theme::script
