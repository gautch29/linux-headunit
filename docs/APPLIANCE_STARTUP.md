<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# CM5 appliance startup and backup cluster

The CM5 bench boots Raspberry Pi OS Lite into a two-output Wayland session.
The selected [retained application release](APPLICATION_RELEASES.md) runs
**five** supervised services: compositor, output configuration, native cluster,
the phone manager through `headunit-receiver.service`, and a resident native
Home page through `headunit-phone-page.service`. All five selected the same
release and are active after the 8 October warm reboot; see
[handoff](HANDOFF.md) for the exact release and current observation. This does
not establish a qualified cold boot, physical Home/Back workflow or reliable cold-power
reconnection. The receiver profile sets `seconds` to `0`, so it has no
development time limit. The touchscreen is HDMI-A-1; the cluster is HDMI-A-2.
Both displays should be powered before boot. This setup has no desktop or
Weston dependency; labwc owns the two outputs and routes touch.

The native backup cluster is a background-layer surface on HDMI-A-2. It uses
the same gauge design as the Standard CarPlay cluster, with switchable Media
and Vehicle center pages. Map becomes available to the Standard cluster during
a phone session; when disconnected, a saved Map selection resolves to Media.
See [native cluster pages](NATIVE_CLUSTER_PAGES.md). An Ultra cluster window
sits above it while CarPlay is healthy. When the phone session
or a failed Ultra worker closes that window, the already-running native cluster
is exposed. A live iPhone disconnect exposed the backup without restarting a
service; a separate stalled-window case has not yet been induced. The backup
draws its authored 960 × 360 pixels and uses Wayland viewport scaling to fit
the configured 1920 × 720 canvas. The earlier minimal
backup used about 6% of one core; the full native gauges take more CPU. It
remains a **simulated** cluster until a physical vehicle-data source is connected;
do not interpret its values as car measurements.

## Boot timing and parked standby

[Boot speed](BOOT_SPEED.md) owns the 9 October read-only kernel timeline, ranked
source optimizations, component evidence and exact bounded live comparison.
The new managed admission/network overlap and removed duplicate page preflight
are source changes; no candidate is selected and no end-to-end saving is claimed.

The boot optimization keeps one radio/display graph and its sealed integrity
checks. Completed cloud-init provisioning is disabled, the unused online-network
wait is masked, and unused removable-storage automation is disabled. Local
user-session admission no longer waits for a network link; local home and
filesystem/account ordering remain. NetworkManager still owns Ethernet
management and enables the Wi-Fi radio before the existing AP launcher. These
OS settings are outside the application manifest, with exact prior-state receipts
in the handoff. Do not remove firmware loading, BlueZ, discovery, DHCP, output
configuration or the retained boot gate simply to shorten `systemd-analyze`.
Measure actual startup branding, Bluetooth acceptance, authentication, control and real
presentation-ready stages separately. Warm kernel timing excludes firmware and
cannot establish ignition-to-picture time.

On 8 October the installed CM5 kernel's `/sys/power/state` is empty and
`/sys/power/mem_sleep` is absent: suspend-to-RAM is unavailable on this bench.
Raspberry Pi's [documented shutdown/VPU sleep settings](https://www.raspberrypi.com/documentation/computers/configuration.html#configure-shutdown-behaviour) describe shutdown behavior,
not preservation of a running Linux/CarPlay session. A short-stop powered standby
would keep Linux initialized, retire phone ownership when parked, mute/power down
displays and audio, then reconnect the selected phone on wake. It is a proposed
future path, not an installed sleep service or measured instant reconnect.

The hardware power specification
owns ignition, MCU sequencing, battery floor and whole-input parked drain. A
bounded short-stop standby would be a separately measured policy; long parking
must still reach the hardware project's low-current state with the CM5 off.
The current bench has no qualified ignition or battery provider, so Linux cannot
infer car-off or enforce that parked-power policy yet.

## V47 warm-boot checks — 10 October 2026

V47 (`e0a3e56`) repeats the complete private headless startup fixture: 21/21
checks in 59.104 s, with original deadlines and four native GTK regressions.
That software-compositor fixture uses synthetic underlays and no physical
radio/display owner. It is component evidence.

Three actual CM5 warm reboots separately preserve the selected sealed graph,
two pairings and original phone, with both screens on. Each post-boot verification
covers sealed payloads and all five installed units; all five services are
active with `NRestarts=0` at the scoped observation.

