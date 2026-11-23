// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/airplay_identity.hpp>
#include <headunit/iap2_wifi_handoff.hpp>
namespace headunit {
struct Iap2ThemeAsset {
  std::string identifier, sdk_version;
  std::uint32_t version = 0;
};
struct Iap2CarplayStart {
  AirplayIdentity identity;
  Iap2WifiConfig wifi;
  std::string address = "10.10.0.1";
  std::uint32_t port = 7000;
  std::optional<Iap2ThemeAsset> installed_asset;
};
// Numeric interface facts from the installed iAP2 specification archive.
// The caller must have completed MFi authentication before sending this.
bool wireless_carplay_available(const ControlMessage&);
ControlMessage make_carplay_start(const Iap2CarplayStart&);
}  // namespace headunit
