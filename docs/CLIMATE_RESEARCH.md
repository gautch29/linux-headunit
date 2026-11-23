<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Climate and automaker settings: research record

## File-driven capability model

### Follow-up: overlapping top button — 1 October 2026

The user later identified a pale icon overlapping the top driver Climate
button. A separately software-decoded original main-video frame shows the
same defect; it is not introduced by the head unit's GPU scene composition.
Removing driver-zone power alone, then additionally steering-wheel heating,
did not clear it. Restoring those controls and omitting only Vent cleared the
page, confirmed by the user. Restoring Vent without its optional On
characteristic reproduced the overlap. These comparisons do not establish
the internal iPhone rendering cause or that every Vent combination fails.

The compatible bench files now keep power, steering-wheel heating and seat
controls on Climate, while an automaker-settings selector uses the existing
`climate.driver.vents.index` provider binding for airflow direction. The full
experimental file still advertises Vent for further protocol/resource work.
The user reconnected with the paired bench files and confirmed clear Climate
icons and an airflow selection that stays changed in car settings. This is a
live simulated-provider check, not a physical HVAC or durable-storage check.
See [the bench profile and placement contract](VEHICLE_CONFIGURATION_GUIDE.md#cm5-bench-profile-and-airflow-placement).

On 30 September 2026 the receiver gained semantic Climate and car-settings
files, an explicit simulation provider and a provider interface for future car
I/O. The user reconnected and confirmed both minimal pages still open and retain
changes with the JSON profiles. This separately verifies the new file-driven path.

Expanded controls are tracked in [VEHICLE_CAPABILITIES.md](VEHICLE_CAPABILITIES.md),
including seat heating/cooling, seat fans, cabin/zone controls and additional
settings families. [The configuration guide](VEHICLE_CONFIGURATION_GUIDE.md)
documents the file schema and provider contract. Legacy fixture IDs below do
not apply to generated file-profile IIDs.

The expanded profile also loaded both Apple pages. Initial checks found seat
heating reverting and overlapping steering-wheel/power/Auto controls. A write
trace showed a requested seat level of 3 was accepted as 3 without error, while
the separate On state remained false. The simulator now keeps level and On
consistent, including zero/off and restoration of the previous level.

The example also stopped advertising a separate automatic-climate level beside
the fan's AutoMode and omitted steering-wheel AutoMode. An extra steering-wheel
layout characteristic was removed: that service has no VehicleLayoutKey getter.
After a fresh reconnect, the user confirmed the seat level stayed changed and
the overlap was gone. The trace recorded seat levels 3, 2, 1 and 0, plus zone
power toggles, all with matching applied replies and no rejection. This qualifies
that revised profile on this bench; every other control, negative cooling levels
and vehicle artwork still need individual checks.

Status: **minimal numeric Climate/settings model and independent transport
live-verified with the simulated CM5 bench on 30 September 2026.** This is
reverse-engineered evidence, not Apple documentation or a complete specification.

See [the developer guide](VEHICLE_STATE_GUIDE.md) for the integration procedure
and API reference. This document preserves the evidence and uncertainties.

## Evidence found on 30 September 2026

Public analysis of the iOS 26.1 CarAccessoryFramework supplied the accessory,
service and characteristic type identifiers below. Sources are pinned to
[commit 90aa0cfe](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/tree/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework).
`CAFAccessoryTypes` and `CAFServiceTypes` map names to types; the typed getters
in `CAFTemperature`, `CAFFan` and `CAFAutomakerSetting` corroborate characteristic
identifiers. The temperature/fan/layout getter identifiers also occur in the
[iOS 18.2 analysis](https://github.com/EthanArbuckle/iPhone17-1_18.2_22C152_Restore/tree/e26ed4563f78871c59d2d96856756a65d62517e5/System/Library/PrivateFrameworks/CarAccessoryFramework.framework).
Temperature's `On` property is newer than that older version.

Only protocol facts were recorded. No decompiled implementation, Apple binary,
original LIVI implementation, private phone capture or signed asset is included
in this repository.

### Types used by the candidate

| Entity | Type | Evidence |
| --- | --- | --- |
| Climate accessory | `0x01000001` | CAFAccessoryTypes |
| Temperature service | `0x11000002` | CAFServiceTypes, CAFClimate |
| Fan service | `0x11000007` | CAFServiceTypes, CAFClimate |
| CurrentTemperature | `0x31000019` | CAFTemperature |
| TargetTemperature | `0x31000017` | CAFTemperature |
| FanLevel | `0x31000012` | CAFFan |
| On | `0x30000002` | CAFTemperature, CAFFan, CAFBooleanSetting |
| FanAutoMode | `0x3000005f` | CAFFan |
| VehicleLayoutKey | `0x36000065` | CAFTemperature, CAFFan |
| AutomakerSettings accessory | `0x05000001` | CAFAccessoryTypes |
| BooleanSetting service | `0x16000001` | CAFServiceTypes |
| SettingsSection service | `0x16000020` | CAFServiceTypes |
| UserVisibleLabel | `0x30000001` | CAFAutomakerSetting |
| Identifier | `0x30000019` | CAFAutomakerSetting, CAFSettingsSection |
| SectionIdentifier | `0x36000028` | CAFAutomakerSetting |
| SortOrder | `0x30000003` | CAFAutomakerSetting, CAFSettingsSection |
| UserVisibleSectionName | `0x36000004` | CAFSettingsSection |

Types identify semantics. Instance identifiers (IIDs) are our own unique local
numbers, not Apple's type identifiers. The bench uses Climate IIDs 100–115 and
settings IIDs 200–214. Unused numbers in those ranges are not advertised.

### Metadata and values

`CAFCharacteristicMetadata` consumes numeric `format`, `units`,
`minimumValue`, `maximumValue`, `stepValue` and `maximumLength`. Formats 1–9
require range metadata; strings require a length. Earlier symbolic profiles
with names and `format="float"` did not establish this contract.

Local CarPlay simulator SDK call-site inspection corroborates Boolean format
0, UInt8 format 1 and String format 10. **Float format 9 remains a candidate**:
the numeric format family and typed measurement getter support it, but the
framework's format-name pointer table has not been recovered. The live phone
accepted this format for the temperature controls and performed writes; that
verifies this profile, not the complete format enum. Temperature unit 1082 maps to Celsius
in `NSUnitFromCAFUnitType`.

Our advertised temperature range (16–30 °C, half-degree steps), fan percentage
(0–100) and front-left zone are bench choices. They are not mandatory Apple
values. `CurrentTemperature` follows the applied simulated setpoint; it is
not a physical cabin-temperature measurement. Simulated values persist within
one connection owner, including a vehicle-channel reopen; a new phone control
connection starts at the defaults.

## Independent exchange

The receiver advertises `vehicleStateProtocolInfo` in `/info` and enables
`vehicleStateProtocol` only when the phone offers it. Both priority DataStream
UUIDs are registered before SETUP:

- Ordinary: `3E2F3C61-AAD0-42CB-A8AA-BF22186DA62E`
- High: `FF4A6720-F2BE-4F56-A3E1-DB3B4E37D634`

The authenticated socket codec already handles encryption and reassembly.
`vehicle::Transport` processes the nested OPACK plugin envelope. A synchronous
transport receipt (`rply`, matching group/token) is separate from the
asynchronous application response (`cmnd`). Both incoming `comm` and `cmnd`
labels are accepted. These channel/envelope facts come from the earlier SDK
observations, not from the newly discovered semantic type table.

`vehicle::Session` owns one model across the two sockets. It answers
configuration, read, register, unregister and single-characteristic write
requests. Writes validate type, range and step before touching simulated state;
the reply contains the applied value and subscriptions receive changes.
No car hardware is called. No control services or batch writes are advertised.
Output queues are bounded and reserve reply space before executing a writer.
A malformed or stalled optional channel closes independently of video/UI.
Updates currently prefer the ordinary channel; per-characteristic priority
routing and per-channel subscription ownership need further protocol evidence.

The initialized profile supplied through `/info` and `configResponse` uses one
builder, avoiding two conflicting configurations.

## Reproduce the bench test

Set `vehicle_state_profile` in a private receiver JSON profile:

| Value | Meaning |
| --- | --- |
| `none` | Default; vehicle-state research path absent |
| `empty` | Vehicle-state transport with no accessories, useful as a control |
| `climate-bench` | One simulated temperature/fan zone |
| `settings-bench` | One simulated comfort-lighting Boolean setting and section |
| `bench` | Both candidate accessories |

The Climate/settings profiles require the existing native Ultra receiver and
verified local asset. Restart the receiver and reconnect the phone because
vehicle configuration is fixed for a session. Keep the working profile for
rollback. Observe channel/request counts in the receiver log; use USB phone
logs to distinguish schema initialization failures from resource failures.

Before claiming functional Climate, verify:

1. Both normal CarPlay views still work.
2. Apple's Climate page opens beyond the loading screen.
3. Temperature/fan actions reach the correct IIDs and applied state returns.
4. Phone-side changes and reconnection preserve consistent displayed state.
5. Car settings lists the simulated switch and reflects its applied value.

## Remaining gaps

- The minimal model loads without Cabin or vent services on the tested phone.
  Requirements for additional zones and controls remain unverified.
- Vehicle resources/variant matching remains unresolved. The discovered
  VehicleResources accessory (`0x01200002`) and VehicleVariant service
  (`0x20000001`) do not establish a usable resource archive or variant strings.
- Settings category/prominence are not included: their complete enum/data
  contract has not been established. The minimal Boolean menu worked without
  them; other menu types remain unverified.
- The final HVAC provider interface, physical readings, command authority,
  actuator acknowledgements and persistence are separate integration work.
- No claim of compatibility with every iOS version or signed asset is made.

## Live qualification, 30 September 2026

The combined `bench` profile opened both vehicle-state channels. The phone
requested configuration and registered subscriptions; the receiver observed
control-write requests without a vehicle-channel failure. The user confirmed:

1. Apple's Climate page displayed temperature and fan controls.
2. Both controls changed and retained their displayed values.
3. Apple's settings interface displayed **Headunit bench / Comfort lighting
   (simulation)**, and the switch retained its changed state.

A separate touch fault initially prevented testing: the labwc rule matched
the kernel device name, while libinput appended a USB-port suffix. Both the
explicit name and an explicit empty-string name left the touchscreen spanning
the two-monitor desktop. Omitting `deviceName` selected the fallback rule for
`HDMI-A-1`; the user then confirmed aligned touch. It was not a Climate-protocol
failure. Bounded coordinate diagnostics now help distinguish those failures.

Phone startup logs also recorded CarAssetUtils resource-resolution warnings,
with the resource/variant redacted. Those did not prevent the later successful
Climate test. The reference ZIP contains no `manifest.json` or named
Climate/vehicle-layout resource entries. The publicly analyzed
[CarAssetUtils framework](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/tree/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAssetUtils.framework/CarAssetUtils)
has a separate manifest-backed artwork model; its full integration
requirements are not established here. No VehicleResources capability was
added for this minimal test.

Apple's [WWDC vehicle integration session](https://developer.apple.com/videos/play/wwdc2024/10111/)
explains the intended model: configured controls and layout keys, vehicle
resources, current state, requested changes and reporting actual state. It
also describes configuring built-in automaker settings; a separate iPhone app
is not intrinsically required for every settings item.
