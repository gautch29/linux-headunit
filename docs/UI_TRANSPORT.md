<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Receiver-to-theme UI transport

`headunit-theme-transport` joins the independent receiver's `DataMessage` and
bounded OPACK codec to our reused per-display `script::Channel`. It requires
no original cloned LIVI source, service or binary.

## Ownership and routing

One `UiTransport` belongs to one negotiated DataStream ID and a fresh set of
connection-scoped display VMs. The owner prepares each runtime from a verified
immutable AssetBundle, registers video inputs and vehicle bindings, then starts
the adapter and the asset's tasks. A configured route maps the advertised physical panel identifier
(for example `Cluster_Display`) to the signed asset's display name.
UUID-form panel routes are also supported, separately from video-stream IDs. Case-insensitive UUID matching does not rename the
asset's internal display or allow incoming messages to create a new one.

The receiver authenticates and decrypts records in `AirplayDataStream` before
passing `DataMessage` to `receive()`. Only asynchronous inbound `comm` messages without
an error are currently accepted. Synchronous requests/replies are explicitly
unsupported. The `comm` inbound / `cmnd` outbound contract now completes
the live CM5 layout exchange and produces the embedded map. The registered service UUID and negotiated stream must be checked
by the receiver owner. This adapter rejects messages for a different stream ID.

The adapter uses the independent OPACK codec directly. Its typed boundary to
`script::Channel` avoids serializing and parsing the envelope a second time.
The theme's existing Lua session owns RESET, sequencing, acknowledgement,
duplicate rejection and recovery. The adapter does not add another protocol
session or rewrite those fields.

## Bounds and output lifetime

Incoming envelopes have the receiver's 512 KiB binary-plist limit. The session
root and optional nested command must be bounded binary dictionaries before
they enter a VM. Graph checking precedes libplist allocation and rejects cycles,
invalid references, duplicate keys, excessive depth and repeated expansion.
`CP.parsePlist` also checks binary graphs at its existing 4 MiB / 16,384-node
host limits so a subsequently interpreted nested DATA object cannot bypass the
check. XML support for existing asset resources is unchanged; this is not a
claim of a new XML parser audit.

The receiver repeatedly obtains `pending()`, attempts
`AirplaySessionSetup::send_service_message(id, *pending)`, and calls `accepted()`
only when the complete message is accepted into the transport queue. A busy
socket leaves the same message held, without duplication or loss. Reacquire the
pointer after every VM turn: `CP.reset`, disconnection and faults increment a
per-display generation and invalidate that display's unsent message. A reset
in one display leaves a held message for the other display intact. Bytes already
accepted by the encrypted transport are transport-owned and cannot be recalled.

All calls run on the VMs' owner thread. Never tick a VM between successful
transport enqueue and `accepted()`. Closing the adapter clears its held message
and disconnects the channel. It cannot restart; a reconnect creates fresh VMs
and a new adapter, with independently negotiated crypto state. The owner must
also hide failed/disconnected Ultra output and select the backup cluster.

## Verification

Synthetic tests exercise byte-at-a-time encrypted input through the real
DataStream decoder, display routing, Lua receive/send and encrypted output
opened with the opposite-direction key. They also cover replay rejection,
unknown/wrong display and stream, unsupported headers, malformed/nested cyclic
plists, queue pressure, disconnect, reset invalidation and display isolation.
They contain no phone credentials, captured packets or Apple source.

The private signed-package probe now routes both displays' real
`APL.ClusterControlSession` through this adapter. It verifies RESET/ready,
commands, ACK, duplicate rejection and a gap-triggered reset. This is a simulated
peer handshake using the actual theme, not a live phone session.

On 2026-09-26, all 36 Mac tests and 51 Pi tests passed. The binary-plist and
UI transport tests also passed with AddressSanitizer and UndefinedBehaviorSanitizer
set to stop on errors, as did the signed-package handshake probe. The Pi's full
reference layout still initialized and rendered both displays on V3D after the
parser change, using simulated values and the explicit offline GPU deadline.
This adds no new live-phone or sustained frame-rate result.

## Remaining live integration

The dedicated script owner and socket lifecycle snapshot are now implemented;
see UI_WORKER.md for transport ownership, cancellation, and the presentation
bootstrap dependency found with the full theme.

The receiver now has an opt-in service consumer with a tested connection
supervisor, verified installed asset selection and copied real clock snapshots.
See UI_SESSION.md. The explicit `--ultra-negotiate` lab option advertises the
installed Ultra service and panel configuration; it is not enabled by default.
Live asset transfer, native decoder routing and display/fallback arbitration
remain incomplete. The GPU render times and layout caveats in GPU_RENDERING.md
still apply. Neither video presentation nor Apple Climate is established by a
UI handshake test.

Outgoing UI messages use `cmnd`, matching our previously working authored bridge;
the phone sends `comm` in the opposite direction. The encrypted test checks the
different header bytes explicitly, rather than assuming a symmetric echo.
