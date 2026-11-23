<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Climate and car-settings capability catalogue

Use this inventory to distinguish a configurable control from a researched
protocol fact and a live-tested iPhone feature. It is not an exhaustive official
Apple specification. Apple can add services, resources and policy requirements.

## Evidence levels

- **Live minimal model:** temperature/fan and a Boolean car setting opened and
  accepted retained changes on the user's iPhone on 30 September 2026.
- **Implemented, not yet live-qualified:** the file builder creates the service
  and validates its typed state and writes; presentation still needs phone tests.
- **Research only:** identifiers or classes are known, but configuration does
  not expose them until their payloads and behavior are established.

The user also opened the expanded file profile on the same day and reported
that the additional controls and settings worked. The receiver accepted both
vehicle channels, configuration/subscriptions and subsequent writes without
channel failure. This confirms profile/page acceptance; it does not independently
qualify every individual control. The revised profile fixed inconsistent seat
level/On simulation and removed redundant Auto controls. The user confirmed seat
heating persists and the steering-wheel/power overlap is gone. Accepted applied
seat levels 3, 2, 1, 0 and zone-power toggles were recorded. Other controls and
resource variants retain their separate qualification requirements.

On 1 October a later top-button overlap was reproduced in the original phone
video. Omitting Vent cleared it while power, steering-wheel heating and seat
controls remained; omitting Vent's On alone did not. The current compatible
bench moves airflow direction to a car-settings selector using the same
provider binding. The user confirms clear Climate icons and retained airflow
selection after reconnecting with the paired files. The full profile remains an experimental catalogue, not an
all-controls layout qualification. [Bench files and evidence](VEHICLE_CONFIGURATION_GUIDE.md#cm5-bench-profile-and-airflow-placement).

[The qualification matrix](CLIMATE_QUALIFICATION.md) records presentation,
retained writes and remaining checks separately for each control and asset.
It also defines the next required-only Vent comparison; no phone result is
claimed for that new profile.

The public architectural source is
[Apple WWDC 2024 session 10111](https://developer.apple.com/videos/play/wwdc2024/10111/).
Numeric facts below were extracted from the published iOS 26.1 framework
analysis pinned to commit `90aa0cfe59d9682b4265e1354c8b19ec3c7823ab`.
Only identifiers, field shapes and observed getter types were used; its
decompiled implementation is not included in this repository.

## Climate inventory

Climate accessory type: `0x01000001`. Seat accessory type: `0x01500001`.
These are different accessory families. The local profile creates a Seat
accessory for each zone declaring seat controls.
[Accessory type map](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFAccessoryTypes.mm).

| Service | Type | Configuration | State |
| --- | --- | --- | --- |
| Cabin | `0x11000001` | `cabin.power/ac/max_ac/max_defrost` | Implemented; not live-qualified |
| Temperature | `0x11000002` | `zones[].temperature` | Original minimal model live-tested |
| SteeringWheelHeatingCooling | `0x11000003` | `steering_wheel` | Experimental |
| Defrost | `0x11000005` | `zones[].defrost` | Experimental |
| Vent | `0x11000006` | `zones[].vents` | Experimental; overlaps in current expanded Climate page; bench uses settings selector |
| Fan | `0x11000007` | `zones[].fan` | Original minimal model live-tested |
| AutoClimateControl | `0x11000008` | `zones[].automatic` (level only) | Implemented; intensity research only |
| ClimateControlsLocked | `0x11000010` | Not exposed | LockState enum not mapped to named choices |
| Recirculation | `0x11000011` | `zones[].recirculation` | Implemented; not live-qualified |
| ZonesSynced | `0x11000012` | `zones[].sync` | Implemented; provider must implement synchronization |
| ZoneOn | `0x11000014` | `zones[].power` | Simulated toggles accepted live |
| TemperatureLevel | `0x11000015` | `zones[].temperature_level` | Experimental |
| SeatHeatingCooling | `0x22000001` | `zones[].seat_heating_cooling` | Simulated levels 0–3 live-tested; cooling unqualified |
| SeatFan | `0x22000002` | `zones[].seat_fan` | Implemented; not live-qualified |

The service map establishes these names and identifiers; each corresponding
class provides the characteristic getter types.
[Service type map](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFServiceTypes.mm).

### Characteristic map

| Characteristic | Type | Value |
| --- | --- | --- |
| On | `0x30000002` | Boolean |
| AutoMode | `0x3000005f` | Boolean |
| VehicleLayoutKey | `0x36000065` | String |
| CurrentTemperature | `0x31000019` | Float |
| TargetTemperature | `0x31000017` | Float |
| FanLevel | `0x31000012` | UInt8 |
| HVACOn | `0x31000002` | Boolean |
| CompressorOn | `0x31000003` | Boolean |
| MaxACOn | `0x31000022` | Boolean |
| MaxDefrostOn | `0x31000029` | Boolean |
| HeatingCoolingLevel | `0x31000021` | Int32 |
| DefrostTypes | `0x31000014` | Bitmask |
| Defrost Level | `0x30000004` | UInt8 |
| Vent Combinations | `0x31000025` | Array |
| Vent CurrentIndex | `0x30000038` | UInt32 |
| Automatic Level | `0x31000024` | UInt8 |
| Automatic Intensity | `0x31000028` | Specialized enum, research only |
| Climate LockState | `0x42000009` | Specialized enum, research only |

Temperature/fan identifiers and their live values are recorded in
[CLIMATE_RESEARCH.md](CLIMATE_RESEARCH.md). Other facts are supported by the
specific framework classes:
[Cabin](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFCabin.mm),
[SeatHeatingCooling](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFSeatHeatingCooling.mm),
[SeatFan](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFSeatFan.mm),
[Defrost](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFDefrost.mm),
[Vent](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFVent.mm).

Defrost masks: blown air `1`, filament `2`. Vent masks: window `1`, upper `2`,
lower `4`, neck `8`, middle `16`. The file format exposes direction names and
builds those masks internally.
[Framework bitmask descriptions](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CarAccessoryFramework.mm).

The seat heating/cooling getter is explicitly signed Int32; it is not a Boolean
heater toggle. The class exposes a configurable range but does not establish
the full UI semantics of negative/positive values. The example's cooling range
is an experiment and must be qualified against the phone and eventual hardware.

### Metadata qualification

Boolean `0`, UInt8 `1`, floating-point `9` and String `10` worked in the minimal
model. Array metadata `12` is observed in the metadata validator. The builder
also uses UInt32 `3`, UInt64 `4` and Int32 `7` for the researched wider integer
families. These wider tags follow the inferred numeric-format ordering; their
exact name table has not been independently recovered. The expanded profile
using these tags was accepted live, but every wider-type control has not had a
separate write/readback test. Controls using them retain `experimental: true`
until their full behavior and resource presentation are qualified.
[Characteristic metadata validation](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFCharacteristicMetadata.mm),
[Int32 accessor](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFInt32Characteristic.mm).

Live acceptance of one family is not evidence that every asset/phone will
display every available control or that its artwork is present.

## Car-settings inventory

AutomakerSettings accessory: `0x05000001`.

| Service | Type | Configuration | State |
| --- | --- | --- | --- |
| BooleanSetting | `0x16000001` | `kind: boolean` | Original minimal switch live-tested |
| ButtonSetting | `0x16000002` | Not exposed | Action lifecycle/notifications unqualified |
| FloatSetting | `0x16000003` | `kind: float` | Experimental |
| SingleSelectSetting | `0x16000004` | `kind: select` | Airflow workaround retained live; other selections experimental |
| MultipleSelectSetting | `0x16000005` | `kind: multi_select` | Experimental |
| DeepLinkSetting | `0x16000006` | Not exposed | Content URL/punch-through routing unqualified |
| IntegerSetting | `0x16000007` | `kind: integer` | Experimental |
| StaticSetting | `0x16000015` | `kind: text` | Experimental; nested children not exposed |
| SingleSelectImageSetting | `0x16000016` | Not exposed | Image/resource payloads unqualified |
| MultipleSelectImageSetting | `0x16000017` | Not exposed | Image/resource payloads unqualified |
| SettingsSection | `0x16000020` | `sections[]` | Original minimal section live-tested |

These families come from the same pinned service type map. Configuring arbitrary
buttons or URLs as ordinary writable values would not implement their actual
action behavior, so unsupported families fail validation.

### Car media presence

Experimental car-settings JSON may include `media` with required `source_id`
(an identifier) and `source_label` (1–64 bytes). It declares fixed, read-only
CAF `Media` `0x03000001` with one `MediaSource` `0x14000006` (identifier
`0x30000019`, label `0x30000001`), and `NowPlayingInformation` `0x0F000001`
whose `NowPlaying` `0x14000001` names that source current (`0x32000023`).
The phone's Media app hosts the large Ultra cluster Now Playing page only for
a car declaring both; with mainAudio on the phone it draws the phone's own
metadata. Phone titles, artwork and playback state are never mirrored into the
car, and the semantic-type and playback-state enums are not declared. Omission
preserves the existing profile. V74 qualifies the MapsMedia page on a real
iPhone; see [cluster review](CLUSTER_STYLE_REVIEW.md#large-now-playing-and-content-navigation--11-october).

### Vehicle resource variants

Experimental car-settings JSON may include `vehicle_resources` with required
`exterior_style`, `interior_style` and `vehicle_layout` identifiers. They are
fixed, read-only strings for VehicleResources `0x01200002` / VehicleVariant
`0x20000001`, with characteristic types `0x48000003`, `0x48000004` and
`0x48000005`. They select the matching phone-side asset manifest; the accessory
renderer's signed ZIP alone is not that manifest. Unknown fields, partial
objects and non-identifier values are rejected. Omission preserves the existing
profile and does not invent a default car model.

The [DBX resource example](../config/car-settings.vehicle-resources.example.json)
is a bench fixture for the current regular pack, with unavailable tyre readings.
It is source capability evidence, not live widget pixels or fitted-car support.
No image-archive control or actuator is enabled. See
[vehicle resource findings](VEHICLE_VIEWS.md#phone-side-vehicle-resources--11-october).

### Shared presentation fields

| Field | Type | Configuration status |
| --- | --- | --- |
| Label | `0x30000001` | `label` |
| Identifier | `0x30000019` | `id` |
| SectionIdentifier | `0x36000028` | `section` |
| SortOrder | `0x30000003` | `order` |
| Description | `0x30000005` | `description` |
| SymbolName | `0x3000005e` | `symbol` |
| Disabled | `0x32000011` | `disabled` (also removes write authority) |
| Hidden | `0x36000023` | `hidden` |
| Category | `0x36000006` | Research; enum mapping missing |
| LimitableUIElement | `0x36000021` | Research; restriction policy missing |
| ProminenceInfo | `0x36000020` | Research; complex payload |
| ShowAudioBrandLogo | `0x36000011` | Research; resources not integrated |
| VehicleLayoutKey | `0x36000065` | Research for settings placement |
| DetailedDescription | `0x36000029` | Research; not an ordinary description string |

Section fields: Identifier `0x30000019`, Name `0x36000004`, Footer `0x30000016`,
SortOrder `0x30000003`.
[Base setting class](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFAutomakerSetting.mm),
[SettingsSection](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFSettingsSection.mm).

### Values, ranges and selections

Boolean uses On `0x30000002`; float uses Value `0x30000006`; integer uses Value
`0x30000010`. Numeric endpoint symbols use `0x36000025` and `0x36000026`;
integer StepperBarHidden uses `0x36000027`. Static user-visible text uses
`0x30000017`.

Select families use EntryList `0x36000024`. Each entry is a dictionary with
`disabled`, `userVisibleLabel` and optional `userVisibleDescription`/`symbolName`.
Single selection uses SelectedEntryIndex `0x30000061` (UInt8); multiple selection
uses SelectedEntryIndices `0x36000062` (array).
[Select entry shape](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFSelectSettingEntry.mm),
[SingleSelectSetting](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFSingleSelectSetting.mm),
[MultipleSelectSetting](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFMultipleSelectSetting.mm).

## Qualification checklist

For each newly enabled control, record: the profile revision, iPhone/OS, whether
the page opens, actual layout and artwork, incoming write IID/type, confirmed
applied state, subscription update and behavior after reconnect. Validate ranges,
off/auto states and provider loss. Preserve the working minimal profile as a
recovery option. Avoid treating framework class availability alone as a successful
end-to-end implementation.

The files cover the recovered scalar and selection models. Climate lock-state,
automatic-intensity names, actions, rich descriptions, image resources and
notifications remain explicit follow-up work rather than silently invented API.
