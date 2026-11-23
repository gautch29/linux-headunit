// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/asset_bundle.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
std::uint64_t integer(const std::string& text) {
  std::uint64_t result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size())
    throw std::invalid_argument("invalid integer argument");
  return result;
}

}  // namespace
int main(int argc, char** argv) {
  const bool allow_expired = argc > 1 && std::string_view(argv[argc - 1]) == "--allow-expired-leaf";
  if (allow_expired) --argc;
  if (argc != 7 && argc != 8) {
    std::cerr << "usage: headunit-asset-inspect ARCHIVE SIGNATURE CHAIN ROOT ASSET_ID VERSION "
                 "[LOCAL_ACCEPTANCE_REFERENCE] [--allow-expired-leaf]\n";
    return 1;
  }
  try {
    headunit::AssetBundlePolicy policy{argv[5], integer(argv[6]), {}};
    if (allow_expired)
      policy.certificate_time_policy = headunit::AssetCertificateTimePolicy::allow_expired_leaf;
    if (argc == 8) policy.previously_accepted = headunit::read_asset_reference(argv[7]);
    const auto bundle = headunit::AssetBundle::open(argv[1], argv[2], argv[3], argv[4], policy);
    const auto& info = bundle.info();
    std::cout << "Verified bundle: version=" << info.version << " files=" << info.files
              << " expanded_bytes=" << info.expanded_bytes
              << " expired_signer_exception=" << (info.expired_signer_exception ? "true" : "false")
              << '\n';
    std::cout << "main.lua verified resource bytes=" << bundle.resource("main.lua").size() << '\n';
    std::cout
        << "Read-only inspection; no installation, script execution, or SDK compatibility claim.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}
