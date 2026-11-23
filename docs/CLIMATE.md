<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Climate and car settings

**Apple's Climate and automaker settings pages work with the simulated bench
profile.** On 30 September 2026, the user opened Climate, changed temperature
and fan, and confirmed both stayed changed. The Headunit bench comfort-lighting
switch also appeared in Apple's settings interface and retained its changed
state. Earlier symbolic profiles had exposed an icon without loading controls.

The independent receiver now has an opt-in vehicle-state transport and
simulated Climate/settings profiles using numeric types found in public iOS
framework analysis. It supports configuration, read/subscription exchanges,
validated single-value writes and applied-state updates. The simulated
controls cover a front-left temperature/fan zone and a comfort-lighting switch.
This is live verification of the minimal simulated profile with the tested
phone and reference pack, not a complete physical HVAC integration.

Read [Integrating Climate and vehicle settings](VEHICLE_STATE_GUIDE.md) for the
developer guide, model, wire exchanges and backend API. See
[the research record](CLIMATE_RESEARCH.md) for pinned sources, type tables,
uncertainties, backend ownership, test procedure and remaining resource gaps.
The example receiver now uses separate semantic Climate and settings files with
an explicit simulation provider. See [the configuration guide](VEHICLE_CONFIGURATION_GUIDE.md)
for available zones, seats, ranges and settings, and [the capability catalogue](VEHICLE_CAPABILITIES.md)
for research status. New capability families still require live qualification.

The phone-facing service is distinct from `VehicleSource`, which supplies our
native cluster's speed, RPM and other readings. No physical HVAC or car-settings
actuator is connected to the research model.
