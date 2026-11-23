// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_info.hpp"
#include "headunit/touch_input.hpp"
#include "headunit/cluster_input.hpp"

#include <plist/plist.h>

#include "oem_home_icon.inc"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace headunit {
const std::vector<std::uint8_t>& select_airplay_info(const std::vector<std::uint8_t>& capabilities,
                                                     const std::vector<std::uint8_t>& standard,
                                                     std::optional<CarplayMode> mode) noexcept {
  return mode == CarplayMode::standard ? standard : capabilities;
}

namespace {
template <class Edit>
std::vector<std::uint8_t> edit_info(std::span<const std::uint8_t> info, Edit edit) {
  plist_t root = nullptr;
  plist_from_bin(reinterpret_cast<const char*>(info.data()),
                 static_cast<std::uint32_t>(info.size()), &root);
  if (!root || plist_get_node_type(root) != PLIST_DICT) {
    if (root) plist_free(root);
    throw std::invalid_argument("AirPlay capability plist is not a dictionary");
  }
  const std::unique_ptr<void, decltype(&plist_free)> owner(root, plist_free);
  edit(root);
  char* encoded = nullptr;
  std::uint32_t length = 0;
  if (plist_to_bin(root, &encoded, &length) != PLIST_ERR_SUCCESS || !encoded)
    throw std::runtime_error("cannot encode AirPlay capability plist");
  std::vector<std::uint8_t> bytes(encoded, encoded + length);
  plist_mem_free(encoded);
  return bytes;
}
}  // namespace

std::vector<std::uint8_t> with_limited_ui(std::span<const std::uint8_t> info, bool limited) {
  return edit_info(info, [&](plist_t root) {
    plist_dict_set_item(root, "limitedUI", plist_new_bool(limited ? 1 : 0));
    plist_t elements = plist_new_array();
    for (const char* element : {"softKeyboard", "softPhoneKeypad", "nonMusicLists", "musicLists"})
      plist_array_append_item(elements, plist_new_string(element));
    plist_dict_set_item(root, "limitedUIElements", elements);
  });
}

std::vector<std::uint8_t> with_night_mode(std::span<const std::uint8_t> info, bool night) {
  return edit_info(info, [&](plist_t root) {
    plist_dict_set_item(root, "nightMode", plist_new_bool(night ? 1 : 0));
  });
}

