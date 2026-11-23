// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/drive_mode.hpp>

#include <algorithm>
#include <type_traits>

namespace headunit {
namespace {
bool letter(char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}
bool character(char value) {
  return letter(value) || (value >= '0' && value <= '9') || value == '_' || value == '-';
}
}  // namespace
static_assert(std::is_trivially_copyable_v<DriveModeIdentifier>);
static_assert(std::is_trivially_copyable_v<DriveModeCatalog>);
static_assert(std::is_nothrow_copy_assignable_v<DriveModeCatalog>);
bool valid_drive_mode_identifier(const DriveModeIdentifier& identifier) noexcept {
  if (identifier.length == 0 || identifier.length > identifier.bytes.size() ||
      !letter(identifier.bytes[0]))
    return false;
  for (std::size_t i = 0; i < identifier.length; ++i)
    if (!character(identifier.bytes[i])) return false;
  for (std::size_t i = identifier.length; i < identifier.bytes.size(); ++i)
    if (identifier.bytes[i] != 0) return false;
  return true;
}
std::optional<DriveModeIdentifier> parse_drive_mode_identifier(std::string_view value) noexcept {
  if (value.empty() || value.size() > 64 || !letter(value.front())) return std::nullopt;
  for (const auto item : value)
    if (!character(item)) return std::nullopt;
  DriveModeIdentifier result;
  std::copy(value.begin(), value.end(), result.bytes.begin());
  result.length = static_cast<std::uint8_t>(value.size());
  return result;
}
std::string_view drive_mode_identifier_key(const DriveModeIdentifier& identifier) noexcept {
  if (!valid_drive_mode_identifier(identifier)) return {};
  return {identifier.bytes.data(), identifier.length};
}
bool valid_drive_mode_catalog(const DriveModeCatalog& catalog) noexcept {
  if (catalog.count == 0 || catalog.count > catalog.identifiers.size()) return false;
  for (std::size_t i = 0; i < catalog.count; ++i) {
    if (!valid_drive_mode_identifier(catalog.identifiers[i])) return false;
    for (std::size_t previous = 0; previous < i; ++previous)
      if (catalog.identifiers[i] == catalog.identifiers[previous]) return false;
  }
  for (std::size_t i = catalog.count; i < catalog.identifiers.size(); ++i)
    if (catalog.identifiers[i] != DriveModeIdentifier{}) return false;
  return true;
}
bool contains_drive_mode(const DriveModeCatalog& catalog,
                         const DriveModeIdentifier& identifier) noexcept {
  if (!valid_drive_mode_catalog(catalog) || !valid_drive_mode_identifier(identifier)) return false;
  for (std::size_t i = 0; i < catalog.count; ++i)
    if (catalog.identifiers[i] == identifier) return true;
  return false;
}
std::optional<DriveModeCatalog> parse_drive_mode_catalog_key(std::string_view value) noexcept {
  if (value.empty() || value.size() > 32 * 64 + 31) return std::nullopt;
  DriveModeCatalog result;
  while (!value.empty()) {
    if (result.count == result.identifiers.size()) return std::nullopt;
    const auto comma = value.find(',');
    const auto identifier = parse_drive_mode_identifier(value.substr(0, comma));
    if (!identifier) return std::nullopt;
    result.identifiers[result.count++] = *identifier;
    if (comma == std::string_view::npos) break;
    value.remove_prefix(comma + 1);
    if (value.empty()) return std::nullopt;
  }
  if (!valid_drive_mode_catalog(result)) return std::nullopt;
  return result;
}
std::string drive_mode_catalog_key(const DriveModeCatalog& catalog) {
  if (!valid_drive_mode_catalog(catalog)) return {};
  std::string result;
  for (std::size_t i = 0; i < catalog.count; ++i) {
    if (i) result += ',';
    result += drive_mode_identifier_key(catalog.identifiers[i]);
  }
  return result;
}
}  // namespace headunit
