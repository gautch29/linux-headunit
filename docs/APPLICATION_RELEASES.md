<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Application releases and rollback

A release keeps our executables, launcher dependencies and static configuration
together. Services use its exact directory. Installing the next release preserves
the previous binaries and settings; rollback selects their original paths.
There is no runtime `current` symlink.

This is a **local administrator workflow**, not signed distribution or a
complete Linux image updater. The selected CM5 generation and its exact binding
are recorded in [HANDOFF](HANDOFF.md). Historical qualification is recorded
[below](#qualification-and-boundaries). Current source work is not automatically
deployed. Start with [the handoff](HANDOFF.md) before changing a bench release.

## Release contents

Publication copies a fixed list of first-party executables, Python launcher
dependencies and service templates. Executables require a 64-bit little-endian
AArch64 ELF header. That rejects a Mac/different-CPU build; it does not prove
that the complete program runs on the CM5.

Static settings are supplied by typed roles:

| Role | Required when | Release file |
| --- | --- | --- |
| `receiver` | Always; `seconds` must be `0` | `config/receiver.json` |
| `displays` | Always; matches receiver `display_config` | `config/displays.conf` |
| `labwc` | Always | `config/labwc-cm5-dual-rc.xml` |
| `outputs` | Always | `config/kanshi-cm5-dual.conf` |
| `ultra` | Receiver supplies `ultra_installed_ui` | `config/installed-ui.conf` |
| `vehicle_profile` | Ultra supplies `vehicleProfile` | `config/vehicle-profile.plist` |
| `warnings` | Ultra supplies `warningOverlayProfile` | `config/warnings.json` |
| `climate` | Receiver supplies `climate_config` | `config/climate.json` |
| `car_settings` | Receiver supplies `car_settings_config` | `config/car-settings.json` |
| `fanout` | Optional physical vehicle distribution | `config/vehicle-fanout.json` |
| `camera` | Signed `cameraProfile` or native camera service | `config/camera.json` |
| `camera_state` | V5 signed camera; receiver `camera_state_config` | `config/camera-state.json` |
| `camera_manual` | V5 signed camera; manager `camera_manual_profile` | `config/camera-manual.json` |
| `phone_manager` | Version 3 managed receiver | `config/phone-manager.json` |
| `standard_displays` | Version 3; separate Standard main geometry | `config/standard-displays.conf` |
| `phone_page` | Version 3 with resident Home enabled | `config/phone-page.json` |

Only these known references are relocated. Omitted dependencies, unused roles
and two roles naming one source file are rejected. The source receiver's
`bin_dir` must name the selected repository's `build`; the captured receiver
then uses the release's `build`.

Identity, Wi-Fi credentials, pairing keys/secrets, bond stores, captures, signed
assets and acceptance references remain external. Media/audio state, page/layout
selection, vehicle sockets, camera request/lease files and native Home surface
request/lease files also stay external.
They cannot point into any release in the selected pool or alias a supplied
static file. Publication copies neither their contents nor live endpoint files.
Keep external asset/trust resources available for both retained releases.

Version 5 closes the managed signed camera graph with all three camera roles.
The capture must be enabled, explicit manual activation and the signed `camera`
input. Both `cameraControlFile` and `cameraStatusFile` must equal the manual
profile's request/status endpoints. All manual endpoints belong to the manager's
private `stateDirectory/camera` directory. Camera On is read-only, owner sourced
and dated from acquisition; its TTL exceeds the capture freshness limit.
The existing renderer owns capture/status, and the manager owns timed requests.
Standard and separate native camera owners remain rejected.

## Prepare a private recipe

The recipe must belong to root or the unprivileged display account and be owner-only. Use
exact absolute paths without spaces or service syntax, and list only referenced
roles. For example:

```json
{
  "version": 1,
  "release_id": "cm5-20261003-one",
  "source_revision": "0000000000000000000000000000000000000000",
  "repo": "/home/headunit/ultra-headunit",
  "user": "headunit",
  "configuration": {
    "receiver": "/home/headunit/.config/ultra-headunit/lab/receiver-appliance.json",
    "displays": "/home/headunit/.config/ultra-headunit/lab/displays.conf",
    "labwc": "/home/headunit/ultra-headunit/config/labwc-cm5-dual-rc.xml",
    "outputs": "/home/headunit/ultra-headunit/config/kanshi-cm5-dual.conf",
    "ultra": "/home/headunit/.config/ultra-headunit/lab/installed-ui.conf",
    "vehicle_profile": "/home/headunit/.config/ultra-headunit/lab/vehicle-profile.plist",
    "climate": "/home/headunit/.config/ultra-headunit/lab/climate.json",
    "car_settings": "/home/headunit/.config/ultra-headunit/lab/car-settings.json"
  },
  "native_camera": null
}
```

Replace the zero revision with the intended 40-character Git revision and the
paths with actual bench files. The revision records an administrator's build
label, not reproducible-build proof; hashes identify the exact copied payload.
Use a new release ID for each changed payload.

For a native camera service, add `camera` and replace `native_camera=null` with
an object containing absolute `request` and `lease` paths. They must equal
Ultra's `nativeCameraControlFile`/`nativeCameraLeaseFile`. The same profile cannot
also declare a signed `cameraProfile` input. Existing fanout/camera validators
check the cross-profile contracts.

Inputs must be bounded regular files with one link and trusted owners. Symlinks,
special files and group/other-writable inputs are rejected; source receiver and
Ultra profiles must additionally be owner-only.

Administrator preflight runs the receiver dry-run, manager/Home validation and
installed-UI probe as the manifest's declared display account, including its
groups. Private live endpoints must satisfy the intended runtime ownership;
checking them as root would reject a valid display-owned content directory.
Unit publication and selection remain administrator operations.
The privileged radio launcher also delegates vehicle capability checking to the
admitted receiver account, with a ten-second bound. This applies to every
manager Connect dry-run as well as administrator admission; the radio/AP owner
retains its existing privileges and the receiver still runs as the display user.

### Include the native manager and Home

Version 1 retains its original direct-receiver schema and inventory. For a
managed receiver, use `version: 3` and add this exact execution descriptor:

```json
{"execution": {"receiver": "phone-manager", "resident_home": true}}
```

Add these roles to the recipe's existing `configuration` object:

```json
{
  "phone_manager": "/home/headunit/.config/ultra-headunit/phone-manager.json",
  "standard_displays": "/home/headunit/.config/ultra-headunit/displays-standard.conf",
  "phone_page": "/home/headunit/.config/ultra-headunit/phone-page.json"
}
```

The manager profile must be enabled. Its `standardDisplayConfig` must name the
supplied Standard role; publication relocates that reference. Standard and Ultra
must target the same main connector and the same cluster geometry. The main
canvas dimensions may differ. Both display roles remain distinct even when
their bytes happen to match.

The optional page must be enabled, use `schemaVersion: 2` and target that main connector. Its
`managerSocket` must equal `stateDirectory + /manager.sock`; the receiver's
`media_state` must equal `stateDirectory + /media.json`. State remains external.
An enabled schema-1 page is rejected because it cannot coordinate ownership of
the main CarPlay surface. A disabled schema-1 page remains readable for legacy
development configuration; it cannot be selected as a resident release role.

### Share the main surface with Home

The enabled page and receiver must name the same two fixed endpoints beside
the manager socket. For a manager using `/var/lib/headunit/phone-manager`, the
page profile contains:

```json
{
  "schemaVersion": 2,
  "enabled": true,
  "managerSocket": "/var/lib/headunit/phone-manager/manager.sock",
  "surfaceControlFile": "/var/lib/headunit/phone-manager/home-surface.request",
  "surfaceLeaseFile": "/var/lib/headunit/phone-manager/home-surface.lease",
  "waylandDisplay": "wayland-0",
  "output": "HDMI-A-1",
  "homeButton": {"corner": "bottom-right", "margin": 16, "size": 64}
}
```

Add the matching pair to the receiver configuration:

```json
{
  "native_page_control_file": "/var/lib/headunit/phone-manager/home-surface.request",
  "native_page_lease_file": "/var/lib/headunit/phone-manager/home-surface.lease"
}
```

These fragments must match the actual manager directory and display routes.
Publication verifies the pair but creates neither endpoint. At runtime the
manager creates the private display-account-owned `0700` directory. Home owns
`page.lock`, publishes its initial OFF request and creates the private handoff
files. Main presenters retain a shared lease while showing CarPlay; Home waits
for exclusive ownership before placing its foreground page on that output.
Back withdraws Home before releasing ownership. The timed request expires if
Home stops renewing it. Missing or invalid control data conservatively hides
the guarded main presenter rather than allowing two foreground owners.

Standard mode derives `preview_sink: wayland-gpu` and uses the exact sealed
Standard display role. The launcher passes the pair through
`HEADUNIT_NATIVE_PAGE_CONTROL` and `HEADUNIT_NATIVE_PAGE_SURFACE_LEASE`.
Ultra instead receives `nativePageControlFile` and `nativePageLeaseFile` in a
private per-session installed-UI copy. The sealed original stays unchanged.
The original may omit both fields or contain the exact complete pair; orphaned,
partial or mismatching fields are rejected. Inherited handoff environment is
cleared, so it cannot select another page owner.
For a fresh installation, omit the original's two fields and let the launcher
derive them after the manager has created its state directory. If the static
original already declares them, Ultra's read-only C++ preflight also validates
the real private parent and any existing endpoint metadata; preflight does not
create a missing directory.

The handoff files stay outside the whole release pool and cannot alias static
inputs, manager/page locks, sockets or media/audio state. A release containing
this pair must also contain its enabled Home role and the sealed
`main_surface_handoff.py` dependency. To omit Home, set `resident_home: false`,
omit `phone_page`, remove both receiver handoff fields, and remove any corresponding
fields from the original installed-UI profile. Existing external handoff state
is not deleted during publication, selection or rollback.

Native Home generations seal `phone_home.py`, `phone_page_style.py` and
`phone_glass.py`. Use a matching recovery helper when publishing. The release
verifier enforces the declared module inventory, file digests and permissions.

Version 3 seals the manager, readiness and page import graph, the shared unit renderer and
their templates. It still uses one `headunit-receiver.service`; there is no
second receiver service. The guard refuses the reserved alternate owner names
`headunit-managed-receiver.service` and `headunit-phone-manager.service` when
found in the inspected unit directory. It neither removes those files nor
includes them in rollback ownership.

V2–V4 managed graphs retain their camera exclusions. V5 admits only the complete
signed camera graph described above, preserving resident Home's existing lease.
A partial source/state/manual graph, aliasing endpoints or competing camera owner
fails before publication.

### Per-phone runtime settings

The managed child reads the exact sealed receiver and manager profiles. It
offers both configured Standard/Ultra capabilities from those profiles and
accepts only a validated phone address, inherited manager lock descriptor, fresh readiness nonce
and optional desired audio gain/mute. It does not read a writable `managed-receiver.json` as privileged
configuration. A bare Version 3 receiver launch is allowed only for read-only
preflight; runtime requires the complete managed selector.

Each phone gets a separate pairing-store child directory. Media and audio
control/state paths are fixed beneath the configured manager state directory;
the manager owns media publication. Gain/mute selectors require a configured
audio sink. The child proves the actual retained lock identity before startup
and holds that descriptor through receiver cleanup. Phone selection cannot
substitute executables, display profiles, audio endpoints or service arguments.

### Retained production boot recovery

Version 4 uses `purpose: appliance`, a `boot_recovery` descriptor containing an
independent helper binding and the existing transaction directory, and
`execution.mode_policy: phone-authoritative`. Publish the helper separately
with `purpose: boot-recovery`, empty configuration, and null execution/camera/
boot descriptor. Its fixed first-party inventory is verified before activation.
The helper pool stays outside the application pool; helper replacement is
refused by default before recovery or candidate writes.

New publication and full unit admission require a recovery helper schema at
least as new as the application. `prepare` checks that compatibility before
launching any checker. In particular, schema-5 signed-camera applications need
a schema-5 helper with the corresponding first-party inventory. Integrity-only
verification remains available for retained incompatible candidates. The V80
operator mismatch is covered by regression checks; V81 uses a compatible helper.

First activation refuses live/transitioning owners or pending owner jobs;
subsequent live selection/rollback/recovery requires the unchanged successful
active/exited gate. Retained installers missing this new admission module refuse
when an activated generator exists; use current administrator tooling.
Selection holds the existing transaction lock and atomically activates one
hash-pinned generator before candidate unit writes. At boot it creates a
rejecting gate plus Requires/After dependencies for all seven managed owners,
including restored legacy units, before reading helper code. It checks bounded
manifest/inventory bytes and rejects unlisted modules before import. The actual
gate recovers the existing journal, verifies the resulting unit bundle and
resolves freshly queried optional jobs without a synchronous gated start.
Missing/damaged helper, lock conflict and malformed journals admit no owners.
Helper availability at generator time is required; retain it on the root
filesystem. CM5 activation, warm selection and physical rollback are separate
qualification steps; no sealed release is changed in place.

### Manifest compatibility

| Manifest | Execution graph |
| --- | --- |
| Version 1 | Original direct receiver and its exact inventory. |
| Version 2 | Retained managed graph; verification and rollback remain supported using that release's own sealed scripts. |
| Version 3 | Current managed graph, adding `private_state.py` and `carplay_readiness.py` to the manager imports. New managed publication requires this version. |

The version selects an exact inventory; missing files cannot be interpreted as
an older graph. The current launcher does not execute a Version 2 managed runtime
graph. Transaction journals retain their independent Version 2 schema.
A readiness release must select its manager, page and matching control probe
together. Installing only a new page over an older manager hides Home/Back
because there is no usable receiver readiness contract. Static recipes reject
derived runtime paths and launch nonces.

### Explicit V5 helper migration

`--install --migrate-boot-helper` admits a separately sealed V5 helper only with
all managed/reserved owners and the boot gate stopped, no queued owner jobs,
a verified sealed current rollback graph, unchanged journal owner, accounts and
helper pool. V4 helper inventories remain byte-exact and independently retained.
V5 recovery uses the current verified sealed unit binding to restore the matching
bootstrap after rollback or an interruption between bootstrap and unit writes.
A failed candidate keeps the existing unit journal/rollback mechanism; no second
journal or runtime owner is introduced. Bootstrap rollback alignment requires
stopped owners. Application selection does not restart services itself.

Portable tests cover closed graphs, explicit/cold admission, live/queued-owner
refusal, interrupted bootstrap, failed unit enable and exact V4 restoration.
Production selection and physical phone evidence must be recorded separately in
HANDOFF; these fixtures do not establish deployment or phone behavior.

## Publish and preflight

Build the Linux programs first. Provision labwc, kanshi, NetworkManager, BlueZ,
hostapd, dnsmasq, Python/GI and the executables' shared libraries separately.
Resident Home additionally needs GTK 4, the Gtk4LayerShell introspection package
and `libgtk4-layer-shell.so.0`; its loader preloads that exact library before
importing GTK. System libraries and packages are not copied.

The managed/Home executable inventory is:

| Executable | Required when |
| --- | --- |
| `headunit-control-probe` | Always |
| `headunit-airplay-advertiser` | Always |
| `headunit-cluster-wayland` | Always |
| `headunit-display-config-check` | Always |
| `headunit-identity-config-check` | Always |
| `headunit-installed-ui-probe` | The `ultra` role exists |
| `headunit-vehicle-config-check` | The `climate` or `car_settings` role exists |

Each input must be a regular, singly linked executable. A development build
directory that symlinks unchanged executables to a previous checkout cannot
be published. Supply independent regular copies of those intended executables,
or complete the coherent Linux build first; hardlinks are rejected too. The
manifest hashes the copied bytes, but its source revision label does not prove
that mixed builds implement compatible runtime features.
In particular, Home needs a control probe built with the Standard GPU backend
and an installed-UI probe built with the new main-surface handoff support. The
ELF-header check cannot detect an older executable lacking those features.

```sh
sudo install -d -m 0755 /opt/ultra-headunit /opt/ultra-headunit/releases
sudo python3 -B tools/install_appliance_release.py \
  --recipe /home/headunit/.config/ultra-headunit/release.json --stage-only
```

Publication locks the pool and validates/hashes all bounded inputs before
creating a staging directory. It measures the actual destination filesystem's
non-reserved available blocks, rounds each copied file and manifest separately,
allows directory metadata and leaves a 16 MiB publication reserve. Fixed-inode
filesystems also need room for the complete inventory. Insufficient or unknown
capacity rejects publication without creating a candidate or changing units.
Archive unused inputs explicitly; publication never deletes them automatically.
This is admission rather than a space reservation: concurrent writers can still
exhaust the filesystem, in which case normal failure cleanup removes the partial
candidate. A second bounded read must match the admitted input hashes, so a
changed executable or tool cannot silently expand or replace the admitted graph.

Publication then copies into a new staging directory, syncs files and
directories, then promotes the completed directory. It never replaces an existing
release ID. An interrupted/failed publication may leave a staging or published
candidate for administrator inspection. There is no automatic deletion of
rollback targets; the pool is bounded to 32 entries excluding its lock.

The manifest pins hashes, sizes, ownership, modes, inventory, account and path.
Missing/extra files—including Python modules or `__pycache__`—and changed files,
symlinks, hardlinks, unsafe ancestors or special files fail verification. Limits
are 128 MiB per executable and 512 MiB total.

Production files belong to root. Static settings are `0440`, readable by the
display account's primary group, inside a `0750` directory. Choose a group whose
members should have access. Display processes cannot chmod/rewrite these
inodes. The root orchestration service remains privileged. Stricter legacy readers admit them only through the explicit verified
release path. Root remains the administrator; guards detect later changes.

Preflight uses the published launcher and checkers. It validates receiver,
displays and Climate/settings, then checks Ultra's installed configuration,
vehicle/warning/camera profiles and verified asset as the display account.
It executes no scripts and connects no phone. Read-only Ultra validation checks
native-camera endpoint metadata without creating or locking a lease. Optional
fanout/native-camera roles must agree with the other profiles. Failed preflight
leaves the candidate available for inspection but selects no services.
Version 3 also runs the released manager and optional page in validate-only
mode. They create no state, contact no radio and load no GTK display. The page
preflight runs as the pinned display account. Actual runtime retains the
separate manager/page process owners. The page preflight checks schema-2 handoff
and both display routes; it does not acquire the live lease or demonstrate
compositor withdrawal, input alignment or CarPlay clock continuity.

## Select a published release

Use the exact path and digest printed by staging:

```sh
sudo python3 -B tools/install_appliance_release.py \
  --existing-release /opt/ultra-headunit/releases/cm5-20261003-one \
  --manifest-sha256 PRINTED_64_CHARACTER_DIGEST --install
```

Alternatively, `--recipe ... --install` publishes, checks and selects a new
release. The unit transaction records old/new service bytes, modes, boot enable
state and verified release bindings. Four core services plus optional vehicle/
camera services, or the Version 3 manager and optional Home, select one release. Complete command-token checks reject path
suffixes, repeated flags and conflicting guard directories.

Each new service start verifies the pinned payload and checks that the installed
managed units select one complete release. Python disables bytecode writes,
inherited search paths and user-site imports. Sealed receiver options cannot be
overridden with launcher flags; its binary directory must match the release.
Version 3 compares the entire expected unit text from the shared renderer,
including ordered preflights, account, environment and dependencies. A missing
guard or an extra preflight fails before the first startup effect.

Selection requests no explicit start, stop or restart. Existing processes keep
their old paths, but systemd's restart policies can adopt new units after a
crash, or a later deliberate restart/reboot. This is not an atomic live handover.

## Rollback and interrupted updates

Keep the recovery installer/checkout available separately. Use the printed
managed-unit snapshot:

```sh
sudo python3 -B tools/install_cm5_appliance.py \
  --rollback /var/lib/ultra-headunit/appliance/transaction-SNAPSHOT
```

Rollback verifies the target release before changing units. Damage in the new
release does not block an intact old target. A changed/missing target fails
before service mutations. Recovery loads neither the current receiver's
configuration nor a running receiver process.
Journal binding extraction remains structural; it does not execute or read the
discarded release. Full target bundle admission happens before restoration.

Legacy journals remain unit-only. Returning from the first versioned installation
to a legacy checkout cannot restore overwritten checkout binaries/settings.
Full application rollback requires both generations to be retained releases.

Interrupted unit changes recover explicitly or on the next installer invocation.
Units are written separately. Startup guards reject a partial/mixed versioned
bundle; they do not repair journals at boot. A complete new bundle may boot
before a pending enable-state transaction is recovered. Initial migration from
unguarded legacy services has weaker isolation. Automatic boot recovery,
deliberate power-cut tests and a health-qualified switch remain unfinished.

## Qualification and boundaries

### Component evidence

Portable tests exercise relocated paths, private-state exclusion, isolated
imports and the published guard. They cover file/manifest damage, path/argument
overrides, linked/special files, failed promotion/preflight, corruption before
applying, retained rollback and interrupted unit writes followed by explicit
recovery. Linux programs are represented by explicit ELF-header/subprocess
fixtures; the actual production display parser reads relocated files.

Resident-release tests execute the released read-only boot graph in an isolated
interpreter, require its full import graph and schema-2 handoff module, preserve
the Standard/Ultra endpoint pair, and leave sealed UI bytes and external state
unchanged. Separate CM5 preflight runs execute actual AArch64 programs; isolated
pointer/GPU fixtures establish their own compositor boundaries. None substitutes
for a physical phone workflow.

V3/V4 target builds retain disclosed GCC indentation and gauge-input
variant/optional diagnostic exceptions. V5 removes the gauge exception after
direct scalar construction passed GCC 14 checks; the global indentation
exception remains. New readiness helpers passed strict
Clang checks separately. An ELF header, a source-revision label and a successful
preflight do not establish a clean default build, reproducibility or complete
hardware compatibility.

### Remaining release acceptance

The earlier packaged test rendered Home without touchscreen activation, although
ordinary CarPlay taps aligned correctly. Earlier source-checkout Home/Back and
mode-switching successes are separate evidence. Physical workflows on the
current readiness generation remain open:

1. Both screens/touch and one Home/Back cycle in Standard and Ultra are now
   user-confirmed on retained V3. Diagnose the reported Back delay and qualify
   repeated transitions. This does not qualify the new unselected candidate.
2. Exercise native Disconnect/reconnect, phone-mode changes and phone-free fallback.
3. Reboot into that exact bound release, then verify manager recovery and preferred
   phone reconnection.
4. Roll back to an intact retained generation and verify its original service,
   configuration and phone behavior.

Automatic boot recovery, deliberate physical power-cut tests, audible output,
health-qualified activation and complete OS/firmware rollback remain unfinished.
Staging, preflight and selection do not complete those gates by themselves.


Optional software journey archives use receiver `trip_history: true` together
with `sport_telemetry: true`. The verifier reads the hash-bound receiver bytes
before admitting exact `tools/trip_history.py` inventory; enabled graphs missing
the module or disabled graphs carrying it reject. Legacy sport-only inventories
remain readable. Adoption needs a newly reviewed helper/config graph and a new
coherently qualified managed generation, never additions to a sealed release.
Runtime history stays external/private in the existing state directory.
[Archive contract and owner checks](TRIP_AND_HEADING.md#private-software-journey-archives--source-only-14-october-2026).
