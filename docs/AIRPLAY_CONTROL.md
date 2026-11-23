<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# AirPlay control endpoint

## Night-mode queue boundary — 15 October source-only

`control_probe.cpp::pump_night_mode` reads the existing receiver policy and
remembers a connection's last choice only after `send_night_mode` admits it.
Absent policy, pre-RECORD, unavailable channel and event-queue backpressure
cannot remember a refused choice. The next eligible pump evaluates the current
policy and fresh vehicle facts. The existing `night_mode_sent` field and
"night mode sent" log describe queued intent, never phone acceptance, applied
appearance or display readiness. A returned event status remains diagnostic;
it does not introduce automatic retries or an applied-appearance model.

Event EOF or authentication failure clears the outbound queue, closes its
listener and makes that control owner terminal. A successor uses its own
connection state and freshly established event keys; an old queued choice cannot
authorize it. There is no asynchronous appearance callback or appearance
generation API to carry across owners. Off/On, headlights and schedule semantics
are unchanged. The separate admitted-facts repair belongs to
[vehicle observations](VEHICLE_OBSERVATIONS.md).

The Linux `night-mode-queue` regression compiles the actual internal caller with
its existing dependencies and exercises real `AirplaySessionSetup` and encrypted
event code against synthetic loopback peers. It combines both RECORD/accept
orders, queue refusal, current-policy recovery, deduplication, a diagnostic 501,
queued-command retirement after EOF/bad tag, refused old-key reconnection and
fresh-key successor admission. No runtime repair was needed for this boundary.
The portable `night-mode` and `airplay-event-channel` fixtures retain policy and
wire coverage. This is source component evidence; phone acceptance, visible
day/night transitions, physical lamp inputs and connected recovery still need
the sole bench owner's separately qualified generation.

For the current dispatcher, outbound event commands, streams/services,
observed request names and missing semantics, see the
[9 October interface audit](IPHONE_PROTOCOL_AUDIT.md). The initial discovery
and pairing paragraphs below are historical research checkpoints; their early
"not yet observed" statements do not describe the later live receiver.