| Attempt | First observed fresh confirmed Ready after kernel boot | Subsequent 20-second tail |
| --- | --- | --- |
| 1 | 37.819 s; misses 30 s target | No sampled loss/error/generation change |
| 2 | 24.874 s; meets 30 s target | Fails: source-clock retirement, 37 not-Ready samples |
| 3 | 20.471 s; meets 30 s target | No sampled loss/error/generation change |

Attempt 1's driver exits on an SSH timeout after dispatching the real reboot;
observation resumes on that same boot at 24.85 kernel seconds. The earlier
interval and host dispatch timestamp are unavailable. Later drivers catch and
count SSH timeouts; no extra reboot is substituted. Attempt 2's automatic
recovery is a separate observation on a new timeline, not a stable-first-session
pass. [Phone clock](PHONE_CLOCK.md#v47-warm-boot-phase-rejection--10-october-2026)
owns its measured rejection and recovery gap. Private receipts are in
`overnight/warm-v47-warm-{1b,2,3}` and associated driver directories.

These are polling bounds for real readiness. They do not establish optical
first picture, cold-power/ignition timing, a compiler or boot-speed comparison,
physical touch, or sustained fresh fps. Phone screen and charging state are
unobserved. Broader boot/reconnect reliability remains open.

## Selected release and legacy checkout installation

The [startup branding startup animation](STARTUP_SPLASH.md) plays once per early kernel
boot on both physical outputs from the existing retained native phone page.
It builds bounded GTK/GSK GPU vector scenes at each output's native dimensions
on layer-shell BOTTOM surfaces, with empty input regions under ordinary phone
windows. Completion, existing phone readiness and failures retire the decoration.
The current
boot graph admits the animation before starting the native cluster and starts
the receiver in parallel; [startup ordering](STARTUP_SPLASH.md#first-visible-content-and-parallel-connection)
owns its notification, timeout and console boundaries.

For the current CM5, use the retained-release publication, selection and
rollback procedure in [application releases](APPLICATION_RELEASES.md). The
selected release seals binaries, static configuration, launcher dependencies
and service bindings together; do not edit it in place or use the checkout
installer below to change that deployed generation. Confirm all five service
paths before a physical test. A successful verifier and active service state do
not prove the physical touchscreen, Home/Back/Disconnect, cold boot or
preferred-phone recovery on this release.

The older source-checkout installer remains useful for a separate lab setup.
It installs **four** direct-receiver services by default and does not select a
retained application release:

Build the project with Wayland enabled, install labwc and kanshi, and provide a
private receiver configuration under the display user's home directory. The
configuration must be owned by that user, mode 0600, and set `seconds` to `0`.
Set display names, dimensions and output routing for the actual monitors in the
existing configuration files. Then install the system services:

```sh
sudo python3 tools/install_cm5_appliance.py \
  --user headunit --repo /home/headunit/ultra-headunit \
  --receiver-config /home/headunit/.config/ultra-headunit/lab/receiver-appliance.json \
  --install
sudo systemd-analyze verify /etc/systemd/system/headunit-*.service
sudo systemctl start headunit-compositor.service
sudo systemctl start headunit-outputs.service headunit-cluster.service
sudo systemctl start headunit-receiver.service
```

The checkout installer enables all four direct-receiver services for subsequent
boots but does not start or restart an existing session. The receiver unit
enables Wi-Fi through NetworkManager before starting the access point. The
cluster waits for its named Wayland output rather than racing the compositor at boot.

In this legacy checkout path, optional `--phone-manager-profile` replaces the
direct launcher in the same receiver service with the native phone manager. It
keeps the external cluster service separate and requires persistent sessions
and a shared media path. It is disabled by default in the checkout example.
The **retained V3 release**
does run that manager and its Home page, with verified service paths; its
physical Home/phone workflows and cold-start recovery remain open. See
[Phone manager startup](PHONE_MANAGER_BOOT.md) for read-only validation,
ownership, configuration and compatibility limits.
An additional `--phone-page-profile` selects the optional resident native page
and Home button on that manager's main output. It requires the manager profile
and rejects camera coexistence until their focus/ownership path is qualified.
See [Native Home](NATIVE_HOME.md).

The fallback unit now takes its canvas, Wayland socket and named physical output
from the validated receiver display configuration. It no longer hardcodes
1920 × 720, HDMI-A-2 or simulation. `simulate_vehicle=true` explicitly selects
bench data; without a source the fallback displays unavailable readings. A
configured physical source requires the optional timestamp-preserving fan-out
service so backup, Standard and Ultra do not compete to bind one socket. See
[vehicle distribution](VEHICLE_DISTRIBUTION.md) for the configuration and an
optional extra service. The selected V3 release does not deploy this observation
broker or a physical car provider; its bench cluster uses simulated values.

The launcher also ensures Bluetooth is powered and accepts incoming connections;
power alone does not establish connectability. It first accepts an already-ready
adapter without changing its state, otherwise uses BlueZ or the controller
management tool with bounded timeouts. This does not erase bonds or permanently
enable pairing/discovery. On 30 September the powered controller was found
non-connectable; restoring that state and resetting the radio was followed by
a successful phone connection. Distance may also have contributed to the user's
failed attempts. The bench's `btmgmt --index 0` command could hang even when
already enabled, so readiness now avoids that path. The actual CM5 readiness
check and subsequent receiver startup pass; a cold-boot reconnect still needs
qualification.

To inspect the current five-service generation, including Home:

```sh
systemctl status headunit-compositor headunit-outputs headunit-cluster headunit-receiver headunit-phone-page
journalctl -u headunit-receiver -b --no-pager
```

Follow the retained-release procedure before restarting a service or changing
its configuration; check [handoff](HANDOFF.md) for the selected generation.

The boot configuration on the current bench also suppresses splash, cursor and
routine kernel/service messages. Its original files are saved as
`/boot/firmware/cmdline.headunit-before.txt` and
`/boot/firmware/config.headunit-before.txt`. This is visual cleanup, not a
measured fast-boot optimization. On the 29 September warm reboot of the
**earlier four-service checkout**, all four units were active, the Wi-Fi AP was
up,
and the user saw the cluster before connecting a phone. Systemd recorded the
cluster unit as active about 8.05 seconds after userspace startup; that is not
yet a display-photon measurement. A timed full power-on check remains open.
After that earlier reboot, the user reconnected the iPhone and confirmed the main
CarPlay view and Ultra cluster returned on their respective displays.

## Recoverable service-unit updates

`--install` now stages and verifies the rendered service bundle before changing
the installed units. It saves the previous local file bytes, permissions and
boot enable state in a private snapshot, then writes and enables the new bundle.
It changes only `headunit-compositor`, `headunit-outputs`, `headunit-cluster`,
`headunit-receiver` and the optional `headunit-phone-page` and
`headunit-vehicle` services. It never starts, stops or restarts a running
service.

The installer prints a snapshot path under
`/var/lib/ultra-headunit/appliance/transaction-…`. To restore those managed unit
files and their previous enable state:

```sh
sudo python3 tools/install_cm5_appliance.py \
  --rollback /var/lib/ultra-headunit/appliance/transaction-SNAPSHOT
```

Use the exact path printed by the installation. This command does not load the
receiver or display configuration, so broken or missing configuration does not
prevent restoration of the service units. If `--transaction-directory` selected
another private snapshot directory during installation, pass the same directory
when rolling back. Both the directory and its lock must belong to root; snapshots
are mode 0700 and journals are mode 0600.

A failed unit write, daemon reload or enable operation automatically restores
the previous bundle. If that restoration itself fails, the journal remains
pending and the same rollback command can be retried. After an interrupted
installation, a recovery-only invocation restores pending transactions without
loading receiver configuration or admitting a candidate:

```sh
sudo python3 tools/install_cm5_appliance.py --recover-pending
```

Pass the original `--transaction-directory` if non-default. This root-only
action uses the same locked transaction owner and retained-target verification
as rollback. It does not start, stop or restart services. It remains available
when candidate/configuration inputs are missing; a damaged retained target or
unrelated unit edit still prevents restoration. A normal installer run also
recovers pending transactions, but only after its configuration admission.
This CLI is not automatic boot recovery.

The journal is bounded, validated and synced before unit changes. Up to 64 snapshots may remain in the transaction directory; archive
older completed or rolled-back snapshots before another installation. Explicit
rollback remains available at that limit. Do not move or delete pending
transactions.

Rollback refuses to overwrite a unit whose current bytes or permissions match
neither the old nor the new snapshot. Symlinks, unsafe permissions, unmanaged
unit names and damaged journals also fail before restoration. The supported
local unit states are enabled, disabled, static and not-found. Masked, linked,
indirect and runtime-only enable states are rejected before installing. A
headunit unit loaded from outside the local unit directory is also rejected.
This policy records the unit's boot enable state; it does not preserve arbitrary
aliases or a hand-built systemd symlink topology.

The legacy checkout installation above provides **service-unit rollback**.
It does not restore built executables, Linux packages, receiver/display files,
vehicle profiles, pairings or asset packs. Those files must remain compatible
with both unit bundles. Isolated tests exercise failed writes, reloads, enables,
interrupted application, retry and refusal to overwrite unrelated edits. A real
checkout-unit installation/rollback, power-cut recovery and complete
software-image rollback remain separate qualification work. These isolated
checkout tests are not the retained V3 deployment evidence.

The [application release workflow](APPLICATION_RELEASES.md) additionally
keeps first-party binaries, launcher dependencies and typed static configuration
in retained versioned directories. Exact service/manifest bindings let a
versioned rollback select both old software and old settings. Root-owned settings
and startup guards reject damaged or mixed bundles; secrets, live state, assets
and OS packages remain external. The V3 release is selected and all five service
paths passed a read-only check on the CM5. Physical rollback, automatic boot-time
journal recovery and full image rollback remain unfinished. New source implements automatic recovery through a retained helper/generator;
CM5 qualification still requires a newly published graph with activation,
ordering and failure dependencies before owners start, plus explicit resolution
of restored/removed optional service jobs after reload. Enabling restored units
does not start them. Existing sealed startup guards cannot be edited in place.
The recovery-only CLI passes portable interrupted-write/enable, retry, no-op,
lock-conflict, bounded/damaged-journal, missing-candidate and retained-target
checks; those fixtures do not qualify real systemd boot transactions.

## Retained boot recovery and isolated qualification

New source implements a production retained recovery helper plus atomically
activated systemd generator. The generator writes a fail-closed gate and fixed
Requires/After dependencies for all seven managed owners before reading any
helper code. Its bounded pinned inventory check rejects missing/damaged files
and unlisted Python modules. The gate alone holds the existing transaction lock,
recovers journals, verifies the restored legacy/current bundle, cancels only
freshly queried unwanted optional jobs and requests restored owners nonblocking.
No helper upgrade, second journal or competing runtime supervisor is introduced.
Version 4 application manifests bind the independent retained helper. See
[release contract](APPLICATION_RELEASES.md#retained-production-boot-recovery).

`tests/appliance_boot_systemd_fixture.py` executes that actual generator/gate
with retained helper inputs and inert legacy-like units inside disposable
rootless Podman PID1 containers. It runs no radio or display. Eleven actual cases
pass: optional removal/addition, interrupted write/rollback, complete/no-op,
damaged journal, lock conflict, missing/damaged helper and warm selection. The damaged-helper
case empties the verifier; no helper executes and zero owners are admitted.
Copied source directories/files have explicit trusted non-writable permissions,
including when the outer caller has umask 0002; production validators stay strict.

On 6 October 2026, portable bootstrap 13, contract 12, transaction 25 and release
46 checks pass. Actual execution uses an isolated Debian 13 ARM64 QEMU guest,
kernel `6.12.111+deb13-cloud-arm64`, systemd `257.13-1~deb13u1`, Podman `5.4.2`,
ordinary UID 1000 and systemd cgroups. Immutable locally prepared image:
`sha256:1c7b094bd51e0758265a0424030773486d3e98e6c7dcfffc67c6017fa72674e2`.
Its official Debian base and first-party fixture are pinned in private evidence.
Exact tested source hashes/results and case retirement are recorded at
`/private/tmp/headunit-production-boot-warm-20261006/manifest.json`.

The fixture remains opt-in, requires an already loaded immutable image ID and
rejects missing infrastructure. CTest uses `HEADUNIT_TEST_APPLIANCE_BOOT_SYSTEMD`
and `HEADUNIT_TEST_APPLIANCE_BOOT_IMAGE`; `appliance-boot-recovery` and
`appliance-boot-contract` remain portable. This proves isolated production-path
cold boot ordering/failure gating, not CM5 activation, visibility, power-cut or
physical rollback. Review found the warm-activation boundary is protected before selection: first activation
requires inactive owners and no pending jobs; an already running appliance
requires the exact successful active gate. The actual warm PID1 regression
qualifies individual restarts and legacy rollback without rerunning recovery,
and refuses inactive-gate/live-owner install, rollback and recovery writes.

## Failure boundaries

- The native cluster has its own process and does not depend on iPhone or MFi
  authentication. Loss of the receiver does not stop it.
- Ultra's cluster scene worker has an opt-in eight-second post-startup
  presentation stall limit. A terminal fault closes the CarPlay session and
  its window so the native cluster can show. The first qualified scene is
  allowed up to sixteen seconds after the phone clock becomes ready.
- Physical vehicle freshness and warning-light state remain separate from
  phone-session state. Only active warnings render; missing car data is marked
  unavailable.
- Current two-screen routing assumes both outputs stay powered. Monitor
  hot-plug and power-cycle recovery are outside this bench configuration.
- This is a development appliance. Safety monitoring of rendered instruments,
  certified vehicle inputs, audio, an on-device pairing page and complete
  software-image rollback are not complete.
