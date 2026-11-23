<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# iPhone turn-by-turn metadata

CarPlay has a separate iAP2 route-guidance feed for native instrument clusters
and head-up displays. It describes upcoming maneuvers, road names, distances and
trip estimates; it is independent of the H.264/HEVC map video. Apple describes
this in [Optimize CarPlay for vehicle systems (WWDC23)](https://developer.apple.com/videos/play/wwdc2023/10150/)
and [Turbocharge your app for CarPlay (WWDC25)](https://developer.apple.com/videos/play/wwdc2025/216/).
Third-party navigation apps must actually supply the metadata; map video alone
does not guarantee it.

## Implementation

The independent receiver now has an opt-in route-guidance component:

- Identification advertises component `0x001e`, configurable component ID/name,
  bounded text/maneuver capacity, and messages `0x5200/0x5203` sent and
  `0x5201/0x5202` received. Lane capacity is zero; lane/exit-info extensions
  are not advertised as implemented.
- After identification acceptance and MFi authentication, the receiver
  sends `StartRouteGuidanceUpdates` (`0x5200`) on the current iAP transport.
  Bluetooth can start the subscription before an iAP Wi-Fi stream exists. It asks for source name and
  source support, with at most three attempts, five seconds apart. Optional
  metadata never consumes the last reliable-link slots.
- `0x5201` updates route state, distance to the next maneuver, remaining trip
  distance/time, arrival timestamp, source, roads and the ordered maneuver cursor.
- `0x5202` supplies indexed maneuver descriptions/types, target roads and exit
  angles. The bounded cache joins the cursor to the correct maneuver; it does
  not guess from distance. A start-route pseudo-step can yield to the next
  actionable item in the supplied order.
- Input is validated before any state changes: framing, duplicate known fields,
  scalar widths, component identity, bounded counts and valid UTF-8. Unknown
  extensions remain opaque. Invalid optional metadata does not disconnect video.
- Source changes, rerouting and route cancellation clear cached guidance. A
  changed cursor cannot inherit the previous turn's distance. Updates are deltas,
  not a periodic heartbeat: an unchanged instruction persists during a healthy
  session, including while stationary. Disconnect clears the session explicitly.
- An in-process shared model passes snapshots to the native Standard cluster's
  existing render worker. A directions card sits below the map, alongside a
  compact now-playing card. Original artwork distinguishes full left/right turns,
  Keep and Slight variants, U-turn, Continue, route start and arrival. Arrival,
  loading, finding-location and updating-route states use their own card labels;
  a cached maneuver cannot retain a directional arrow in those status cards.
  Unknown and unqualified roundabout/exit types use a neutral marker and
  phone-provided text. No lane or junction geometry is inferred.

The feed is received in both Standard and Ultra sessions. This change draws it
in **our native cluster**; it does not inject a new card into Apple's signed
Ultra layouts. Lane diagrams, complete junction geometry, all 54 arrow variants,
spoken navigation audio and destination-timezone formatting remain separate work.
The existing phone-free cluster takeover limitation still applies.

The optional [native HUD](HUD.md) can also consume this same model in either
mode through `navigation_socket`. It registers no second phone service. Its
local publication retains the original accepted route/maneuver date, while a
separate bounded live-owner lease handles publisher loss. Stationary guidance
is still change-driven; local redraws and heartbeats are not new measurements.
This feed has no implemented speed-limit field.

## Configuration

Add these non-secret options to the private receiver JSON:

```json
{
  "route_guidance": true,
  "route_guidance_component_id": 42,
  "route_guidance_name": "Instrument cluster"
}
```

The feature defaults off. IDs must be 3..65535, distinct from the receiver's
Bluetooth/Wi-Fi component IDs 1 and 2. The name is 1..127 printable ASCII
characters. Fixed parser/storage limits are 256 UTF-8 bytes per text field,
128 cached maneuvers and 16 current indices. Unsupported lane capacity is zero.
All settings also have equivalent launcher arguments.

## Provenance and validation

This is original implementation code. No code from the original LIVI repository
or third-party receiver implementations is included. Apple publicly documents the
feature, but does not publish the accessory wire-field table. Protocol facts were
cross-checked against the authors' published research:

- [mib2q route-guidance TLV map](https://github.com/luka-dev/mib2q-carplay-rgi/blob/main/docs/rgd/rgd-tlv.md)
- [CPC200 iAP2 message catalog and capture analysis](https://github.com/lvalen91/CPC200-CCPA_resources/blob/main/documentation/02_Protocol_Reference/iap2_message_catalog.md)

Where older analysis differs (notably route-state meanings and maneuver cursor
versus count), the newer independently cross-checked map is used. These are
reverse-engineered facts, not a claim of Apple certification or MFi documentation.

Tests exercise subscription timing through the real reliable session engine,
identification capability, malformed messages, UTF-8, cursor changes, partial
updates, rerouting, unsupported sources, silent-session persistence and cluster drawing. Synthetic
fixtures are explicitly test data. A simulated visual preview can be generated:

```sh
build/headunit-native-cluster-snapshot /tmp/navigation.ppm --navigation-demo
```

On 6 October, the native glyph regression fails against the previous renderer
where full-turn, Keep and Slight artwork was identical. It passes with the
new artwork, including Continue variants, U-turn/start/arrival and status cards
with a retained maneuver. An actual Cairo contact sheet was inspected using
explicit synthetic guidance. These are portable source checks, outside V3/V5;
no new phone or physical-display qualification is claimed.

Live verification on 27 September 2026: the standalone CM5 receiver subscribed
on Bluetooth and received real Apple Maps guidance in both Ultra and Standard
sessions. The route supplied a left turn with changing metre distances, with
no rejected navigation updates. The first rendering check exposed an incorrect
five-second expiry: stationary guidance arrives as changes, not a heartbeat.
That expiry has been removed and covered by a silent-session regression test.
A capture of the CM5 native Standard cluster confirms the live map, left-turn
arrow, 5-metre distance and phone-supplied road name together. Vehicle readings
are explicitly simulated. This was an isolated virtual Wayland output because
the physical touchscreen was unpowered; it is not a physical-panel test.
Ending the route on the iPhone produced an unavailable snapshot and removed the
directions card from a second CM5 capture while map video continued. The private
receiver profile now enables guidance; the temporary Standard test was returned
to its previous Ultra configuration. Captures stay outside Git.

Validation: all 54 Mac tests passed; eight targeted CM5 checks passed. After the
live delta-update correction, the three affected Mac tests and CM5 route-guidance
test passed again, followed by the live render and route-end checks.
