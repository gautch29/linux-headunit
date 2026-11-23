<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Add a phone

Pair a new phone from the native **Phones** page. The head unit opens a temporary
incoming Bluetooth window, displays the comparison code and waits for explicit
confirmation. A Bluetooth bond does not select a CarPlay mode or prove that
CarPlay is connected. This optional feature defaults off. V37 paired a new iPhone on the CM5 through
explicit comparison-code consent; repeated pairing/failure qualification remains
separate from that scoped success.

## Configure pairing

Add this optional object to the [phone manager profile](PHONE_MANAGER_BOOT.md):

```json
{
  "pairing": {
    "enabled": true,
    "adapter": "hci0"
  }
}
```

The existing profile fields remain required. Missing `pairing` or
`enabled: false` provides no agent or pairing action. Only explicit **Add phone**
starts a window; configuration validation, construction and status polling do
not. The current receiver uses `hci0`, so enabled pairing must use that controller.
The standalone component accepts other bounded controller names for isolated
checks; this does not establish multi-controller receiver support.

The page shows the controller's existing alias. The flow does not change the
configured Bluetooth/CarPlay name.

The next-source page directs an empty phone list to **Add phone** when a valid
manager snapshot reports pairing support. Older or unsupported managers retain
the Bluetooth-settings instructions. Changing that capability updates the
instructions even when the paired-phone list stays empty; rendering the message
does not start pairing. This refinement is separate from sealed V10 (`eabca30`).

## Pair on the head unit

1. Disconnect the managed CarPlay session, then choose **Phones → Add phone**.
2. On the iPhone, open **Settings → Bluetooth** and select the displayed name.
3. Compare the six-digit codes. Choose **Pair** only when they match, or
   **Reject** to refuse. Complete the phone's own prompt as well.
4. Wait for **Phone paired**, then choose **Connect** from the paired-phone list.
   The page reports the negotiated CarPlay mode after the phone connects.

Code acceptance remains pending until a fresh read confirms the selected
device's bond. Only this explicitly accepted candidate is then marked trusted
and checked again, allowing future Bluetooth service connections. The existing
iAP profile requires service authorization, so the persisted permission is
necessary after the temporary agent closes. A failed trust write is reported
with the observed bond; it is not complete success. This step creates no
CarPlay protocol keys, starts no receiver and sends no media command.

The explicit Add action may power the controller on and verify that state.
It leaves power on afterward so the newly paired phone can keep connecting.
Power failures are reported, not treated as success.

The window lasts at most 120 seconds from its original monotonic start time.
Prompts, replies and polls cannot extend it. Cancel, rejection, expiry or loss
of BlueZ clears pending consent and closes the window. A running owned receiver
prevents Add; an active pairing window prevents receiver/mode/removal changes.
These operations do not implicitly interrupt working CarPlay. The native page
disables saved-phone Connect/Disconnect, Standard/Ultra and Remove controls
during `starting`, `discoverable`, `confirmation` and `stopping`, using the
manager snapshot. Callback admission checks the same state so a stale click
cannot send a forbidden request or open Remove. Fresh idle/paired/error status
restores the controls; pairing confirmation/cancel remain usable.

## Radio and agent ownership

`tools/phone_pairing.py` owns a private D-Bus connection and GLib context on an
independent worker. Slow manager callbacks do not renew the pairing deadline.
D-Bus connection setup is asynchronous on that worker, so it keeps enforcing
the deadline while authentication waits. Normal requests use the remaining
original deadline; cancellation interrupts Gio. Final manager shutdown first
withdraws consent, then waits for the
pairing worker to retire before releasing its ownership lock. Status and UI
callbacks do not wait for that cleanup.

The backend registers a `DisplayYesNo` agent and requests default-agent routing
for the explicit session. Calls must come from the current unique owner of
`org.bluez`. Device identity is bound to the configured adapter, address and one
candidate. A second device cannot replace a pending confirmation. Consent
requires the exact session and random prompt identifier; stale clicks cannot
confirm a new attempt.

Already-discoverable adapters are refused. A finite discovery timeout is set
before opening the owned window. Cleanup restores only properties that still
match the session's owned values and closes the agent connection. It does not
overwrite observed external setting changes. Restoration is read back; a
failure stays visible even when the phone's bond has completed.

