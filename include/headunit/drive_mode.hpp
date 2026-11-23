// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace headunit {

// A vehicle's case-sensitive semantic identifier, independent of display text,
// a signed asset's array order, or the transmission's effective selector.
struct DriveModeIdentifier {
  std::array<char, 64> bytes{};
  std::uint8_t length = 0;
  bool operator==(const DriveModeIdentifier&) const = default;
};
struct DriveModeCatalog {
  std::array<DriveModeIdentifier, 32> identifiers{};
  std::uint8_t count = 0;
  bool operator==(const DriveModeCatalog&) const = default;
};

bool valid_drive_mode_identifier(const DriveModeIdentifier&) noexcept;
std::optional<DriveModeIdentifier> parse_drive_mode_identifier(std::string_view) noexcept;
// Malformed/default identifiers have no key. This never exposes bytes outside
// the bounded object, even if its public length was corrupted.
std::string_view drive_mode_identifier_key(const DriveModeIdentifier&) noexcept;
bool valid_drive_mode_catalog(const DriveModeCatalog&) noexcept;
bool contains_drive_mode(const DriveModeCatalog&, const DriveModeIdentifier&) noexcept;
std::optional<DriveModeCatalog> parse_drive_mode_catalog_key(std::string_view) noexcept;
// Comma-separated canonical identifiers, in the vehicle's configured order.
// An invalid catalog has no key; absent catalogs are represented separately.
std::string drive_mode_catalog_key(const DriveModeCatalog&);

}  // namespace headunit