namespace {
void text(plist_t dictionary, const char* key, const std::string& value) {
  plist_dict_set_item(dictionary, key, plist_new_string(value.c_str()));
}

void number(plist_t dictionary, const char* key, std::uint64_t value) {
  plist_dict_set_item(dictionary, key, plist_new_uint(value));
}

std::vector<std::uint8_t> touch_descriptor(AirplayDisplaySize size, bool multitouch) {
  // Absolute touchscreen: one finger collection per contact, each with its
  // transducer index, touch bit, X and Y. Two contacts allow pinch gestures.
  // Usage values and descriptor item encodings are defined by USB-IF HID.
  std::vector<std::uint8_t> bytes{0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01};
  const std::uint8_t contacts = multitouch ? 2 : 1;
  const auto finger = [&](bool restore_page) {
    // The axes below select Generic Desktop; a later finger returns to Digitizer.
    if (restore_page) bytes.insert(bytes.end(), {0x05, 0x0d});
    bytes.insert(bytes.end(),
                 {0x09, 0x22, 0xa1, 0x02, 0x09, 0x38, 0x15, 0x00, 0x25, contacts, 0x75, 0x08,
                  0x95, 0x01, 0x81, 0x02, 0x09, 0x33, 0x15, 0x00, 0x25, 0x01,     0x75, 0x01,
                  0x95, 0x01, 0x81, 0x02, 0x75, 0x07, 0x95, 0x01, 0x81, 0x03});
    auto axis = [&](std::uint8_t usage, std::uint16_t maximum) {
      bytes.insert(bytes.end(),
                   {0x05, 0x01, 0x09, usage, 0x15, 0x00, 0x26, static_cast<std::uint8_t>(maximum),
                    static_cast<std::uint8_t>(maximum >> 8), 0x75, 0x10, 0x95, 0x01, 0x81, 0x02});
    };
    axis(0x30, static_cast<std::uint16_t>(size.width - 1));
    axis(0x31, static_cast<std::uint16_t>(size.height - 1));
    bytes.push_back(0xc0);
  };
  finger(false);
  if (multitouch) finger(true);
  bytes.push_back(0xc0);
  return bytes;
}

plist_t touch_device(const std::string& display_uuid, AirplayDisplaySize size,
                     const std::string& name) {
  plist_t device = plist_new_dict();
  number(device, "hidProductID", 1);
  number(device, "hidVendorID", 0xffff);
  number(device, "hidCountryCode", 0);
  text(device, "uuid", touchscreen_hid_uuid);
  text(device, "name", name);
  text(device, "displayUUID", display_uuid);
  const auto descriptor = touch_descriptor(size, multitouch_enabled());
  plist_dict_set_item(
      device, "hidDescriptor",
      plist_new_data(reinterpret_cast<const char*>(descriptor.data()), descriptor.size()));
  return device;
}

plist_t display(const std::string& uuid, AirplayDisplaySize size, std::uint64_t type,
                std::uint64_t input_device) {
  if (uuid.size() != 36 || size.width < 320 || size.width > 8192 || size.height < 240 ||
      size.height > 4320 || size.max_fps < 15 || size.max_fps > 120 ||
      bool(size.width_mm) != bool(size.height_mm) || size.width_mm > 2000 ||
      size.height_mm > 2000) {
    throw std::invalid_argument("invalid AirPlay display configuration");
  }
  plist_t item = plist_new_dict();
  text(item, "uuid", uuid);
  number(item, "type", type);
  number(item, "maxFPS", size.max_fps);
  number(item, "widthPixels", size.width);
  number(item, "heightPixels", size.height);
  if (size.width_mm && size.height_mm) {
    number(item, "widthPhysical", size.width_mm);
    number(item, "heightPhysical", size.height_mm);
  }
  number(item, "features", 10);
  number(item, "primaryInputDevice", input_device);
  number(item, "initialViewArea", 0);

  plist_t view_areas = plist_new_array();
  plist_t view = plist_new_dict();
  number(view, "widthPixels", size.width);
  number(view, "heightPixels", size.height);
  number(view, "originXPixels", 0);
  number(view, "originYPixels", 0);
  plist_t safe_area = plist_new_dict();
  for (const char* edge : {"top", "bottom", "left", "right"}) {
    number(safe_area, edge, 0);
  }
  plist_dict_set_item(view, "safeArea", safe_area);
  plist_array_append_item(view_areas, view);
  plist_dict_set_item(item, "viewAreas", view_areas);
  return item;
}
}  // namespace

