<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Local startup and goodbye sequence

Source-only on `development branch`. The phone-session agent owns
CM5 qualification and publication. No services, manager state, signed bundles,
pairing records or sealed generations were changed by this work.

The original Linux Head Unit emblem and wordmark provide a transparent local sequence:
1.8 seconds of outline/fill and a dissolve into already-running instruments;
a 0.9-second goodbye dissolves the same mark. `minimal` uses a restrained cyan
line instead; `off` submits no decoration. There is no opaque cluster cover,
gauge sweep through invented readings, progress percentage, readiness signal,
power command, audio, second window or renderer owner.

## Asset choice and ownership

The externally supplied
signed pack's genuine cluster logos and completion remain authoritative for an
authenticated Ultra connection, including its manufacturer-only Linux Head Unit option.
Its main welcome is phone video; there is no reusable signed main Hello asset.
See [signed greeting](ULTRA_GREETING.md#main-screen-hello-ownership),
[asset trust](ULTRA_ASSETS.md) and [asset discovery](ASSET_DISCOVERY.md).
The new local sequence uses only our original polygons from
`assets/branding/headunit/logo.svg`, shared with the existing greeting and offline
`tools/render_startup_animation.py`. No Apple/Aston artwork or signed resource
was copied, edited, re-signed or replaced in Git.

Phone-free signed gauges retain their **resume** launch, empty remote-video slot,
local monotonic animation and dated vehicle inputs. Decoration is a sibling on
that same GPU owner. Native gauges draw the matching vectors on their existing
Cairo owner; instruments and lamps draw above them. The cluster mark occupies
only x=874..1046, y=170..292 on the 1920x720 canvas, clear of top arrows and the
bottom warning row. Centre content remains live beneath it. Connected Ultra
accepts explicit events through its existing cluster decorator, preserving
signed manufacturer/Apple branding and warning-overlay ordering. Standard
CarPlay accepts those events on its existing native cluster painter thread.
The optional HUD is unchanged; useful HUD information remains available.

## Configuration and native UI's settings interface

Opt in through a new **unsealed input recipe**, then publish a new generation:

```json
{"greeting_sequence_file":"/home/headunit/.config/ultra-headunit/native-home/state/greeting-sequence"}
```

For an Ultra-capable profile, also set the identical absolute external path:

```ini
greetingSequenceFile=/home/headunit/.config/ultra-headunit/native-home/state/greeting-sequence
```

The receiver JSON path must equal the phone manager's
`stateDirectory/greeting-sequence`. The release checks require the receiver,
installed-UI profile and cluster unit to agree, including direct releases.
The managed cluster receives `--greeting-sequence-file PATH`; the launcher
scrubs inherited `HEADUNIT_GREETING_SEQUENCE_FILE` and forwards only its
explicit configured path to Standard's native painter. Omit the setting to
retain the previous appliance boot behavior.

The preference is an atomic, display-account-owned, single-link **0600** ASCII
file containing exactly `off`, `brand` or `minimal`, optionally followed by one
newline, in the owned **0700** manager state directory. Missing, unsafe or
malformed preferences select Off and retire active decoration. A saved Off
cancels within the next 50 ms owner poll, subject to actual rendering progress.
Other variant changes apply to the next event. The manager syncs file and parent
directory for preference durability; only a genuine confirmed file save is
reported. This preference is presentation data, never a car reading.

Manager status adds:

```json
"greeting_sequence": {
  "configured": true,
  "variant": "off",
  "variants": ["off", "brand", "minimal"],
  "mainIntegrated": false,
  "powerIntegrated": false
}
```

Requests use the existing manager socket and admission:

```json
{"action":"greeting_sequence","variant":"brand"}
{"action":"greeting_preview","phase":"startup"}
{"action":"greeting_preview","phase":"goodbye"}
```

Settings and previews do not connect/disconnect a phone, change pairing, request
Home or control power. Preview returns `confirmed:false` and
`provenance:"native-preview"`; saving a preference does not acknowledge physical
presentation. Do not offer preview controls while another modal/main owner would
make the result misleading. No native GTK page styling was changed.

## Main-screen component for native UI integration

`tools/greeting_sequence.py` is self-contained, uses only Python's standard
library, imports no GTK and owns no display, IPC endpoint, timer or thread:

```python
from greeting_sequence import GreetingSequence, IgnitionObservation
sequence = GreetingSequence()
sequence.begin('startup', 'brand')
frame = sequence.frame()             # immutable phase, variant, opacity, reveal
svg = frame.svg(1280, 720)           # transparent, original vector component
sequence.retire()                    # readiness, owner loss, output withdrawal
```

SVG allocations are bounded to 240..4096 by 240..2160. A GTK/GSK integration
should cache the three polygon paths and apply the frame's opacity/reveal rather
than decode a new texture every tick. Use the existing page owner and an empty
input region; keep camera/phone foreground and touch ownership unchanged. Date
the event once with the real local monotonic clock. Stop ticks on completion,
failure or withdrawal. Construction/ticks never grant CarPlay readiness.
This module is **not yet in the sealed resident import inventory**: native UI must
integrate it into an admitted first-party module or explicitly extend the
versioned release inventory and qualify that new graph. Do not copy an extra
unlisted module into a sealed generation.

When `greeting_sequence_file` is configured, the page boot wrapper skips the old
opaque **dual-panel** Linux Head Unit splash, including when the saved variant is Off.
This operational routing change allows the native interface and cluster to
start immediately. It supplies no new GTK design. native UI must integrate the
new main component on the existing page surface before claiming a two-screen
sequence. The manager truthfully reports `mainIntegrated:false` today.

## Timing, events and retirement

The existing cluster claims a private volatile once-per-kernel-boot marker only
with an actual `CLOCK_BOOTTIME` age <=60 seconds. Late/uncertain starts or unsafe
runtime paths skip decoration. Once claimed, restart/failure cannot replay it;
local signed failure and native fallback share one sequence owner. Phone
connection, Home/Back and worker replacement do not claim boot/wake or replay
an event. The signed authenticated session greeting is a separate existing
lifecycle and is unchanged by this feature.

The external `PATH.event` mailbox contains one bounded cosmetic request:
`startup MONOTONIC_NS\n` or `goodbye MONOTONIC_NS\n`. The manager stamps its actual
request acquisition, not a file date or a phone timestamp. The domain matches
`headunit::Clock`: actual `CLOCK_MONOTONIC` on Linux and
`CLOCK_MONOTONIC_RAW` with the supported macOS libc++; no offset is fabricated. Readers enforce
private regular-file/directory metadata, stable before/after/named identity,
canonical parents, a 64-byte limit, exact syntax, acquisition at/after their own
creation, a 900 ms admission age, no future dates and no consumed-event replay.
A witnessed future request cannot become valid merely when a later redraw
reaches that date. Animation starts at **original acquisition**, so a delayed
poll cannot extend it. Clock reversal, invalid settings and owner retirement
remove it; hidden-layer redraws cannot revive an old sequence. There is no
service shutdown wait or replacement readiness/presentation clock.

`GreetingSequence.observe_ignition(IgnitionObservation(...), variant)` provides
a future **main-component adapter**, not an installed hardware provider. It
requires an explicitly available Boolean observation, original local acquisition
within 500 ms, bounded provenance and source generation. Only a fresh observed
edge from a known same-generation baseline starts startup/goodbye. Initial Off,
unknown/stale state, repeated dates and source replacement cannot invent an edge.
The immutable observation keeps acquisition/provenance; the cosmetic timeline
uses that acquisition. Requested ignition or actuator state must never feed it.
Actual MCU clock mapping, power sequencing and cutoff authority remain in the
hardware repository.
A future power owner may request a best-effort goodbye before withdrawal and
must retain its own deadline; this component cannot hold power on or confirm
ignition/cutoff. SIGTERM, phone disconnect and renderer faults retire immediately
and are **not interpreted as ignition-off**.

## Evidence and limits

Portable tests cover lifecycle completion, immutable frames, variants, clock
reversal, unavailable/fresh observed ignition edges and source replacement;
private preferences/requests, exact syntax, FIFO/symlink/permission rejection,
expiry, owner replacement, future-date rejection and Off cancellation;
unchanged native gauge/lamp pixels outside the centre mark, transparent scene
bounds, original shared geometry, environment scrubbing, durable manager
preferences, honest preview dates and exact release/profile/unit agreement.
Boot-wrapper tests show opt-in skips the legacy cover before starting the page.
These are synthetic/provider and portable CPU pixel checks, not physical car,
GPU, panel, phone or whole-boot acceptance.

A 1920x720 portable native CPU benchmark alternates 120 Off and 120 Linux Head Unit
frames after cold font/backdrop preparation, with unavailable vehicle readings.
The initial Mac run measured median **1.41083 ms Off / 1.53154 ms Linux Head Unit**
(**+0.120708 ms**), p95 **1.58204 / 1.6425 ms**, and a **16.8437 ms** initial native
draw. These are actual drawing costs with a simulated animation frame, not boot,
VM/GPU scheduling, phone freshness or compositor presentation measurements.
Reproduce with `build/headunit-greeting-benchmark`; retain its JSON privately.

Local matrix receipts and failures live outside Git under
`~/.cache/headunit-qualification/20261013/codex-greeting-sequence/`.
Source `012a44c` has the following component evidence (later documentation
changes no implementation/test input):

| Check | Result |
| --- | --- |
| Pinned clang-format 19.1.7 | PASS |
| Mac portable full initial feature tree | 158/158; final main 5, manager 36, CPU-budget 1 and real manager-to-C++ clock probe pass |
| Linux GCC Debug/video | 186/186 |
| Linux Clang 19 ASan+UBSan Debug/video | 186/186 |
| Full Skia GCC RelWithDebInfo | 187/187 nonhardware |
| Full Skia Clang 19 ASan RelWithDebInfo | 187/187 nonhardware |

All 727 non-Markdown inputs match the final Linux frozen source. Mac's complete
suite precedes the later clock/fixture-only corrections; the affected final
checks above cover those changes. The default Linux jobs use actual Linux
builder-owned source and executable 512 MiB tmpfs fixtures, with only their
build directories stored on the Mac and unprivileged builder matching host UID.
This storage variation preserves the original compiler/CTest/warning/leak
policies and timeouts. Both full receiver jobs use the standard local-CI runner
once VM storage stabilizes. Linux leak checking stays enabled with only the
existing fontconfig/Cairo suppressions; no warning exception, assertion waiver
or hosted CI resumption. C++ production inputs remain unchanged after the
initial feature implementation; upstream `b32c966` corrects only a budget fixture
to measure the existing owner-thread CPU limit. No production VM limit changes.

Initial release-suite timeout, non-Linux host socket metadata, implicit noexec
temporary mount, incomplete builds and runner-setup errors remain retained.
Corrected GCC release checks pass under the unchanged 30-second CTest limit.
The final matrix result is also recorded in the ready-for-bench coordination note.
An initial fixture used macOS's redirected `/tmp`; canonical-path admission
correctly rejected it. The fixture now uses its actual canonical directory.
That failed focused run and interrupted preliminary matrix are retained.

## Exact bench adoption and on/off measurement (phone-session owner only)

1. Merge this branch with the other source workers and rerun the integrated
   local matrix. Retain frozen source and ARM64 binaries. Put both identical
   endpoint keys into **new private recipe inputs**, preserving current pack,
   pairings, displays, local-gauge defaults, camera, audio and vehicle profiles.
   Keep `firstPairingAnimation=false`; no signed script/resource changes.
2. The unit template/parser changed. Publish a **new independent schema-5 helper
   containing these tools**, then a new application generation bound to it;
   compare all rendered units and verify payload/helper/installed bindings.
   An old helper cannot render the new optional template/flag. Check capacity
   before staging. Fix forward; no older-generation restore/rehearsal.
   `tools/greeting_sequence.py` is delivered for native UI integration and is not installed by
   this generation's existing inventory. Main integration is a subsequent
   explicitly admitted graph change.
   Concrete publication/selection commands, with the bench owner supplying the
   absolute private recipe/source paths and the **newly published** binding:

   ```sh
   sudo python3 -B -E -s "$greeting_repo/tools/install_appliance_release.py" \
     --recipe "$greeting_helper_recipe" \
     --release-directory /opt/ultra-headunit/boot-helpers --stage-only
   # Put that actual helper path/digest in the new application recipe.
   sudo python3 -B -E -s "$greeting_repo/tools/install_appliance_release.py" \
     --recipe "$greeting_app_recipe" --stage-only
   # Retain the returned new release path/digest as greeting_release/digest.
   sudo systemctl stop headunit-receiver headunit-cluster headunit-phone-page \
     headunit-outputs headunit-compositor headunit-boot-recovery
   sudo systemctl show headunit-receiver headunit-cluster headunit-phone-page \
     headunit-outputs headunit-compositor headunit-boot-recovery \
     -p Id -p MainPID -p ActiveState
   # Require no jobs/cgroup descendants; then select only the new generation.
   sudo python3 -B -E -s "$greeting_repo/tools/install_appliance_release.py" \
     --existing-release "$greeting_release" --manifest-sha256 "$greeting_digest" \
     --install --migrate-boot-helper
   sudo python3 -B -E -s "$greeting_release/tools/appliance_release.py" \
     --verify "$greeting_release" --manifest-sha256 "$greeting_digest" \
     --unit-directory /etc/systemd/system
   sudo systemctl start headunit-boot-recovery
   sudo systemctl start headunit-compositor
   sudo systemctl start headunit-outputs
   sudo systemctl start headunit-phone-page
   sudo systemctl start headunit-cluster
   sudo systemctl start headunit-receiver
   ```

   These are instructions for the exclusive bench owner, not actions performed
   by this worker. Use its actual validated retention/capacity and whole-cgroup
   admission procedure; stop on a failed guard and fix the next generation.

3. After new-owner admission, use the **existing manager socket**, as the display
   account, to save `brand` and request `startup`, then `goodbye`. Observe the
   local/native cluster or connected Ultra cluster on the existing owner for at
   most five seconds each. Confirm mark/fade, continuously current gauges,
   unchanged missing-reading indicators, top arrows/bottom warning lights,
   main picture/touch/Climate, both Ready panels and active audio/control.
   Repeat with `minimal`; set Off during an active preview and observe clearing.
   A request reply is not a presentation ACK. Standard is a separate check only
   when the phone actually negotiates Standard.
4. To send a request without a new UI, use this bounded existing-socket client
   **on the CM5 as headunit** (no alternate owner or power action):

   ```sh
   python3 -B - <<'PY'
   import json, socket
   path = '/home/headunit/.config/ultra-headunit/native-home/state/manager.sock'
   for request in ({'action':'greeting_sequence','variant':'brand'},
                   {'action':'greeting_preview','phase':'goodbye'}):
       with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
           client.settimeout(3)
           client.connect(path)
           client.sendall((json.dumps(request) + '\n').encode())
           data = b''
           while b'\n' not in data and len(data) <= 262144:
               part = client.recv(4096)
               if not part:
                   break
               data += part
           if b'\n' not in data or len(data) > 262144:
               raise RuntimeError('Missing or oversized manager response')
           reply = json.loads(data.split(b'\n', 1)[0])
           if reply.get('ok') is not True:
               raise RuntimeError(reply.get('error', 'Manager request rejected'))
           print(json.dumps(reply))
   PY
   ```

   Retain sanitized results privately; change only `variant`/`phase` for cases.
   Do not replay private phone logs or use future-dated fixture events live.
5. **Measure six controlled warm boots on the same new generation**, alternating
   Off/Linux Head Unit three times, <=90 seconds per boot. Save the variant through the
   manager **before** each boot, retain confirmed status and boot ID, and quiesce
   builds/other clients. Keep phone state, pack/layout, audio, display geometry
   and diagnostics identical. Restart only under the bench owner's authority.
   Preserve each first-session failure; do not substitute a successful retry.
6. For each run retain `journalctl -b -o json` for the five owners and the gate
   directly to the private Mac cache, plus original launcher/receiver diagnostics,
   launch-bound readiness and local compositor receipt logs. The local log
   `Local start-to-first-presentation us=...` dates an actual receipt relative
   to renderer entry. Kernel-relative journal receipt time bounds first useful
   gauges; the receiver's original presentation-ready date and matching fresh
   publisher establish CarPlay readiness. Animation end, systemd-active,
   SVG construction and redrawing are not those measurements. Report all six
   samples, failures, medians/ranges and on-minus-off differences separately
   for kernel-to-gauges, renderer-to-first-receipt and kernel-to-CarPlay.
7. Record both physical panels optically throughout those boots, retaining an
   actual common timing reference/uncertainty. First visible gauges must not wait
   for the 1.8-second local animation; first-frame decoration is transparent.
   Phone takeover/early readiness must keep the original foreground in control.
   Check phone loss, new owner, Home/Back, malformed private fixture and retirement
   separately without altering sealed inputs or genuine confirmed preferences.
   Main-screen animation, true cold power/door wake and actual ignition/cutoff
   remain open until their owners/equipment are integrated. **No full boot-time
   or physical goodbye claim is made by this source-only branch.**
