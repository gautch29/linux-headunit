<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Control head-unit audio

Set local playback volume and mute independently of the phone's Bluetooth
volume. The native Music page sends a complete requested level to the active
CarPlay media worker and displays the level that worker has actually applied.

This interface controls our PCM mixer. It does not change a physical
amplifier, implement a new Apple command, or prove that a speaker played sound.

## Configure playback

Select an output in the receiver JSON, then optionally set its initial level:

```json
{
  "audio_sink": "alsasink",
  "audio_device": "plughw:CARD=Headunit,DEV=0",
  "audio_volume_percent": 60,
  "audio_muted": false
}
```

Use the device actually connected to the amplifier. `fakesink` is useful for
silent bench checks. The default remains `audio_sink: "none"`; controls do not
enable an audio advertisement by themselves. Volume is an integer from 0 to
100, with 100 meaning unity gain and 50 meaning half sample amplitude. It is
not a calibrated sound-pressure or logarithmic loudness scale. Mute retains
the selected volume so unmuting can restore it.

The phone manager supplies private request/state paths automatically when an
audio sink is configured. It persists the desired level in
`audio-preferences.json` independently of the observed mixer level. A later
managed receiver starts with that preference. Do not interpret a saved
preference as evidence that a currently disconnected route applied it.

For a receiver launched without the manager, add both `audio_control` and
`audio_state` as distinct file paths. Their final parent directories must be
owned by the receiver account with mode 0700. Existing files must be owned
single-link regular files with mode 0600. The launcher preserves the final
filename; a linked sidecar is rejected by the C++ file boundary. The paths may
be relative to the saved receiver configuration. Omitting them leaves the
initial fixed gain available without external controls.

The equivalent launcher flags are `--audio-control`, `--audio-state`,
`--audio-volume-percent`, and `--audio-muted` / `--no-audio-muted`.
The launcher clears inherited audio environment flags before applying the
explicit configuration, including zero volume and false mute.

## Use the native page

Open **Music → Head-unit volume**. Its increase, decrease and mute controls
are separate from the existing **Bluetooth volume** controls.

- The displayed percentage is the confirmed mixer level.
- “Applying…” means a request is pending; it does not change the confirmed value.
- “Unavailable” means there is no fresh, uniquely active audio worker.
- Bluetooth controls still use the selected phone's exposed BlueZ transport.

A receiver process running, Bluetooth connected, or a CarPlay picture visible
does not by itself make head-unit audio controls available. The phone must
actually open the implemented audio stream, and its output/mixer must initialize.

## Follow a request

```mermaid
flowchart LR
  Page[Native Music page] --> Manager[Selected phone and session checks]
  Manager --> Request[Private request file]
  Request --> Bridge[Receiver control-thread bridge]
  Bridge --> Mailbox[Latest requested level]
  Mailbox --> Worker[Audio worker applies mixer gain]
  Worker --> Mailbox
  Mailbox --> State[Private confirmed-state file]
  State --> Manager
  Manager --> Page
```

`audio_control::Mailbox` owns a fixed, mutex-protected requested/applied
snapshot and at most one leased command. Only the thread that opened the
mailbox may apply or close it. Requests contain the full gain and mute state;
newer pending requests supersede older pending ones. An already leased command
may complete first, and its confirmation never claims that a newer request
was applied.

`CarplayMediaStream` creates a new mailbox for each audio stream. It opens that
mailbox only after configuring the initial mixer and output. On teardown or
fault, the audio owner withdraws availability. The control-thread
`audio_control::Bridge` assigns each selected mailbox a random 128-bit route
identity and publishes state approximately every 100 ms. File parsing and
publication stay off the audio/display workers. Invalid files and publication
errors do not close healthy CarPlay video or audio.

The manager checks its selected phone, live owned child, session generation,
fresh state timestamp and exact current route before accepting a request.
Route changes retire pending UI requests. More than one available media route
withdraws controls rather than guessing which mixer to change. This first
interface does not coordinate gain across multiple simultaneous audio buses.

