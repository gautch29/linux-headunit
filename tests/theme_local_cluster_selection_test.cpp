// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/local_cluster_selection.hpp>
#include <iostream>
#include <stdexcept>

using namespace headunit::theme::script;
namespace {
constexpr auto first = "00000000-1111-2222-3333-444444444444";
constexpr auto second = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
constexpr auto missing = "11111111-2222-3333-4444-555555555555";
void check(bool value) {
  if (!value) throw std::runtime_error("local cluster selection check failed");
}
ResourceReader resources() {
  return [](std::string_view path) -> std::string {
    if (path == "r/PackageData.lua")
      return R"(return {displays={{id='Cluster_Display',layouts={
        {id='00000000-1111-2222-3333-444444444444',layoutType='primary'},
        {id='AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE',layoutType='primary'},
        {id='11111111-2222-3333-4444-555555555555',layoutType='overlay'}
      }},{id='Center_Display',layouts={
        {id='11111111-2222-3333-4444-555555555555',layoutType='primary'}
      }}}})";
    if (path == "r/ThemeData.lua")
      return R"(return {palettes={Ocean={id='Ocean'},Wrong={id='Other'}}})";
    throw std::runtime_error("unexpected metadata resource");
  };
}
LocalClusterSelection select(std::optional<std::string_view> saved) {
  return local_cluster_selection(resources(), first, saved);
}
void rejected_caches() {
  for (const auto& text :
       {std::string(missing) + '\n', std::string("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF\n"),
        std::string(first) + '\n' + std::string(65, 'A') + '\n', std::string(first) + "\n\n",
        std::string(first) + "\nOcean", std::string(first) + "\nO'cean\n", std::string(103, 'x'),
        std::string("not-a-layout\n")}) {
    const auto value = select(text);
    check(value.layout == first && value.palette.empty());
  }
  check(select(std::string(missing) + "\nOcean\n").palette.empty());
  const auto unknown_colour = select(std::string(second) + "\nMissing\n");
  check(unknown_colour.layout == second && unknown_colour.palette.empty());
  check(select(std::string(second) + "\nWrong\n").palette.empty());
}
}  // namespace
int main() {
  try {
    check(select(std::nullopt).layout == first);
    check(select(std::string(second) + '\n').layout == second);
    const auto remembered = select(std::string(second) + "\nOcean\n");
    check(remembered.layout == second && remembered.palette == "Ocean");
    rejected_caches();
    const auto asset_default = local_cluster_selection(resources(), {}, std::nullopt);
    check(asset_default.layout.empty() && asset_default.palette.empty());
    bool rejected = false;
    try {
      (void)local_cluster_selection(resources(), missing, std::nullopt);
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected);
    std::cout << "local cluster cache admission and configured fallback passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