BlueZ normally starts pairable. The flow leaves that existing `Pairable` state
and `PairableTimeout` untouched. For a previously non-pairable adapter, it sets
a finite timeout before enabling pairing and conditionally restores its own
settings. These writes precede agent registration: with `AlwaysPairable=false`,
BlueZ can enable Pairable as a side effect of installing the first default agent.
The old order raced that side effect against a stale false baseline and cut
Add phone off in roughly one second. Repair `54f3eb0` admits the finite owned
window first, while undiscoverable; the same CM5 now opens and cancels cleanly.
All 21 isolated Gio/BlueZ protocol cases pass, including this daemon behavior
and restoration. See the official [BlueZ adapter implementation](https://github.com/bluez/bluez/blob/5.79/src/adapter.c)
and [default-agent implementation](https://github.com/bluez/bluez/blob/5.79/src/agent.c). A finite assisted window is not control over every pre-existing
system pairing policy.

The protocol follows the official [BlueZ Agent API](https://bluez.readthedocs.io/en/latest/agent-api/)
and [Adapter API](https://bluez.readthedocs.io/en/latest/adapter-api/).
Legacy PIN/passkey entry is unsupported by this comparison-code interface.
BlueZ's [profile implementation](https://raw.githubusercontent.com/bluez/bluez/master/src/profile.c)
defaults registered profiles to service authorization. The receiver retains
its existing authentication and selected-phone checks.

## Manager API

Commands use the existing private socket and peer-account checks. Only their
complete fields are accepted:

| Action | Fields | Result |
| --- | --- | --- |
| `pairing_start` | `action` | Start a temporary session; no bond or CarPlay success is implied. |
| `pairing_cancel` | `action`, `session` | Withdraw the current session. |
| `pairing_confirm` | `action`, `session`, `prompt`, Boolean `accepted` | Respond to exactly the current prompt. |

Configured `status.pairing` contains `supported`, `phase`, `session`,
`expiresInSeconds`, `adapterName`, `prompt`, `pairedAddress` and `error`.
Phases are `idle`, `starting`, `discoverable`, `confirmation`, `stopping`,
`paired` and `error`. A prompt contains `id`, `deviceName`, `address` and a
zero-padded `passkey`; an authorization prompt may have an empty passkey.
Terminal state follows cleanup attempts, with cleanup errors still visible.

Older manager replies without support hide Add. Lost or invalid status disables
stale consent controls. Rendering a prompt never accepts it automatically.

## Qualification

Configuration, manager admission, private D-Bus protocol and GTK are checked
separately. A private D-Bus daemon and explicit fake BlueZ service exercise
actual Gio messages without the host's Bluetooth service or a phone. They
cannot prove firmware behavior, Linux system-bus authorization or iPhone pairing.

Enable the isolated protocol test explicitly on a development machine with
Python Gio and `dbus-daemon` installed:

```sh
cmake -S . -B build -DHEADUNIT_TEST_PHONE_PAIRING=ON
ctest --test-dir build --output-on-failure -R '^phone-pairing$'
```

Missing dependencies fail this enabled test. The GTK fixture separately checks
the actual native page against a private fake manager. The Linux
`HEADUNIT_TEST_NATIVE_PHONE_PAGE` option runs both page fixtures in an isolated
display session; that fixture's display tools are not production requirements.

On 6 October, actual local GTK/private-manager fixtures passed all four active
pairing phases, stale saved-phone callbacks, held idle status, fresh recovery
and status loss. The new regression fails against the previous source when
saved-phone actions remain enabled. General page and resident ownership
fixtures also pass. Existing GDK frame-clock warnings remain; these are
simulated manager boundaries, not Bluetooth, CM5 or phone qualification.

On 7 October, the actual CM5 GTK fixture under isolated Weston 14 kiosk shell
and Mesa/V3D exposed a fullscreen failure from a 300-character unbroken backend
error. The default word wrapping advertised a minimum width of 2664 pixels;
Wayland correctly rejected 2664 × 720 geometry against the configured 1280 × 720
fullscreen state. The existing pairing note and cleanup warning now use word-or-
character wrapping. The unchanged compositor fixture passes with both long
error paths, preserving 1280 × 720 and bounded label minimum widths. Private
before/after Wayland logs retain the exact failure and recovery. This is actual
Linux GTK/GPU evidence against a fake manager, not physical pairing or scanout.

The next-source empty-state refinement passes the actual macOS GTK/private-manager
fixture through unsupported → supported → unsupported → supported changes with
an unchanged empty phone list. No pairing command occurs until **Add phone** is
explicitly activated. The focused portable pairing contract suite passes all
20 checks. Logs are retained privately under
`~/.cache/headunit-qualification/20261007/oem-pass/next-native-capabilities/`.
Existing GTK/GDK diagnostics remain; these checks do not qualify Linux Wayland,
the selected release or physical pairing.

Pending target checks: first pairing, comparison/rejection, expiry/cancel,
BlueZ restart, radio cleanup, appearance in the managed list and the subsequent
Standard/Ultra connection. Resident page startup and the physical Ultra
Home/Back path are separately qualified on the CM5; that does not prove native
pairing. Version 2 sealed application releases now include the manager and
optional Home dependency graph. Their portable checks pass; target activation
and physical pairing qualification remain open.