### Request format

```json
{
  "version": 1,
  "route": "0123456789abcdef0123456789abcdef",
  "sequence": "1",
  "volumePercent": 60,
  "muted": false
}
```

The route must match the receiver's current 32-character lowercase hexadecimal
identity. Sequence is a **string**, using canonical positive decimal uint64
syntax. It must increase within that route; replayed or reordered commands
cannot reapply old levels. Reusing the current sequence with different values
reports an error. A new route starts a new sequence history. Do not reuse the
example identity for real controls.

### State format

```json
{
  "version": 1,
  "updatedMonotonicNs": "123456789000",
  "available": true,
  "route": "0123456789abcdef0123456789abcdef",
  "acceptedSequence": "2",
  "appliedSequence": "1",
  "requestedVolumePercent": 35,
  "requestedMuted": true,
  "volumePercent": 60,
  "muted": false,
  "error": ""
}
```

This example shows a pending request. The mixer still confirms 60%, unmuted.
Applied sequence cannot exceed accepted sequence. Equal sequences require
identical requested/applied values. Initial configuration uses sequence `"0"`.
New source optionally adds an `output` object to this version-1 state:

```json
{
  "sampledMonotonicNs": "123456788000",
  "acceptedBlocks": "100",
  "discontinuities": "1",
  "underruns": "0",
  "starved": false,
  "timing": {
    "scheduledAheadNs": "70000000",
    "maximumOwnerGapNs": "50000000",
    "lastUnderrunOwnerGapNs": "0",
    "lastUnderrunLateNs": "0",
    "queuedBytes": "3840",
    "lastUnderrunQueuedBytes": "0"
  }
}
```

Only the output owner publishes these fixed counters after actual output polling
and submission. Direct media output and the shared playback worker use the same
mailbox seam; shared counters describe the single mixed sink, not individual
music/voice streams. Accepted blocks are ten-millisecond PCM submissions, not
speaker receipts. Muted or resource-denied decoded blocks may still be submitted.
No PCM, track metadata or phone identity is included. Owner dates and all counters
must never regress; an equal date cannot describe changed state. Retirement
clears live telemetry and a new output starts its own counter lifetime.

The file bridge preserves the original worker date and dates file publication
after copying state, allowing concurrent worker updates without inventing dates.
The manager adds `output.fresh`, using a separate one-second worker-age limit;
a current file heartbeat cannot refresh stale output. Missing telemetry remains
absent, rather than a zero reading. Stale worker telemetry does not change the
confirmed mixer level. Consumers must compare counters only within the same
current route and sample intervals; a progressing counter alone does not prove
gapless playback or identify why a local schedule starved.