std::vector<std::uint8_t> make_airplay_info(const AirplayIdentity& identity,
                                            AirplayDisplaySize main_display,
                                            AirplayDisplaySize cluster_display, bool native_ultra,
                                            const std::string& main_panel,
                                            const std::string& cluster_panel, bool media_audio,
                                            bool oem_icon) {
  if (main_panel.empty() || cluster_panel.empty() || main_panel == cluster_panel)
    throw std::invalid_argument("invalid Ultra panel names");
  if (native_ultra && (!main_display.width_mm || !main_display.height_mm ||
                       !cluster_display.width_mm || !cluster_display.height_mm))
    throw std::invalid_argument("Ultra panels require explicit physical dimensions in millimetres");
  // Owns the whole tree so a rejected display configuration cannot leak it.
  const std::unique_ptr<void, decltype(&plist_free)> owner(plist_new_dict(), plist_free);
  plist_t root = owner.get();
  text(root, "name", identity.name);
  text(root, "model", identity.model);
  text(root, "manufacturer", identity.manufacturer);
  text(root, "deviceID", identity.device_id);
  text(root, "sourceVersion", identity.source_version);
  number(root, "features", identity.features);
  number(root, "statusFlags", identity.flags);
  plist_t displays = plist_new_array();
  plist_dict_set_item(root, "displays", displays);  // Owned by root before display() may throw.
  plist_array_append_item(displays, display(identity.main_display_id, main_display, 110, 1));
  plist_array_append_item(displays, display(identity.cluster_display_id, cluster_display, 111, 3));
  if (native_ultra) {
    plist_t panels = plist_new_array();
    for (unsigned index = 0; index < 2; ++index) {
      const auto& uuid = index ? identity.cluster_display_id : identity.main_display_id;
      const auto size = index ? cluster_display : main_display;
      plist_t panel = plist_new_dict();
      text(panel, "uid", index ? cluster_panel : main_panel);
      number(panel, "widthPixels", size.width);
      number(panel, "heightPixels", size.height);
      if (size.width_mm && size.height_mm) {
        number(panel, "widthPhysical", size.width_mm);
        number(panel, "heightPhysical", size.height_mm);
      }
      plist_t properties = plist_new_array();
      plist_array_append_item(properties, plist_new_string("DPManaged"));
      if (index) plist_array_append_item(properties, plist_new_string("showsInstruments"));
      plist_dict_set_item(panel, "properties", properties);
      plist_t initial = plist_new_array(), video = plist_new_dict();
      text(video, "uuid", uuid);
      number(video, "originXPixels", 0);
      number(video, "originYPixels", 0);
      number(video, "zIndex", 0);
      plist_array_append_item(initial, video);
      plist_dict_set_item(panel, "initialVideoStreams", initial);
      plist_array_append_item(panels, panel);
    }
    plist_dict_set_item(root, "displayPanels", panels);
    plist_dict_set_item(root, "uiSyncInfo", plist_new_dict());
    // iOS requires the matching information dictionary for negotiated HEVC.
    plist_dict_set_item(root, "hevcInfo", plist_new_dict());
  }
  plist_t hid_devices = plist_new_array();
  plist_array_append_item(
      hid_devices, touch_device(identity.main_display_id, main_display, identity.touchscreen_name));
  plist_dict_set_item(root, "hidDevices", hid_devices);
  if (native_ultra) {
    plist_t device = plist_new_dict();
    number(device, "hidProductID", 2);
    number(device, "hidVendorID", 0xffff);
    number(device, "hidCountryCode", 0);
    text(device, "uuid", cluster_hid_uuid);
    text(device, "name", "Cluster navigation");
    text(device, "displayUUID", identity.cluster_display_id);
    // USB-IF Consumer Control: Menu Pick, Up, Down, Left, Right, Escape.
    constexpr std::array<std::uint8_t, 33> descriptor{
        0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75,
        0x01, 0x95, 0x06, 0x09, 0x41, 0x09, 0x42, 0x09, 0x43, 0x09, 0x44,
        0x09, 0x45, 0x09, 0x46, 0x81, 0x02, 0x95, 0x02, 0x81, 0x03, 0xc0};
    plist_dict_set_item(
        device, "hidDescriptor",
        plist_new_data(reinterpret_cast<const char*>(descriptor.data()), descriptor.size()));
    plist_array_append_item(hid_devices, device);
    plist_t media = plist_new_dict();
    number(media, "hidProductID", 3);
    number(media, "hidVendorID", 0xffff);
    number(media, "hidCountryCode", 0);
    text(media, "uuid", media_hid_uuid);
    text(media, "name", "Media keys");
    text(media, "displayUUID", identity.main_display_id);
    // USB-IF Consumer Control: Play/Pause, Scan Next Track, Scan Previous Track.
    constexpr std::array<std::uint8_t, 27> media_descriptor{
        0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03,
        0x09, 0xcd, 0x09, 0xb5, 0x09, 0xb6, 0x81, 0x02, 0x95, 0x05, 0x81, 0x03, 0xc0};
    plist_dict_set_item(media, "hidDescriptor",
                        plist_new_data(reinterpret_cast<const char*>(media_descriptor.data()),
                                       media_descriptor.size()));
    plist_array_append_item(hid_devices, media);
  }
  plist_t audio_formats = plist_new_array();
  plist_t audio_latencies = plist_new_array();
  if (media_audio) {
    {
      // iOS opened buffered AAC music only when the basic CarPlay audio
      // profile accompanied stream 102. The session now admits the advertised
      // PCM/Opus categories through bounded stream workers and a shared sink.
      struct Profile {
        unsigned type;
        const char* category;
        std::uint64_t output;
        std::uint64_t input;
      };
      constexpr std::array profiles{
          Profile{100, "compatibility", 0xc3fc, 0x4154},
          Profile{101, "compatibility", 0xc3fc, 0},
          Profile{100, "default", 0x7000c3fc, 0x70004154},
          Profile{100, "alert", 0x7000c3fc, 0},
          Profile{100, "media", 0xc3fc, 0},
          Profile{100, "telephony", 0x70004154, 0x70004154},
          Profile{100, "speechRecognition", 0x70004154, 0x70004154},
          Profile{101, "default", 0x7000c3fc, 0},
      };
      for (const auto& profile : profiles) {
        plist_t format = plist_new_dict();
        number(format, "type", profile.type);
        text(format, "audioType", profile.category);
        number(format, "audioOutputFormats", profile.output);
        if (profile.input) number(format, "audioInputFormats", profile.input);
        plist_array_append_item(audio_formats, format);
        plist_t latency = plist_new_dict();
        number(latency, "type", profile.type);
        text(latency, "audioType", profile.category);
        number(latency, "inputLatencyMicros", 0);
        number(latency, "outputLatencyMicros", 0);
        plist_array_append_item(audio_latencies, latency);
      }
    }
    plist_t format = plist_new_dict();
    number(format, "type", 102);
    text(format, "audioType", "media");
    number(format, "audioOutputFormats", 0x800000);  // AAC-LC, 48 kHz stereo.
    plist_array_append_item(audio_formats, format);
    plist_t latency = plist_new_dict();
    number(latency, "type", 102);
    text(latency, "audioType", "media");
    number(latency, "inputLatencyMicros", 0);
    number(latency, "outputLatencyMicros", 0);
    plist_array_append_item(audio_latencies, latency);
  }
  plist_dict_set_item(root, "audioFormats", audio_formats);
  plist_dict_set_item(root, "audioLatencies", audio_latencies);
  plist_t modes = plist_new_dict();
  plist_dict_set_item(modes, "appStates", plist_new_array());
  plist_dict_set_item(modes, "resources", plist_new_array());
  plist_dict_set_item(root, "modes", modes);
  plist_dict_set_item(root, "keepAliveLowPower", plist_new_bool(0));
  // Offer the phone's return-to-car icon (oem:back releases mainScreen).
  // Ultra additionally gates it on the phone-side pack's per-display
  // allowManufacturerAppIcon. This /info offer cannot override that policy.
  plist_dict_set_item(root, "oemIconVisible", plist_new_bool(oem_icon ? 1 : 0));
  if (oem_icon) {
    text(root, "oemIconLabel", "Home");
    plist_dict_set_item(root, "oemIcon",
                        plist_new_data(reinterpret_cast<const char*>(oem_home_icon_png),
                                       sizeof(oem_home_icon_png)));
  }

  char* encoded = nullptr;
  std::uint32_t length = 0;
  const auto result = plist_to_bin(root, &encoded, &length);
  if (result != PLIST_ERR_SUCCESS || !encoded) {
    throw std::runtime_error("cannot encode AirPlay capability plist");
  }
  std::vector<std::uint8_t> bytes(encoded, encoded + length);
  plist_mem_free(encoded);
  return bytes;
}

}  // namespace headunit
