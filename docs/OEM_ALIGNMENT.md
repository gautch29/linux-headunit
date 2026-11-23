<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Align the head unit with Apple's CarPlay Ultra guidance

Reviewed 11 October 2026 against Apple's public
[design system session](https://developer.apple.com/videos/play/wwdc2024/10112/)
and [architecture session](https://developer.apple.com/videos/play/wwdc2024/10111/).
This is an implementation and qualification audit of this repository, not an
Apple certification result or a replacement for the automaker/MFi specification.
[README](../README.md) owns feature status and [HANDOFF](HANDOFF.md) owns deployment.

## Design audit

The design session describes adaptable instruments, a reserved dynamic content
area and coherent branding. It includes Maps, media, trips, tyres and assistance,
plus temporary vehicle content. Apply those targets through the verified pack;
its references alone do not prove a working phone route.

| Area | Repository result and remaining acceptance |
| --- | --- |
| Dynamic content | Map and ADAS are phone-confirmed in V63. V66 deploys opt-in body/tyre declarations and dismissal-aware requests. Phone subscriptions and live overlay/withdrawal pass, but widget contents are blank; resource/rendering acceptance remains open. Charging is omitted in the combustion profile. |
| Now Playing | Native metadata/artwork works. Keep phone music phone-owned; do not mirror it back as a vehicle radio accessory. Establish the signed centre route and independent title/artwork updates, stale clearing and return to Map. |
| Trips | Native dated trip observations and consumption conversion exist. New phone-facing Trip readings start unavailable. Qualify the phone page, units, ordering and selection independently of native success. |
| Speed and energy | Verify speed and fuel/state of charge remain readable in every active layout. Existing simulated gauges are not calibrated vehicle inputs. |
| Cruise and limiter | Dated contracts and source mappings exist. Qualify enabled/engaged/suspended states, targets and unavailable readings across all authored styles; do not infer engagement from vehicle speed. |
| Signs and alternate units | Keep road-sign space clear. Establish actual acquisition and widget routes; retain explicit conversions and source units. |
| Powertrain | Select capabilities for the fitted combustion, electric or hybrid vehicle. Do not advertise charging or infer transmission from RPM on every car. |
| Layouts and controls | Existing picker waits for phone confirmation. Add only catalogued content; test change/close/back, simultaneous alert placement and steering-wheel input once its event contract is verified. |
| Appearance | Keep authored palette/layout transitions, wallpaper and native typography coherent. Re-test day/night, black wallpaper, unbroken labels and contrasting warning colours on both physical outputs. |
| Brand and startup | Preserve startup branding × original Apple, startup lamp policy and signed animation timing. Do not allow temporary content over the startup scene. |

## Architecture audit

The architecture session separates vehicle instruments and warning overlays,
phone-rendered Remote UI and locally composed Punch-through. It describes
immutable session capabilities, dated synchronisation and useful instruments
before phone reconnection. Those ownership boundaries govern the implementation.

| Boundary | Repository result and remaining acceptance |
| --- | --- |
| Essential indicators | Shared native/Ultra warning vocabulary and startup policy exist. Physical lamp requests, ignition lamp check and final top turn-arrow placement remain to qualify. No unavailable signal is treated as an active warning or a healthy zero. |
| Local UI before phone | Native fallback is operational. Cached signed local gauges before phone detection remain open; establish disconnected timing and one-owner handover without replaying phone frames. |
| Remote UI resources | New bounded capability declarations cover trips, restraints, closures, tyre and charging readings. Layout/resource identities, enum meanings and consumption still require qualification. Omit unfitted systems and keep unknown measurements unavailable. |
| RequestContent | New manager intent is nonce/generation-bound, expires after two seconds and permits only dismissal through phone VDP or the explicitly bound local Close action. Both routes stay Off during startup; the scene consumes the same copied state. Renewal cannot reopen a dismissed command. No request is a presentation receipt. |
| Camera and assistance | V63 demonstrates physical USB main Punch-through and labelled simulated cluster lanes. Keep their identities separate from body status. Phone Close/focus, unplug recovery and sustained audio/video remain open. |
| Synchronisation | Preserve original acquisition and phone clock mapping, bounded queues and actual GPU feedback. Thirty fps is accepted for feature work; do not count stale redraws as fresh sixty fps. |
| Car commands | Existing requested/applied separation remains mandatory. New content readings are read-only; no seat, closure, tyre or charging actuator is enabled by these declarations. |
| Notifications | A content overlay is not the notification protocol. Establish bounded trigger/acknowledgement, replacement, severity, action and dismissal semantics before advertising remote notification controls. |
| Phone and audio loss | Existing generation retirement/recovery remains authoritative. Test content expiry, voice replacement and native fallback together; audible output and microphone acceptance need devices. |
| Appliance | Use immutable releases, validated complete inputs and exact rollback through the existing five owners. Retain CI pause and qualify locally/on CM5. |

## Work ownership

[ROADMAP](ROADMAP.md) is the sole remaining-work order. This audit supplies design
and ownership constraints, not a second backlog. Camera controls/focus,
remembered local gauges, top-arrow deployment and display formats are the user's
immediate priorities. Blank widget resources and signed Media/Trip follow in that
plan. Notifications and richer optional services require fitted equipment or a
concrete product need; their absence is not a mandatory feature expansion.

The public sessions establish the product target. They do not supply every wire
identifier, input event, resource layout or acceptance test. Unresolved contracts
stay explicit rather than being filled with guessed values.
