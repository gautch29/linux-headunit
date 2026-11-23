<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Qualify Climate, settings and asset compatibility

## Synchronous provider lifetime regression — 15 October, source only

`vehicle-configuration` combines the production `ConfiguredModel`, OPACK
request endpoint and subscription owner with a controlled synchronous provider.
It checks off-grid, out-of-range, wrong-type and absent confirmations, thrown
rejection while available or unavailable, and an actual value different from
the request. Failed writes produce errors without publishing requested state.
The read-only temperature alias changes on the next provider sampling turn;
loss withdraws both aliases, equal-date replay stays withdrawn, and a newer
acquisition recovers them. Disconnect clears subscriptions while retaining the
provider's own state. A fresh model with a different provider and the same
generated IIDs starts unavailable and cannot inherit the old provider's values
or subscriptions.

This adds regression coverage without changing runtime behavior or capability
files. The synchronous provider has no pending completion to deliver after
destruction. No new confirmation/generation API, UI keys, actions, IDs or UI
pieces are introduced. These are component fixtures, not physical HVAC,
phone presentation or deployment evidence.

## V84 bounded presentation recheck — 14 October

Frozen `3bd3db9 real iPhone, existing virtual pointer: Climate opens over Maps;
X at the observed sidebar target dismisses it back to Maps. Both-panel Ready and
five owner identities remain unchanged. This supersedes the scoped V83 failed
X observation, without claiming a diagnosed code fix or broad dismissal soak.
All six native Home tabs also retain both Ready panels. Original before/after
pixels and USB log: private `20261013/phone-session/v84-final-climate-*` and
`v84-final-ui-*`. Physical touch, advanced writes and actual HVAC remain open.

**Integration record · 1 October 2026; scoped V47 check · 10 October 2026**

Separate capability construction, presentation, retained writes and actual
hardware operation. A working page is useful evidence, but it does not qualify
every control on it. This is project documentation, not an Apple specification.

## V47 retained simulated temperature check

Selected V47 (`e0a3e56`) receives the same phone's Climate popover and full
Climate app on the powered main output. A bounded Wayland virtual pointer
changes the driver's displayed simulated setpoint from 21.0 to 21.5 °C;
closing and reopening the popover retains 21.5. A decrement restores 21.0
in the popover and launcher; the passenger stays 21.0. The popover's ellipsis
opens the full two-zone Climate app, and the sidebar opens CarPlay Settings.
The selected phone, two pairings and publisher/connection generation remain
unchanged across these checks. Several early or expired-popover captures are
retained as inconclusive attempts; the settled captures establish the stated
values and pages. Private receipts are in `overnight/functional-v47-*` and
`v47-functional-summary.json` under the handoff's qualification directory.

This is visible simulated retention and page-opening evidence. It does not
qualify physical HVAC applied state, advanced car-settings writes, every
Climate control, physical touch or airflow inside Climate. The earlier matrix
and its remaining gates continue to apply.

## Control matrix

The compatible reference-pack bench is the paired
[Climate](../config/climate.cm5-bench.example.json) and
[settings](../config/car-settings.cm5-bench.example.json) profile. The full
examples are research inventories. No physical HVAC provider is installed.

| Control | Offline implementation | Recorded phone result | Remaining gate |
| --- | --- | --- | --- |
| Temperature setpoint | Float, units, range/step, applied-state validation | Minimal and file-driven pages open; user confirms retained changes | Independent measured input, units variants and hardware |
| CurrentTemperature | Read-only; setpoint alias by default; independent binding and optional measurement bounds/step | Default alias used by the working pages | Independent measurement presentation, unavailable sensor and differing measurement range |
| Fan | UInt8 level, optional On/Auto; level/power simulation relationship | User confirms retained fan changes | Individually check On, Auto and non-percentage fan ranges |
| Cabin HVAC power, A/C, Max A/C, Max defrost | Four independent Boolean provider bindings | Expanded page accepted; no isolated write evidence for every switch | Each switch, interlocks, unavailable provider and actual equipment |
| Zone power | Boolean On, fixed layout key | Applied simulated toggles recorded | Hardware and dependent state updates |
| Steering-wheel heating/cooling | Signed level, optional Auto, no separate On | Visible in expanded profile; removing it did not clear the later overlap | Each level, retained write and optional Auto |
| Seat heating/cooling | Signed level and optional On/Auto in a Seat accessory | Simulated heating 3/2/1/0 retained; user confirms corrected level behavior | Negative cooling levels, every zone and Auto |
| Seat fan | UInt8 level in a Seat accessory; optional On/Auto | Expanded page includes it; no independently isolated retained-write record | Level/On/Auto, every zone and cooling artwork |
| Recirculation | Boolean On, optional Auto | Expanded model/page accepted | Individual On/Auto changes and retained notifications |
| Zone synchronization | Boolean On | Expanded model accepted | Retained write and coordinated temperatures from the provider |
| Defrost | Types bitmask and UInt8 level | Expanded model accepted | Air/filament variants, range and retained writes |
| Vent direction in Climate | Array of direction masks, UInt32 current index, optional On/Auto | Expanded combination overlaps; removing On alone still overlaps | Required-only isolated profile, retained indices and readable icons |
| Airflow settings workaround | SingleSelectSetting sharing `climate.driver.vents.index` | User confirms clear Climate icons and retained selection in settings | Return the control to Climate; qualify physical actuator mapping |
| Automatic-climate level | UInt8 level | No separate live test | Level states and comparison with fan Auto |
| Relative temperature level | Signed level with optional On/Auto | No separate live test | Exact presentation, signs/levels and retained writes |
| Climate control locking / automatic intensity | Not exposed; semantic enum contracts incomplete | None | Recover named values and behavior before exposing configuration |

### Car settings

| Family | Offline implementation | Recorded phone result | Remaining gate |
| --- | --- | --- | --- |
| Section + Boolean | Stable identifiers, labels, ordering and applied state | Comfort-lighting switch opens and retains changes | Real provider and persistence policy |
| Single select | Choices, disabled entries, UInt8 index and bounds | Driver-airflow workaround retains selection | Other choices/resources, disabled entries and reconnect state |
| Float / integer | Explicit bounds/step and write validation | Expanded model accepted, not each control separately | Retained values, increments, endpoint symbols and provider loss |
| Multiple select | Unique permitted indices; disabled/out-of-range rejection | No isolated phone result | Multi-selection presentation and retained array writes |
| Static text | Read-only timed string | No isolated phone result | Changing text and unavailable state |
| Buttons, links, image selections, notifications, prominence/restriction policies | Not exposed | None | Action/resource/lifecycle contracts; do not substitute ordinary scalar writes |

The common model checks request type, bounds, increments and disabled choices
before calling the provider. Provider confirmations receive the same checks.
The configuration tests cover retained setpoints and seat levels, rejected
writes, source loss, independent temperature precision and coherent aliases.
That evidence does not qualify physical command acknowledgements.

## Restore airflow inside Climate

The observed overlap is already present in the iPhone's video, before the local
scene renderer. The following comparisons are recorded in
[the integration guide](VEHICLE_CONFIGURATION_GUIDE.md#cm5-bench-profile-and-airflow-placement):

| Comparison in the expanded model | Result |
| --- | --- |
| Remove steering-wheel heating and/or zone power | Overlap remains |
| Remove Vent, restore seats/wheel/power | Icons become separate |
| Restore Vent but omit only its On characteristic | Overlap remains |
| Omit Vent and expose the existing binding through settings | Icons separate; airflow selection retained |

The last Vent comparison still contained its AutoMode characteristic and the
other advanced services. It does not establish whether a required-only Vent
service overlaps on its own. That is the next comparison with a clear purpose:

1. Keep the accepted asset, identity, display geometry and temperature/fan
   baseline fixed. Use
   [the isolation profile](../config/climate.vent-isolation.example.json) with
   the same known direction masks. Do not change transport identifiers.
2. In a fresh phone session, inspect the driver button and select all four
   directions. Match each request to the index's applied response and subsequent
   subscription update. Require readable icons and retained selection.
3. If this minimal service is clear, add Vent On, then AutoMode, then the other
   capabilities one at a time. The first failing addition identifies the
   incompatible combination. A smaller profile is a diagnostic, not the final
   replacement for the car's declared equipment.
4. If it still overlaps, capture the original phone frame and resource-resolution
   diagnostics. Compare a second supported iOS/phone if available. Keep the
   working settings workaround while investigating resources/variant mapping.

No step above has a new live result yet. The isolation profile passes the same
configuration/model checker and all four index writes locally. Its raw Vent
shape uses only the already researched combinations/index/layout characteristics;
it adds no guessed numeric identifiers.

## Separate cluster packages from vehicle artwork

The four downloaded archives have been inspected again locally. Their
`r/Center_Display` files are byte-identical: seven files per pack, including
three PNGs and four source/compiled resource-table/font files. They contain no
`manifest.json` and no explicitly named Climate/Vent/Seat resources. The audit
is private under `captures/climate-capability-audit-20261001/pack-resource-audit.json`.
Names alone do not establish what every hashed image means. The matching center
files show that these variants do not offer an alternative set of center-display
resources to test for this overlap.

CarAssetUtils has a separate manifest-backed vehicle-artwork model, already
noted in [the research record](CLIMATE_RESEARCH.md#live-qualification-30-september-2026).
VehicleResources/VehicleVariant identifiers alone do not establish the archive
manifest, variants, delivery mechanism or ownership contract. Supplying invented
variant strings would not qualify that integration. Neither the public
[Apple architecture session](https://developer.apple.com/videos/play/wwdc2024/10111/)
nor the currently recovered model supplies a complete vehicle-resource schema.

The diagnostic route therefore begins with the isolated capability combination;
it proceeds to the resource contract if phone evidence requires it. Changing the
signed cluster artwork, certificate policy or gauge renderer has no established
connection to the overlapping native Climate button.

## Pack matrix

All four are Aston Martin family packages. CN's exact meaning is unverified.
Expiry exceptions are explicit development choices; none is newly strict-qualified
with a currently valid signing leaf. See [asset provenance](ASSET_DISCOVERY.md).

| Pack | Offline cluster evidence | Recorded phone evidence | Remaining compatibility gates |
| --- | --- | --- | --- |
| `44C9FA31EC / 985` | All six layouts, both appearances and richer readings render at 1920 × 720 | Two panels, maps, touch, five live style switches, compatible Climate/settings | Every layout/content/appearance combination; new richer readings and fault recovery |
| `44C9FA31EC-Single / 547` | Nine layouts in both appearances render; three extra layouts pass GPU preflight | Two views, Single gauge, main touch and Climate page | Retained advanced controls/settings, live style switches, maps and sustained operation |
| `44C9FA31ECCN / 985` | Regular layout/artwork family; identity and configuration checks pass | Both panels appear | Touch/maps/control writes and sustained operation under this exact identity |
| `44C9FA31ECCN-Single / 547` | Single layout/artwork family; identity/configuration checks pass | Both panels appear with SingleDigiBandTicks | Live switches across all nine layouts, maps/touch, control writes and recovery |

The substantive Single pack retains six regular styles and adds SingleDigiRing,
SingleDigiBandTicks and SingleDigiBand. The regular and Single day/night rendering
reports cover 30 local style/appearance combinations. CN variants share their
respective family's artwork; family equivalence is not a separate phone test.
Passing the configuration parser, drawing offline or accepting one startup style
does not prove the full UI-sync/control exchange for every live transition.

### Record each new qualification

Record exact pack identifier/version, certificate policy, profile files, phone
and OS, layout UUID and control binding. Capture page/frame presentation, write
request, applied response, subscription update and reconnect behavior. For hardware,
also record the actual applied actuator state and unavailable/fault behavior.
Keep captures and third-party signed archives outside Git. Update this matrix
only with evidence that meets the specific gate.