The Pi's working receiver advertises `_airplay._tcp` on port 7000. A local
`GET /info RTSP/1.0` request returned a binary property list containing
identity, two display descriptions, audio formats, input devices, and other
capabilities. We inspected the field names, types, lengths, and non-secret
numeric display values; no response body or asset data was copied into this
repository. Public AirPlay receiver behavior also documents `GET /info` as an
early plaintext request ([openairplay protocol notes](https://github.com/openairplay/airplay-spec/blob/master/src/contributing.md)).

Our independent `headunit-control-probe` handles bounded RTSP or HTTP
requests, echoes a validated numeric `CSeq`, and answers `GET /info` with a
new binary plist built using libplist. The reply carries our own stable
identity, two configurable display sizes, and no claimed audio formats. Its
default mode returns `501` for pairing and all other unsupported requests
while privately capturing the first 64 KiB per connection. The identity
generator keeps a separate stable UUID for each display.

Local and integrated checks on the Pi verified that a client can decode the
857-byte plist, see both display descriptions, and receive a `501` for
`POST /pair-setup`. The integrated check ran our AP, Avahi advertisement,
Bluetooth profile, and control listener together; LIVI was restored afterward.
No iPhone has yet requested `/info` from this receiver. The capability reply
is diagnostic: its discovery feature mask reflects the observed CarPlay
service, while media, live pairing, and Ultra assets are still unverified or
unimplemented. Its
success in a local client does not establish iPhone acceptance.

The control listener decodes bounded HomeKit TLV8 messages on the pairing
paths. It logs only the pairing mode, state, method, flags, and cryptographic
field lengths; it does not print their contents. Long values are reassembled,
and malformed or oversized values are rejected. The message format and
exchange follow public [Apple HomeKit ADK pair-setup](https://github.com/apple/HomeKitADK/blob/master/HAP/HAPPairingPairSetup.c),
[pair-verify](https://github.com/apple/HomeKitADK/blob/master/HAP/HAPPairingPairVerify.c),
and [RFC 5054](https://www.rfc-editor.org/rfc/rfc5054.html).

An independent SRP-6a server component now generates a random salt and
ephemeral secret, publishes a 3072-bit challenge, verifies the controller
proof, and derives the matching server proof and session key. It uses OpenSSL
for SHA-512, cryptographic randomness, constant-time comparison, and
large-number arithmetic. A separate test calculates the controller half and
checks both proofs, rejects wrong and replayed proofs, and rejects invalid
public keys. When explicitly given a private setup-secret file, the listener
serves pair-setup M1-M4. With a matching private Ed25519 key, it also verifies
the controller's M5 signature and signs M6. An optional private pairing store
persists the controller's public key in a `0600` file under a `0700` directory.
Pair-verify then uses X25519 and the saved Ed25519 key to complete M1-M4.
After that plaintext M4 response, the connection switches to authenticated
ChaCha20-Poly1305 frames with independent counters and keys for each direction.
It can decrypt and answer an encrypted `GET /info` request.

The [AirPlay pairing notes](https://openairplay.github.io/airplay-spec/pairing/hkp.html)
describe a transient mode that stops after M4; Apple's public
[pairing flags](https://github.com/apple/HomeKitADK/blob/master/HAP/HAPPairing.h)
define its flag as `0x10`. The receiver now detects
that flag, derives the two control-frame keys from the SRP session key, and
starts encrypted control immediately without saving a phone identity. The
synthetic client exercised an encrypted `/info` request on this short path.
Split pairing, which requires retaining a verifier across sessions, remains
unsupported. The actual iPhone pairing mode remains unobserved.

Apple's public [pair-setup implementation](https://github.com/apple/HomeKitADK/blob/master/HAP/HAPPairingPairSetup.c)
also specifies method `1`, Pair Setup with Auth: M4 contains an encrypted
certificate and a coprocessor signature over an SRP-derived challenge. The
independent receiver now supports this method when explicitly started with
`--mfi-auth` and its MFi chip is available. Without that option, only
method `0` is accepted. A fake-chip test decrypts M4, verifies its TLV structure and
certificate length, and checks the exact challenge passed to the chip. This
does not establish that the iPhone chooses method `1` or accepts the response.

The saved working session shows `POST /auth-setup` after pair-verify: its
request body was 33 bytes and its reply body 711 bytes. The public
[AirPlay auth-setup notes](https://openairplay.github.io/airplay-spec/audio/rtsp_requests/post_auth_setup.html)
describe X25519 public-key exchange, an AES-CTR encrypted chip signature, and
a length-prefixed certificate. A new C++ handler implements this exchange with
our protocol-3 chip driver. Its fake-chip check recovers the 64-byte signature,
verifies the 607-byte certificate and the 711-byte reply, and checks that the
chip received the SHA-256 digest of both public keys. The diagnostic endpoint
serves `/auth-setup` only over an encrypted connection with `--mfi-auth`.
The live iPhone accepted this handler on 2026-09-25/26 and proceeded into
screen setup and encrypted video.

The Pi's synthetic Python client, independently implemented with Python's
`cryptography` library, completed all six normal setup messages, all four verify
messages, and the four-message transient setup. It checked both signatures
and the saved key, decrypted the receiver's encrypted `/info` response in
both modes, and rejected a second connection with
an invalid controller signature. Mac and Pi unit checks cover frame
fragmentation, tampering, and invalid key material. The listener is still a
bounded diagnostic receiver: it now answers the observed session and stream
`SETUP` requests, drives the UDP timing exchange, accepts `RECORD`, and
authenticates incoming screen and iAP packets. With combined mode enabled,
the parsed iAP messages feed the same engine that handled Bluetooth; replies
use the encrypted event connection. Subsequent live tests completed normal
pairing, verification, encrypted `/info`, MFi authentication, `RECORD`, and
both video stream setups with LIVI stopped. `RECORD` may precede per-stream
setup. Initial setup advertises `altScreen` and `viewAreas`, and `/info`
includes an original single-contact touchscreen descriptor; the iPhone logs
identified both as missing prerequisites in earlier attempts.

Classic iAP record admission drains complete APTransport packages after every
successfully authenticated record. A 64-KiB input read has a separate budget
from the preceding incomplete record (at most 16 KiB + 18 bytes); completed
packages do not accumulate behind subsequent records. The maximum package is
still 4 MiB. Parser faults wipe the key and pending plaintext, release buffers
and permanently retire that instance. Consumed plaintext storage is wiped after
moving any retained tail, including bytes left beyond the resized vector.

The 9 October regression reproduces the former false overflow when a read
finishes a maximum package and carries two following records, and checks a
maximum feed following a retained wire fragment. Every split point, malformed
package lengths, authentication failure after progress, input budget rejection
and terminal reuse also pass. The iAP library/test now build on both macOS and
Linux: they have no Linux API dependency. All 132 portable suites pass; scoped
iAP tests pass macOS/CM5 ASan+UBSan and strict CM5 GCC 14.2.0 without the receiver's
bench warning exception. This is synthetic component evidence, not a new
selected release or real-phone handoff qualification.

The phone sends auxiliary screen packets with type 5. Treating those as a
fatal parser error closed both video sockets. The receiver now accepts their
bounded payload without advancing the video-frame nonce. Both captured H.264
streams decode at 1280×720, and the main CarPlay home screen is visible in the
independent Pi preview. The receiver now selects the map on the cluster with
a showUI event; the user confirmed it visible on 26 September. That initial
checkpoint predates the integrated Ultra UI/vehicle consumers and optional AAC
media path. See [Ultra integration](CARPLAY_ULTRA_GUIDE.md) and
[audio output](AUDIO_OUTPUT.md) for current routes and incomplete audible/voice
qualification. `/command` uses the same bounded dispatcher on the main and
event sockets. It accepts validated `modesChanged` observations and supported
duck/unduck requests; unavailable focus and unsupported types, including
`suggestUI` and `disableBluetooth`, return 501. Malformed commands return 400.
Unknown event methods/paths also return 501. Empty authenticated OPTIONS returns
200 on both sockets; nonempty OPTIONS and RECORD bodies return 400. `/feedback`
remains an empty keepalive response without playback-clock interpretation.
These error changes require real-phone qualification before release selection.
AirPlay resource modes remain independent of [car drive modes](DRIVE_MODES.md).

### Protocol repair source — 9 October 2026

`airplay_command` bounds the binary plist graph and validates resource IDs 1–2
and app-state IDs 1–3. IDs, entities and resource fields remain unsigned.
V72 accepts the authenticated phone's integer `speechMode=-1` as an unknown
sentinel, retaining it exactly; other speech values are bounded to UINT32_MAX.
Wrong types or values below -1/above UINT32_MAX reject the entire delta. Main
and event command fixtures, including encrypted ownership/retirement checks,
pass. Real-phone V72 `modesChanged` updates now return 200. No observed sentinel
is converted into a resource grant or active speech mode. A valid delta preserves
omitted array entries; malformed or duplicate entries change nothing. Entity
numbers are retained as observations. Each resource/app entry now retains its
own receive date: an empty or unrelated delta cannot renew it. The session also
records the command's genuine receive time/revision. Terminal event retirement
clears modes, disables the surviving media worker and rejects later commands
and SETUP through retained session references; direct duck requests report
unavailable. A retired reference cannot create a new unobserved audio worker.

For the existing type-102 media consumer, resource 2 (`mainAudio`) now gates
only the media bus. Current entity 1 (controller) enables it; entity 2
(accessory), zero or an unrecognized entity disables it. `permanentEntity` is
retained for observation and never overrides current ownership. An absent entry
retains legacy media admission without claiming an observed ownership grant.
App states and screen-only/empty deltas neither grant nor revoke mainAudio.
A newly constructed or selectively reopened worker receives the retained
permission before startup; a replacement control session has fresh observations.

Permission is independent of phone ducking and user gain/mute. Unduck cannot
bypass a denial, and return preserves the latest duck target and user controls.
The worker rechecks permission on each PCM submission/retry, produces silent
media while denied, and continues transport/decode/control work. Alternate
guidance remains independent in the mixer; no alternate-audio transport is
implemented by this change. Already accepted output retains its bounded schedule
(the next block starts less than 80 ms ahead); this does not flush a physical
sink or retract an in-flight block. `resource_requested`/`resource_configured`
and `media_resource_enabled` report requested permission and worker
configuration, never a speaker receipt. A modesChanged 200 still means ingestion,
including when no healthy audio consumer is attached.

Apple's [resource-manager session](https://developer.apple.com/videos/play/wwdc2016/723/)
establishes current versus permanent/borrowed ownership and independent alternate
audio. Numeric IDs are cross-checked against the independent author's
[SDK/binary fact inventory, pinned revision](https://github.com/lvalen91/carlink_linux/blob/fbbfa59400dac4704f34a5d76e745080ce7d6338/docs/CARPLAY_CAPABILITIES.md#8-modes--resources-arbitration-sdk)
and bounded numeric-only extraction from the external retained command corpus.
Those observations do not establish current-phone transition timing. This
original implementation imports no SDK, reference implementation or signed code.
No capability or feature token was added. Screen handoff, outbound changeModes,
Siri/call/navigation routing, `/info` ownership and physical restoration remain
open; [G02 capture plan](IPHONE_PROTOCOL_GAPS.md#bounded-live-evidence-queue)
keeps those under the live owner.

The protocol-gaps pass on 9 October uses the supplied
`development branch` worktree and is not selected or installed.
Final Mac build and **141/141 portable tests** pass with AppleClang 21,
libplist 2.8.0 and explicitly selected OpenSSL 3. The affected `airplay-command`
and `audio-mixer` suites pass **2/2 AppleClang ASan+UBSan**; Mac leak detection is
disabled, so that run is not leak evidence. Pinned clang-format 19 and whitespace
checks pass. The initial Mac run passed 84/141, with 57 failures from a cached
libplist install-name mismatch; fresh configuration then exposed an unrelated
OpenSSL 4 deprecation error. Reconfiguration against the supported OpenSSL 3
and current libplist produced the passing full run. Failed/corrective receipts
remain outside Git in `~/.cache/headunit-qualification/20261009/protocol-gaps/`.

The original CM5 GCC build was **interrupted for resource coordination at
190/228 steps**, without a complete build or CTest result; that receipt remains
archived. After the manager granted an exclusive slot, all **732 tracked files**
from `72a3e5c6e8b53a5413101703fbe673e6258adca8` were refreshed and hash-verified.
`headunit-control-probe` and all seven affected test targets then built with
GCC 14.2 (`-O0 -g0`) and Clang 19.1.7 (`-O1 -g0`,
`-fsanitize=address,undefined -fno-omit-frame-pointer`). Both configurations keep
the existing warning/error checks; no assertions or suppressions were changed.

The anchored `airplay-command`, `audio-mixer`, `airplay-session-setup`,
`airplay-session-lifecycle`, `audio-focus-session`, `carplay-media-stream` and
`carplay-audio-control` selection passes **7/7 GCC (8.43 s)** and **7/7 Clang
ASan+UBSan (8.87 s)**. The Linux sanitizer run enables leak detection and
halt-on-error, with only the existing external fontconfig/Cairo suppressions in
`tools/ci/lsan.supp`. These are encrypted-session and silent-sink PCM/backpressure
component fixtures, not phone or audible output evidence. Source manifests,
compiler flags, binary hashes, build logs, CTest XML and full test logs are
archived in the qualification directory above, alongside the interrupted run.

Both builds used `umask 022`, `TMPDIR=/tmp`, `nice -n 10`, at most `-j2` and the
approved shared Luau source. The workspace was 297,308,547 bytes at completion,
with 2,878,865,408 bytes free; no Skia/GPU build was needed. After archive checksum
verification, scoped process checks found no owned processes before or after
removing only `/home/headunit/headunit-protocol-gaps-20261009`. The exclusive slot
was released at **16:16:53 UTC on 9 October**. This qualifies this branch's
scoped components. The subsequent
[integrated strict Linux matrix](CI.md#integrated-seven-worker-candidate--9-october-2026)
separately passes with the audio output/retirement changes and both test sets.
No GPU, live phone, speaker or physical resource-routing
result follows from these checks.

Outbound iAP, showUI, touch and refresh commands have a 64-slot CSeq registry
and a 64-result queue. Provisional responses keep the request pending; final
2xx and non-2xx responses record acceptance or rejection. Missing, duplicate,
late and unmatched responses are counted. A five-second observation timeout
and eviction never imply presentation or stop touch traffic. Queue-capacity
rejection creates neither a pending command nor a CSeq/nonce gap. The combined
regression checks the later admitted retry against exact reference ciphertext
and its authenticated CSeq acknowledgement. Fixed diagnostics
report counts and dropped results without arbitrary phone parameters. Event
EOF, malformed framing or write loss retires the control owner; the same keys
cannot restart event encryption counters on a replacement TCP connection.

RECORD is idempotent for an initialized timing/event owner. Stream SETUP accepts
1–12 entries within the session's twelve-allocation lifetime budget. It validates
the complete batch before allocating sockets/workers, replies in offer order,
and closes unpublished ports on allocation/encoding failure. Failed allocations
retain key/ID reservations. All audio/video/DataStream key derivation IDs share
one reservation set, so another type or a closed stream cannot reuse a cipher
key. Preflight bounds apply before libplist sees SETUP/TEARDOWN graphs.

Selective TEARDOWN validates every selected entry before closing anything. It
supports media 102, video 110/111, classic iAP 130 and registered generic 130;
timing, event and unselected transports remain owned. Reopening requires fresh
keys. The Ultra consumer now isolates video retirement and replacement per
screen while retaining its shared worker and Runtime streams; see
[native input](NATIVE_INPUT.md#per-screen-retirement-and-replacement--9-october-2026).
Real-phone video restart/keyframe ordering and end-to-end continuity remain
unqualified. Voice/navigation audio, handoff/suggestion
semantics and clock feedback remain open.

Coverage/framing follow-on source `10ebe99` adds shared strict start-line and
header parsing for main/event control. Ambiguous lengths, duplicate CSeq/HKP,
invalid numeric fields, bare line breaks, invalid header names/folding and
unsupported transfer encoding reject before dispatch. Valid pipeline framing,
maximum numeric CSeq and multiword response reasons remain supported.

Initial SETUP now keeps timing/event allocations local until reply encoding
succeeds; failure closes both and permits a clean retry. Rolled-back stream
allocations retain their crypto/ID reservations **and consume the twelve-key
lifetime budget**. Real CM5 `RLIMIT_NOFILE` exhaustion exercises failure after
the first allocation, ten failed batches, healthy video/RECORD preservation,
no descriptor leaks, fresh IDs and final budget rejection.

[Vehicle coverage](VEHICLE_STATE_GUIDE.md#connection-coverage) now records
per-channel service/type/IID outcomes through close/reopen, with known entries
pinned and 32 unknown IIDs rotating. The iAP engine separately counts all 256
session IDs, 13 known and 32 rotating unknown control IDs. Numeric notices
contain no unknown parameters, are bounded to 64, and overflow does not turn an
idle observer into a fatal application queue. Real encrypted SETUP fixtures
exercise two unknown-control floods alongside continued RECORD and reliable ACKs.
Dispatch, service confirmation, socket transfer and presentation remain distinct.

Late-diversity source `0f3a5e8` separately retains known main/event route categories
and command kinds, plus **32 rotating unknown numeric fingerprints per table**.
Both channels share each table with separate counters. Received/result/status
totals saturate and survive eviction; known categories are pinned. Unknown command
types hash only their type, and unknown routes hash the method and target with
length-separated FNV-1a. No arbitrary labels, URLs, parameters or bodies are stored.
Fingerprints are not cryptographic identities; collisions can merge counts.
Main route results follow socket write completion; event/command results record
the selected dispatch status. Neither means peer consumption or applied focus.
Invalid framing is observed without inventing a reply, and retirement emits only
bounded numeric reports. The encrypted fixture floods 49 late command types and
49 routes on **both** channels after the earlier diagnostic budget is full;
RECORD remains usable and totals/evictions/errors remain consistent. Its bounded
output reader prevents diagnostic-pipe backpressure from invalidating the test.

Restored qualification at `c81afbf` verifies **637 first-party build/test inputs**,
the complete Skia receiver build, 15 focused CM5 checks and encrypted smoke.
The release fixture's initial group-writable temporary directories rejected as
designed; its private-umask rerun passes. The two Standard cluster preview fixtures
exercise configured gauge construction and malformed-profile rejection under a
private headless Wayland compositor, without owning physical outputs. A 136-suite
portable run and three focused AppleClang sanitizer suites pass; the final gauge
simulation guard has additional focused compiler checks. Exact source, logs,
receiver binary and receipts are retained privately under
`~/.cache/headunit-qualification/20261009/iphone-gap-generic-configuration/`.
Control SHA-256: `3f24217702256dad4126d55ed6fa0ddaafa2809a38250887cb978fa24b18b112`.

Final source `0d60915` also separates audio wire/authentication/admission/overload
and unavailable drop measurements; [audio](AUDIO_OUTPUT.md#loss-and-overload-accounting)
owns that contract. All 637 inputs match; the full receiver rebuild, six affected
CM5 suites, three focused Clang 19 ASan+UBSan audio suites and encrypted smoke
pass, with a separate final configuration-compiler check. Control SHA-256:
`f02909e0ea2ff0ae5e5876092b475dbe8f8546c18ba0e2b105c5561d4787a01c`.
Private receipts/logs/source/binary are under
`~/.cache/headunit-qualification/20261009/iphone-gap-audio-accounting/`.
GNU uses the disclosed `-Wno-misleading-indentation`, `-O2 -g0 -DNDEBUG` bench
flags. This is no new full Linux-suite, phone, audible-device or deployment
qualification. V27's manifest remains
`1f79a529f13bae6ead3b8a31155801a03e921e87a2614be1fd1cb5294c8f51ba`;
the receiver and Home services are active/running with zero restarts.

All **630 build/test inputs** on the CM5 match `10ebe99`. Strict portable
checks pass **134/134**, with four focused AppleClang ASan+UBSan suites;
macOS control-request explicitly disables unavailable leak detection. Eight
focused CM5 suites and the full receiver target build pass, using the disclosed
GNU bench flags. Encrypted synthetic smoke passes all previous lifecycle checks
plus malformed framing retirement before SETUP dispatch. Its old summary-string
assertion was updated for the added notice-drop field. No full Linux suite rerun
or real-phone result is implied. Control SHA-256:
`e35d89ebdce3c17e39f579bd4e2c8e7f9a571f1c7f87aa1b4d92f65581e556d3`.
Private source hashes, logs and receipt are under
`~/.cache/headunit-qualification/20261009/iphone-gap-coverage/`.
V27 and both observed services remain active with zero restarts; no deployment,
radio/display takeover, sealed release or green CI result follows.

First-batch implementation source is `f134e94`, on `development branch`.
Strict macOS Debug checks pass **133/133 portable suites**, plus three focused
AppleClang ASan+UBSan suites for command parsing, event correlation and vehicle
writes. The isolated CM5 full Skia/native-video receiver build succeeds. Its
176-suite run initially passed 170: the focus fixture still expected malformed
modes to succeed, private test/source directories inherited group write access,
and a camera fixture checked retained state after bootstrap deliberately withdrew
it. Canonical command assertions, trusted fixture permissions and active-phase
camera assertions correct those failures; all six pass in corrective reruns.
The camera correction changes the fixture, not production camera behavior.

The CM5 synthetic encrypted smoke passes normal/transient pairing and verification,
both command sockets, unsupported/malformed responses with session survival,
OPTIONS body policy, repeated/malformed RECORD, stream/control continuity,
selective retirement, full teardown and bad-signature rejection. It uses separate
private test identities, an empty vehicle model and port 7001; it creates no radio,
physical-display or car owner. Linux lifecycle tests cover multi-stream offers,
whole-request rejection, selective audio/video/classic/generic retirement,
fresh-key reopening and event EOF. No real-phone error/reopen, audible output,
physical camera/car or applied resource-focus result follows from these checks.

At the first-batch check, all **628 first-party build/test inputs** on the CM5
matched that source. That build uses external pinned Skia `a8363ad14693`,
Luau 0.739 and the disclosed GCC
bench flags `-O2 -g0 -DNDEBUG -Wno-misleading-indentation`; it is not a default
warning-clean GNU build. The full control binary SHA-256 is
`75d13953e62dd0b57bbee8146b52ffa7303ff9a421bc8e0c6edf70bf7f43c64e`.
Private receipts/logs live under
`~/.cache/headunit-qualification/20261009/iphone-gap-implementation/`;
the isolated CM5 source/build are `/home/headunit/headunit-iphone-gaps-20261009/`.
GitHub CI remains explicitly deferred because billing prevents jobs starting;
local checks are not green CI. **V27 remains selected**, its receiver/Home services
active with zero restarts. No new release was published or selected. The iPhone
is unavailable; live qualification remains pending.


Follow-on source `49eeee4` makes video and classic iAP parser generations
terminal on error or explicit retirement. Retirement erases keys and partial
buffers while preserving parsed-message/frame counters. Each negotiated cipher
has one accepted TCP owner, including diagnostic video without a decoder. EOF
or authentication/framing failure closes its listener/client; old IDs/seeds stay
reserved, and replacement SETUP needs fresh keys. Linux socket tests cover
video EOF, classic iAP authentication failure and preservation of healthy
transport/RECORD ownership. The subsequent per-screen native-video repair is
documented in [native input](NATIVE_INPUT.md#per-screen-retirement-and-replacement--9-october-2026).

The configured provider also withdraws conflicting equal-acquisition readings
without allowing replay to revive them; [vehicle state](VEHICLE_STATE_GUIDE.md)
owns that contract. An explicit [signed-entry mode probe](DRIVE_MODES.md#qualification-boundaries)
verifies the actual identifier-string `0x14` payload, reordered mappings,
unknown/expiry/restart suppression and fresh recovery for both displays. Its
vehicle/UI peer and frame context are synthetic; no phone or presentation result
is inferred. The parser library/test is now portable as well as Linux-built.

Strict portable validation passes **134/134 suites**, with a final three-suite
parser/provider rerun and three ASan+UBSan suites after explicit key/buffer
retirement was added. The full CM5 Skia/native-video build succeeds. The broad
176-suite run passes 175; its remaining legacy SETUP fixture closed classic iAP
then reused the same old negotiated TCP/key. That fixture now retains its original
owner. All eight affected Linux suites pass on the final source, including the
corrected SETUP test, and the updated encrypted smoke passes. These results
cover all 176 suites across runs; they are not a single final-source full-suite
run or real-phone compatibility evidence.

All **630 first-party build/test inputs** match `49eeee4` on the isolated CM5.
The disclosed Skia/Luau inputs and GCC bench flags above remain unchanged.
The final control binary SHA-256 is
`b21406cc6f68dd0c8cbebb8cbb46429bfc254f1f44d0771842848beca921e8c7`.
Private source verification, build archive and logs are under
`~/.cache/headunit-qualification/20261009/iphone-gap-implementation-follow-on/`.
The CM5 disk filled while collecting receipts; after a verified local archive,
only generated artifacts in this isolated workspace were removed. Its source,
logs, control and mode-probe binaries remain. Rebuild or restore the archived
build before another test pass. V27 remains selected, receiver/Home active with
zero restarts, and the iPhone remains unavailable. About 179 MiB free remains;
plan storage recovery before another full build. No release was published.


Experimental syntax: `headunit-control-probe PORT SECONDS PRIVATE_CAPTURE_DIR
IDENTITY_FILE SETUP_SECRET_FILE PRIVATE_ED25519_PEM PAIR_STORE_DIR
[--mfi-auth [--iap2-wifi-config PRIVATE_WIFI_FILE]]`. The Bluetooth option
requires exclusive use of the Wireless iAP BlueZ profile. Omitting
the files after `IDENTITY_FILE` preserves the non-pairing diagnostic mode. The
test identity, secret, capture, and store paths are private and ignored by Git.

The user confirmed a click opening an app through our own HID report on
2026-09-26. The next gates are sustained rendering and recovery,
audio routing and complete Ultra controls/recovery. A stable control
connection and visible main screen do not prove complete CarPlay Ultra.


## Control response completion

`send_control_response` performs the current synchronous IP response write with
one absolute deadline per call, capped at two seconds. Partial sends, readiness
races and interrupted waits retain that deadline. It suppresses SIGPIPE and
rejects closed/error sockets; the connection owner closes a failed partially
written response instead of retrying its encrypted records under new counters.
Response-end diagnostics follow successful writes. Plaintext header and body
remain separate calls; this does not make the control loop asynchronous or set
a whole-session timeout. [Wireless handoff](WIRELESS_HANDOFF.md) records the
slow-reader and retirement qualification.

### Authenticated framing and event admission

The unselected `development branch` candidate separates the
64-KiB encrypted input-call budget from the one retained incomplete HomeKit
record. A legal boundary completion no longer loses the next record. Framing
faults, including allocation failures after partial nonce progress, permanently
retire both directions and cleanse keys and decrypted temporary storage.
Encoding reserves the exact wire size before advancing counters.

The event input parser drains complete requests/responses before retaining their
successors, so a TCP read completing a legal 64 KiB message and beginning the next
one does not retire the session. The retained-message limit still includes its
header and body. A portable encrypted fixture reproduces that boundary and
rejects a body whose header would exceed it; this does not qualify a live phone.

Event iAP output checks its actual plist, request-header and authenticated-record
size against remaining queue capacity before encryption. Insufficient capacity
returns false without consuming CSeq or nonces; a smaller following command
remains admissible. Allocation failure while packaging or queueing cannot leave
a resumable channel with missing records. The plist encoder owns its C buffer
through C++ allocation failures.

An event port admits one socket per control generation. EOF, authentication
failure or failed writes retire that owner and propagate through the existing
control-session failure path. Reconnecting cannot reset counters under the same
secret. Loopback fixtures cover event acceptance both before and after RECORD,
EOF, a rejected tag, refused reconnection and terminal repeated drain. These are
Linux component fixtures, not new phone interoperability evidence.

Portable framing tests inject each C++ allocation failure in two-record send,
receive and event packaging. Strict CM5 GCC `-O2` and ASan/UBSan/LSan runs pass
the allocation fixture. Control request tests also reject duplicate
Content-Length fields and respect a smaller caller budget before a complete
header arrives.


## Registered data services

The session transport now distinguishes type-130 services by clientTypeUUID.
An omitted service UUID retains the classic iAP behavior. Additional UUIDs must
be explicitly registered before session setup. The receiver registers the
configured UI, asset and vehicle consumers before advertising their services;
an unconfigured diagnostic does not advertise them. Registration/transport
success is not application acceptance. See [UI integration](UI_SESSION.md).

Registered streams have independent ports, IDs, encryption state and bounded
input/output queues. At most eight services can be registered, with twelve
stream allocations per control session including closed entries. IDs increase
within the session. Seed reservations include classic iAP and survive selective
close, EOF and authentication failure. A generic service permits one TCP
connection only. A failed channel closes without resetting its nonce counter
or interfering with other channels.

The consumer receives the service UUID, stream ID and complete APTransport
message. It sends replies explicitly, including any transport acknowledgement;
the transport layer does not pretend that an unhandled application message
succeeded. The inbox allows 64 messages and 2 MiB including headers; each
outbound channel allows 2 MiB. The poll loop drains nonblocking writes.

The reliability candidate uses a bounded output cursor for event and registered
service sockets. Partial writes advance that cursor without shifting remaining
ciphertext on the control thread. Appending compacts only when needed to keep
stored bytes within the original bound; admission counts unsent bytes. Wire
order, encryption counters, one-send-per-drain scheduling and retirement remain
unchanged. The queue fixture compares 20,000 randomized operations to a reference
FIFO, injects allocation failure after compaction, and forces real nonblocking
socket partial writes followed by an append. Existing encrypted loopback session
tests verify event and registered-service admission and output.

Selective TEARDOWN currently supports registered type-130 streams by streamID.
It validates the entire selection before closing any channel. Unknown IDs,
duplicate IDs and unsupported types receive 400 without disturbing the session.
An empty TEARDOWN returns 200 and then closes the full control/media session.
Only that full-session close retires RECORD and main-presentation authority;
closing an unrelated data service leaves the current presentation owner alive.
Selective closure of classic iAP/video is not implemented.

When a dual-mode receiver selects Standard, initial SETUP does not agree the
Ultra vehicle-state protocol whose resources are omitted from Standard `/info`.
Complete UI-Sync/HEVC agreement retains that configured service offer; standalone
non-Ultra empty-profile diagnostics include their explicit vehicle service in
both initial and post-SETUP `/info`. Authenticated smoke offers only
`vehicleStateProtocol`, checks agreement and retained resource information,
and checks the actual received-feature mask of 4. This fixes
a conditional capability mismatch. It has not established the cause of the live
Standard pre-stream failure: the earlier phone run completed SETUP, `/info` and
RECORD, then requested TEARDOWN before any video stream SETUP. Its encrypted
capture does not reveal the original offer without ephemeral session keys.

On 7 October, the authenticated loopback smoke reproduced a control-owner defect
in the earlier CM5 binary: closing an unrelated registered data service moved
the production presentation publisher to `fault`. The current source retires
RECORD/presentation only on full-session TEARDOWN. The same smoke passes on
the integrated CM5 build with an explicit empty vehicle fixture: a fresh
post-dispatch publication retains the same Standard connection in `starting`,
`/info` remains usable, and full TEARDOWN publishes `disconnected` without
readiness. Synthetic video and this publisher check do not qualify rendering,
a phone session or physical outputs. Private before/after logs and exact source
and binary hashes are retained with the scoped build evidence in HANDOFF.

Pi loopback tests verify two services alongside classic iAP/video, independent
encrypted replies, fragment reassembly, seed collisions, malformed selections,
selective close and reopening with a fresh seed/ID, EOF, and isolation after a
bad authentication tag. These are synthetic transport checks, not live Ultra
UI or signed asset transfer proof.


The updated synthetic connection client also passes normal pairing, verification,
transient pairing, complete multi-record /info replies, two video streams,
RECORD, an invalid selective TEARDOWN followed by a successful full TEARDOWN,
and bad-signature rejection. Its send/receive counters are independent and it
reassembles replies using Content-Length rather than assuming one encrypted
record per reply. The full Pi unit suite passes 29/29 after this integration.