V72 carries the original progress counters; optional `timing` is source-only
on 13 October pending integration and a new retained generation. It consists of
exactly six canonical uint64 decimal strings. `scheduledAheadNs` is the end of
the accepted PCM schedule relative to the last output-owner clock observation
(zero when absent/exhausted), not appsrc occupancy. `maximumOwnerGapNs` is the
largest interval between those observations in the selected output pipeline's
running clock after scheduling begins, including idle pauses.
`lastUnderrunOwnerGapNs` and `lastUnderrunLateNs` retain the interval
since the preceding observation and time past the scheduled end at the most
recent exhaustion; zero means none observed in this generation. `queuedBytes`
reads actual appsrc occupancy after the latest owner operation;
`lastUnderrunQueuedBytes` retains that occupancy at the exhaustion observation.
These bytes exclude downstream in-flight/device buffers and upstream decoded
channel queues. A nonempty queue at exhaustion provides evidence of buffered
PCM on a late streaming path; an empty appsrc alone does not prove source loss.
Repeated polls
do not rewrite that episode. Remaining lead is an instantaneous measurement,
while maximum gap cannot regress or disappear once supplied. The existing
owner date covers timing too; file publication cannot refresh it. Legacy states
without timing remain accepted. These fields distinguish scheduling evidence
from source absence but do not attribute CPU/GPU/lock contention or measure
a hardware xrun. See [camera/audio follow-up](AUDIO_OUTPUT.md#cameraaudio-scheduling-repair--13-october-source-only).

Unavailable state has an empty route.

The timestamp is a real receiver-local `CLOCK_MONOTONIC` nanosecond value. The manager
accepts a state only within three seconds, using the same machine's monotonic
clock (`time.clock_gettime_ns(time.CLOCK_MONOTONIC)`); it is not a phone or media
presentation timestamp. Do not substitute either a C++ `steady_clock` origin
or Python's default `monotonic_ns()` on macOS: those can use different clocks.
Sequence and timestamp
strings preserve the entire uint64 range across Python and C++ JSON readers.
Unknown/duplicate fields, wrong scalar types, out-of-range values and files
over 2048 bytes are rejected. State publication atomically replaces a private
file; readers never treat an incomplete update as confirmation. Diagnostic
errors are bounded to 128 printable ASCII characters.
State is an ephemeral heartbeat, so publication does not force a storage flush
on every receiver poll. Desired preferences are stored separately and durably.

## Understand when sound changes

“Applied” confirms the mixer configuration, not the speaker's playback time.
PCM already accepted by the output can drain with its previous gain. Our output
schedule is bounded to less than 80 ms ahead; a device's own buffer and the
upstream RTP jitter buffer are separate. Mute is not an emergency hardware cut.

Under backpressure, the worker retains one **unmixed** source block. A new
volume/mute setting affects that block's next attempt. Failed submissions do
not advance either guidance or phone-requested music ducking envelopes.
The phone envelope is separate from user volume/mute, and its command reply
confirms acceptance rather than playback. Successfully accepted PCM is not
modified afterward. See [audio output](AUDIO_OUTPUT.md) for scheduling and
remaining clock limitations.

The owner also checks output failures on idle turns. A failed device, lost
output clock or unexpected stream end retires availability even while music is
paused. Once retired, the mailbox rejects further commands and retains its
requested/applied history; a successful earlier mixer acknowledgment does not
prove that the later failed device played sound.

Explicit offline output drains also retain their errors: a failed drain cannot
turn a later submission into an ordinary full-queue retry. A successful drain
still rejects new PCM normally. This does not add a live route-recovery policy;
the media worker retires a failed route as before.

## Verification

Portable fixtures exercise mailbox ownership/concurrency, route changes, replay
rejection, unsafe files and separate requested/applied state. A cross-language
check writes a Python request at uint64's maximum sequence, processes it through
the C++ private file bridge and actual mailbox, then validates pending and
confirmed state with Python. Native GTK fixtures exercise the control workflow;
their radio and phone boundaries are explicit substitutes.

The production GStreamer decoder/output and authenticated synthetic AAC
fixtures confirm initial, full and half gain, mute, zero gain and restored gain
in actual sink-pad PCM. Actual idle bus faults retire controls without new audio;
destroyed workers reject new commands. CM5 Linux and Mac sanitizer results,
including the explicit-drain regression and their limits, are recorded in
[audio output](AUDIO_OUTPUT.md#verification-and-remaining-work).

```sh
ctest --test-dir build --output-on-failure \
  -R '^(audio-(control|control-file|control-bridge|control-python|control-interop|mixer)|phone-management|receiver-configuration)$'
ctest --test-dir build --output-on-failure \
  -R '^(audio-output|audio-rtp-decoder|carplay-media-stream|carplay-audio-control)$'
```

The current retained CM5 release has no configured audio sink, so these controls
are not proof of an active speaker route. The test processes did not replace
that running release; see [the handoff](HANDOFF.md). Audible output, sustained
real-phone gain/mute, artwork delivery, navigation focus/mixing, amplifier
control, microphone/Siri and calls remain open acceptance work.
