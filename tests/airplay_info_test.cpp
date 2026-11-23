// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_info.hpp"
#include "headunit/airplay_session_setup.hpp"
#include "headunit/cluster_input.hpp"

#include <plist/plist.h>

#include <cstdint>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct TraceOutput {
  std::ostringstream output;
  std::streambuf* previous = std::cout.rdbuf(output.rdbuf());
  ~TraceOutput() { std::cout.rdbuf(previous); }
};

std::uint64_t number(plist_t object, const char* key) {
  std::uint64_t value = 0;
  plist_get_uint_val(plist_dict_get_item(object, key), &value);
  return value;
}
}  // namespace

int main() try {
  headunit::AirplayIdentity identity{
      "Independent Headunit",
      "02:11:22:33:44:55",
      "0x44540380,0x61",
      "0x4",
      "IndependentHeadunit",
      "1.1",
      "950.7.1",
      "AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA",
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "BBBBBBBB-BBBB-4BBB-8BBB-BBBBBBBBBBBB",
      "CCCCCCCC-CCCC-4CCC-8CCC-CCCCCCCCCCCC",
      0x0000006144540380ULL,
      4};
  identity.manufacturer = "Test Workshop";
  identity.serial = "Test Serial";
  identity.firmware = "2.3";
  identity.hardware = "Test Board";
  identity.touchscreen_name = "Test Touch";
  const auto accessory = headunit::make_iap2_identity(identity);
  check(accessory.name == identity.name && accessory.model == identity.model &&
            accessory.manufacturer == identity.manufacturer &&
            accessory.serial == identity.serial && accessory.firmware == identity.firmware &&
            accessory.hardware == identity.hardware,
        "Bluetooth iAP identity differs from configured CarPlay identity");
  const auto bytes = headunit::make_airplay_info(identity, {1664, 832, 60}, {1024, 600, 30});
  check(bytes.size() > 100 && bytes.size() < 4096, "unexpected /info body size");
  plist_t decoded = nullptr;
  check(plist_from_bin(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::uint32_t>(bytes.size()), &decoded) == PLIST_ERR_SUCCESS &&
            decoded,
        "binary /info plist cannot be decoded");
  auto text_matches = [](plist_t object, const char* key, const std::string& expected) {
    char* text = nullptr;
    plist_get_string_val(plist_dict_get_item(object, key), &text);
    const bool same = text && text == expected;
    plist_mem_free(text);
    return same;
  };
  check(text_matches(decoded, "name", identity.name) &&
            text_matches(decoded, "model", identity.model) &&
            text_matches(decoded, "manufacturer", identity.manufacturer),
        "configured CarPlay device labels");
  check(text_matches(plist_array_get_item(plist_dict_get_item(decoded, "hidDevices"), 0), "name",
                     identity.touchscreen_name),
        "configured touchscreen label");

  check(number(decoded, "features") == identity.features, "feature mask changed in serialization");
  plist_t displays = plist_dict_get_item(decoded, "displays");
  check(displays && plist_array_get_size(displays) == 2,
        "expected two independent display descriptions");
  check(number(plist_array_get_item(displays, 0), "widthPixels") == 1664 &&
            number(plist_array_get_item(displays, 0), "heightPixels") == 832 &&
            number(plist_array_get_item(displays, 1), "heightPixels") == 600,
        "display dimensions changed in serialization");
  check(number(plist_array_get_item(displays, 0), "type") == 110 &&
            number(plist_array_get_item(displays, 1), "type") == 111,
        "display roles changed in serialization");
  check(!plist_dict_get_item(plist_array_get_item(displays, 0), "widthPhysical"),
        "ordinary mode invented physical size");
  plist_t hid_devices = plist_dict_get_item(decoded, "hidDevices");
  check(hid_devices && plist_get_node_type(hid_devices) == PLIST_ARRAY &&
            plist_array_get_size(hid_devices) == 1,
        "main touchscreen is missing");
  plist_t touch = plist_array_get_item(hid_devices, 0);
  plist_t descriptor = plist_dict_get_item(touch, "hidDescriptor");
  check(descriptor && plist_get_node_type(descriptor) == PLIST_DATA,
        "touchscreen HID descriptor is missing");
  char* report = nullptr;
  std::uint64_t report_length = 0;
  plist_get_data_val(descriptor, &report, &report_length);
  check(report && report_length >= 40 && report_length <= 128,
        "touchscreen HID descriptor has an invalid size");
  const auto* descriptor_bytes = reinterpret_cast<const unsigned char*>(report);
  for (const auto maximum : {1663u, 831u}) {
    const std::array<unsigned char, 3> logical_maximum{0x26, static_cast<unsigned char>(maximum),
                                                       static_cast<unsigned char>(maximum >> 8)};
    check(std::search(descriptor_bytes, descriptor_bytes + report_length, logical_maximum.begin(),
                      logical_maximum.end()) != descriptor_bytes + report_length,
          "touchscreen range does not match resized display");
  }
  check(!plist_dict_get_item(decoded, "uiSyncInfo") && !plist_dict_get_item(decoded, "hevcInfo") &&
            !plist_dict_get_item(decoded, "displayPanels"),
        "ordinary info unexpectedly advertises Ultra");
  check(plist_array_get_size(plist_dict_get_item(decoded, "audioFormats")) == 0,
        "audio was advertised without an output path");
  plist_mem_free(report);
  plist_free(decoded);
  const auto media_only = headunit::make_airplay_info(
      identity, {1280, 720, 60}, {1920, 720, 30}, false, "Center_Display", "Cluster_Display", true);
  decoded = nullptr;
  check(plist_from_bin(reinterpret_cast<const char*>(media_only.data()), media_only.size(),
                       &decoded) == PLIST_ERR_SUCCESS,
        "audio info decode");
  auto formats = plist_dict_get_item(decoded, "audioFormats");
  check(plist_array_get_size(formats) == 9 &&
            number(plist_array_get_item(formats, 0), "type") == 100 &&
            text_matches(plist_array_get_item(formats, 0), "audioType", "compatibility") &&
            number(plist_array_get_item(formats, 0), "audioOutputFormats") == 0xc3fc &&
            number(plist_array_get_item(formats, 2), "audioOutputFormats") == 0x7000c3fc &&
            number(plist_array_get_item(formats, 8), "type") == 102 &&
            number(plist_array_get_item(formats, 8), "audioOutputFormats") == 0x800000,
        "AAC media capability changed");
  check(plist_array_get_size(plist_dict_get_item(decoded, "audioLatencies")) == 9,
        "media latency declaration missing");
  plist_free(decoded);
  for (const bool ultra_enabled : {false, true}) {
    for (const bool icon_enabled : {false, true}) {
      const auto icon_info = headunit::make_airplay_info(
          identity, {1920, 720, 30, 200, 75}, {1920, 720, 30, 200, 75}, ultra_enabled,
          "Center_Display", "Cluster_Display", false, icon_enabled);
      decoded = nullptr;
      check(plist_from_bin(reinterpret_cast<const char*>(icon_info.data()), icon_info.size(),
                           &decoded) == PLIST_ERR_SUCCESS &&
                decoded,
            "OEM icon info decode");
      const auto visible = plist_dict_get_item(decoded, "oemIconVisible");
      std::uint8_t offered = 0;
      plist_get_bool_val(visible, &offered);
      check(
          visible && plist_get_node_type(visible) == PLIST_BOOLEAN && bool(offered) == icon_enabled,
          "OEM icon offer does not follow the receiver option");
      if (icon_enabled) {
        check(text_matches(decoded, "oemIconLabel", "Home"), "OEM Home label missing");
        const auto icon = plist_dict_get_item(decoded, "oemIcon");
        check(icon && plist_get_node_type(icon) == PLIST_DATA, "OEM Home image missing");
        char* png = nullptr;
        std::uint64_t size = 0;
        plist_get_data_val(icon, &png, &size);
        constexpr std::array<unsigned char, 8> signature{0x89, 'P',  'N',  'G',
                                                         0x0d, 0x0a, 0x1a, 0x0a};
        constexpr std::array<unsigned char, 16> header{0, 0, 0, 13,  'I', 'H', 'D', 'R',
                                                       0, 0, 0, 120, 0,   0,   0,   120};
        const auto* data = reinterpret_cast<const unsigned char*>(png);
        check(png && size >= 33 && size <= 4096 &&
                  std::equal(signature.begin(), signature.end(), data) &&
                  std::equal(header.begin(), header.end(), data + signature.size()),
              "OEM Home image is not a bounded 120-pixel PNG");
        plist_mem_free(png);
      } else {
        check(!plist_dict_get_item(decoded, "oemIconLabel") &&
                  !plist_dict_get_item(decoded, "oemIcon"),
              "disabled OEM icon retains a label or image");
      }
      if (ultra_enabled) {
        const auto icon_panels = plist_dict_get_item(decoded, "displayPanels");
        check(text_matches(plist_array_get_item(icon_panels, 0), "uid", "Center_Display") &&
                  text_matches(plist_array_get_item(icon_panels, 1), "uid", "Cluster_Display"),
              "OEM icon offer changed signed display bindings");
      }
      plist_free(decoded);
    }
  }
  const auto ultra =
      headunit::make_airplay_info(identity, {1280, 720, 60, 155, 87}, {1920, 720, 30, 200, 75},
                                  true, "Main_Custom", "Cluster_Custom");
  decoded = nullptr;
  check(plist_from_bin(reinterpret_cast<const char*>(ultra.data()), ultra.size(), &decoded) ==
            PLIST_ERR_SUCCESS,
        "Ultra info decode");
  const auto panels = plist_dict_get_item(decoded, "displayPanels");
  check(plist_array_get_size(panels) == 2 &&
            plist_get_node_type(plist_dict_get_item(decoded, "uiSyncInfo")) == PLIST_DICT,
        "Ultra panel/channel info missing");
  check(plist_get_node_type(plist_dict_get_item(decoded, "hevcInfo")) == PLIST_DICT,
        "negotiated HEVC requires its information dictionary");
  for (unsigned i = 0; i < 2; ++i) {
    const auto panel = plist_array_get_item(panels, i);
    char* id = nullptr;
    plist_get_string_val(plist_dict_get_item(panel, "uid"), &id);
    check(id && std::string(id) == (i ? "Cluster_Custom" : "Main_Custom"),
          "physical panel must identify its signed asset layout");
    plist_mem_free(id);
    check(number(panel, "widthPixels") == (i ? 1920 : 1280) && number(panel, "heightPixels") == 720,
          "panel/video dimensions differ");
    const auto video_display = plist_array_get_item(plist_dict_get_item(decoded, "displays"), i);
    check(number(video_display, "widthPixels") == number(panel, "widthPixels"),
          "video and Ultra geometry differ");
    check(number(video_display, "maxFPS") == unsigned(i ? 30 : 60), "configured FPS ignored");
    if (i == 0)
      check(number(panel, "widthPhysical") == 155 && number(video_display, "heightPhysical") == 87,
            "measured physical size ignored");
    else
      check(number(panel, "widthPhysical") == 200 && number(video_display, "heightPhysical") == 75,
            "virtual panel physical size ignored");
    const auto initial = plist_dict_get_item(panel, "initialVideoStreams");
    check(plist_array_get_size(initial) == 1, "ambiguous initial video mapping");
    plist_get_string_val(plist_dict_get_item(plist_array_get_item(initial, 0), "uuid"), &id);
    check(id && std::string(id) == (i ? identity.cluster_display_id : identity.main_display_id),
          "panel video UUID mismatch");
    plist_mem_free(id);
    auto properties = plist_dict_get_item(panel, "properties");
    plist_get_string_val(plist_array_get_item(properties, 0), &id);
    check(id && std::string(id) == "DPManaged" && plist_array_get_size(properties) == i + 1,
          "managed panel properties missing");
    plist_mem_free(id);
  }
  touch = plist_array_get_item(plist_dict_get_item(decoded, "hidDevices"), 0);
  const auto devices = plist_dict_get_item(decoded, "hidDevices");
  check(plist_array_get_size(devices) == 3, "Ultra cluster navigation or media device missing");
  {
    // A two-finger digitizer repeats the finger collection (pinch/rotate).
    headunit::set_multitouch(true);
    const auto pinch_info = headunit::make_airplay_info(identity, {1280, 720, 60, 155, 87},
                                                        {1920, 720, 30, 200, 75}, true);
    headunit::set_multitouch(false);
    plist_t pinch = nullptr;
    plist_from_bin(reinterpret_cast<const char*>(pinch_info.data()),
                   static_cast<std::uint32_t>(pinch_info.size()), &pinch);
    check(pinch != nullptr, "multitouch info did not decode");
    char* pinch_report = nullptr;
    std::uint64_t pinch_length = 0;
    plist_get_data_val(
        plist_dict_get_item(plist_array_get_item(plist_dict_get_item(pinch, "hidDevices"), 0),
                            "hidDescriptor"),
        &pinch_report, &pinch_length);
    const std::string bytes(pinch_report, pinch_length);
    check(pinch_length == 139 &&
              bytes.find("\x09\x22\xa1\x02") != bytes.rfind("\x09\x22\xa1\x02") &&
              static_cast<unsigned char>(pinch_report[15]) == 2,
          "two-finger digitizer descriptor invalid");
    plist_mem_free(pinch_report);
    plist_free(pinch);
  }
  const auto media = plist_array_get_item(devices, 2);
  check(text_matches(media, "displayUUID", identity.main_display_id.c_str()) &&
            text_matches(media, "uuid", headunit::media_hid_uuid),
        "media keys target the wrong device or display");
  char* media_report = nullptr;
  std::uint64_t media_length = 0;
  plist_get_data_val(plist_dict_get_item(media, "hidDescriptor"), &media_report, &media_length);
  check(media_length == 27 && static_cast<unsigned char>(media_report[15]) == 0xcd &&
            static_cast<unsigned char>(media_report[17]) == 0xb5 &&
            static_cast<unsigned char>(media_report[19]) == 0xb6,
        "invalid media consumer descriptor");
  plist_mem_free(media_report);
  const auto buttons = plist_array_get_item(devices, 1);
  check(text_matches(buttons, "displayUUID", identity.cluster_display_id.c_str()),
        "cluster navigation targets the main display");
  char* button_report = nullptr;
  std::uint64_t button_length = 0;
  plist_get_data_val(plist_dict_get_item(buttons, "hidDescriptor"), &button_report, &button_length);
  check(button_length == 33 && static_cast<unsigned char>(button_report[1]) == 0x0c &&
            static_cast<unsigned char>(button_report[31]) == 0x03 &&
            static_cast<unsigned char>(button_report[32]) == 0xc0,
        "invalid cluster consumer descriptor");
  plist_mem_free(button_report);
  report = nullptr;
  report_length = 0;
  plist_get_data_val(plist_dict_get_item(touch, "hidDescriptor"), &report, &report_length);
  descriptor_bytes = reinterpret_cast<const unsigned char*>(report);
  for (auto max : {1279u, 719u}) {
    const std::array<unsigned char, 3> pattern{0x26, static_cast<unsigned char>(max),
                                               static_cast<unsigned char>(max >> 8)};
    check(std::search(descriptor_bytes, descriptor_bytes + report_length, pattern.begin(),
                      pattern.end()) != descriptor_bytes + report_length,
          "Ultra touch bounds differ from main panel");
  }
  plist_mem_free(report);
  check(!plist_dict_get_item(decoded, "fileTransferInfo"),
        "unimplemented file transfer advertised");
  plist_free(decoded);
  // A dual-capability receiver must answer the post-SETUP /info using the
  // phone's actual choice, without losing the next connection's Ultra offer.
  const auto capability_info = headunit::make_airplay_info(identity, {1920, 720, 30, 200, 75},
                                                           {1920, 720, 30, 200, 75}, true);
  const auto standard_info =
      headunit::make_airplay_info(identity, {1280, 720, 30}, {1920, 720, 30});
  for (const bool native : {false, true})
    for (unsigned feature_case = 0; feature_case < 5; ++feature_case) {
      headunit::AirplaySessionSetup receiver;
      receiver.register_data_service("07D9F906-8D64-4B54-A808-A20BCA2C51C2");
      receiver.register_data_service("3E2F3C61-AAD0-42CB-A8AA-BF22186DA62E");
      receiver.register_data_service("FF4A6720-F2BE-4F56-A3E1-DB3B4E37D634");
      receiver.enable_vehicle_state();
      receiver.set_video_consumer([](auto, auto&) { return true; });
      if (native) receiver.enable_native_ultra();
      sockaddr_in6 peer{};
      peer.sin6_family = AF_INET6;
      peer.sin6_addr = in6addr_loopback;
      peer.sin6_port = htons(49152);
      receiver.set_peer(peer);
      auto request = plist_new_dict();
      plist_dict_set_item(request, "timingPort", plist_new_uint(49152));
      if (feature_case) {
        auto features = plist_new_array();
        if (feature_case == 2 || feature_case == 4)
          plist_array_append_item(features, plist_new_string("hevc"));
        if (feature_case == 3 || feature_case == 4)
          plist_array_append_item(features, plist_new_string("uiSync"));
        plist_array_append_item(features, plist_new_string("vehicleStateProtocol"));
        plist_dict_set_item(request, "features", features);
      }
      char* encoded = nullptr;
      std::uint32_t length = 0;
      check(plist_to_bin(request, &encoded, &length) == PLIST_ERR_SUCCESS,
            "session offer encoding");
      headunit::ConnectionDiagnostics trace(headunit::ConnectionDiagnostics::Scope::control,
                                            feature_case + 1, true);
      TraceOutput observation;
      const auto setup_reply =
          receiver.respond({reinterpret_cast<const std::uint8_t*>(encoded), length}, &trace, 9);
      constexpr unsigned masks[]{0, 4, 6, 5, 7};
      check(observation.output.str().find(
                "stage=setup-features elapsed_ns=0 result=" + std::to_string(masks[feature_case]) +
                " linkGeneration=9\n") != std::string::npos,
            "SETUP diagnostic mask differs from received feature offer");
      plist_mem_free(encoded);
      plist_free(request);
      plist_t setup_decoded = nullptr;
      check(plist_from_bin(reinterpret_cast<const char*>(setup_reply.data()), setup_reply.size(),
                           &setup_decoded) == PLIST_ERR_SUCCESS,
            "feature agreement decode");
      const auto enabled = plist_dict_get_item(setup_decoded, "enabledFeatures");
      bool agreed_vehicle = false;
      for (std::uint32_t i = 0; i < plist_array_get_size(enabled); ++i) {
        auto feature = plist_array_get_item(enabled, i);
        char* name = nullptr;
        plist_get_string_val(feature, &name);
        if (name) {
          agreed_vehicle |= std::string_view(name) == "vehicleStateProtocol";
          plist_mem_free(name);
        }
      }
      check(agreed_vehicle == (native ? feature_case == 4 : feature_case != 0),
            "Vehicle agreement differs from the selected profile's resources");
      plist_free(setup_decoded);
      const auto& selected =
          headunit::select_airplay_info(capability_info, standard_info, receiver.negotiated_mode());
      const bool requested_ultra = native && feature_case == 4;
      decoded = nullptr;
      check(plist_from_bin(reinterpret_cast<const char*>(selected.data()), selected.size(),
                           &decoded) == PLIST_ERR_SUCCESS,
            "selected session info decode");
      const auto main = plist_array_get_item(plist_dict_get_item(decoded, "displays"), 0);
      check(number(main, "widthPixels") == unsigned(requested_ultra ? 1920 : 1280),
            "session main geometry ignored phone choice");
      check(text_matches(main, "uuid", identity.main_display_id),
            "session selection changed stable main UUID");
      check(bool(plist_dict_get_item(decoded, "displayPanels")) == requested_ultra &&
                bool(plist_dict_get_item(decoded, "uiSyncInfo")) == requested_ultra &&
                bool(plist_dict_get_item(decoded, "hevcInfo")) == requested_ultra,
            "Standard session advertised Ultra-only information");
      auto device = plist_array_get_item(plist_dict_get_item(decoded, "hidDevices"), 0);
      char* descriptor = nullptr;
      std::uint64_t descriptor_length = 0;
      plist_get_data_val(plist_dict_get_item(device, "hidDescriptor"), &descriptor,
                         &descriptor_length);
      const auto* data = reinterpret_cast<const unsigned char*>(descriptor);
      const auto xmax = unsigned(requested_ultra ? 1919 : 1279);
      const std::array<unsigned char, 3> limit{0x26, static_cast<unsigned char>(xmax),
                                               static_cast<unsigned char>(xmax >> 8)};
      check(descriptor && std::search(data, data + descriptor_length, limit.begin(), limit.end()) !=
                              data + descriptor_length,
            "session HID does not match main geometry");
      plist_mem_free(descriptor);
      plist_free(decoded);
      check(headunit::select_airplay_info(capability_info, standard_info, std::nullopt) ==
                    capability_info &&
                headunit::select_airplay_info(capability_info, standard_info,
                                              headunit::CarplayMode::ultra) == capability_info,
            "Standard session changed a later Ultra capability offer");
    }
  bool rejected = false;
  try {
    (void)headunit::make_airplay_info(identity, {100, 720, 60, 155, 87}, {1920, 720, 60, 200, 75},
                                      true);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid display dimensions accepted");
  rejected = false;
  try {
    (void)headunit::make_airplay_info(identity, {1280, 720, 60}, {1920, 720, 60}, true);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "Ultra allowed physical dimensions that iOS cannot parse");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
