<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Independent Ultra message encoding

The OPACK codec and per-display UI envelope began as offline components. They
now carry the connection-owned signed UI worker over registered encrypted
DataStream channels; see [UI session](UI_SESSION.md). The portable session and
timeline helpers below remain distinct from the live authored-script session.
The [iPhone interface audit](IPHONE_PROTOCOL_AUDIT.md) inventories known UI
commands, directions and qualification gaps. Codec tests alone do not establish
Ultra readiness.

## Origin and independent checks

Format facts came from the primary [pyatv protocol documentation](https://pyatv.dev/documentation/protocols/#opack)
and experiments with the installed macOS CoreUtils implementation. The C++
codec, test tools and envelope adapter were written from scratch in this
repository. No LIVI code, older native module, or third-party codec
implementation was copied or linked.

The reference table is useful but several details differ from the installed
Apple codec. Synthetic checks establish:

- Multi-byte integer payloads are signed and little-endian.
- Variable string/data lengths and extended reference indices use 1, 2, 4
  or 8 bytes, rather than 1, 2, 3 or 4.
- Nonempty strings, data longer than one byte, floats, and integers outside
  the compact -1..39 range enter the reference table.
- Duplicate literals occupy separate reference slots. Reading a reference
  does not insert another slot. Arrays and dictionaries are not interned.

The encoder deliberately emits literals without reference compression. The
decoder accepts verified reference forms, fixed and terminated collections,
UTF-8 strings, finite floats, signed 64-bit integers, booleans, null and data.
UUID/date objects, 128-bit integers and fragmented data remain unsupported
and fail explicitly. These are not silently converted to a different type.

Both directions enforce wire size, expanded-data size, node count and nesting
limits. Referenced data is charged again before copying, so a short message
cannot expand without bound. Duplicate dictionary keys, non-string keys,
trailing bytes, invalid UTF-8 and incomplete values are rejected.

## Verification

On 2026-09-26, 232 synthetic values passed
Apple encoder → independent decode/re-encode → Apple decoder, with value and
type equality checked. Cases include repeated reference objects, wide indices,
signed limits, Unicode, long collections, a per-display wrapper and a
200 KiB theme-chunk-shaped dictionary. This is serialization interoperability,
not a theme transfer or a live Ultra test.

The codec tests also cover malformed/truncated inputs and resource limits.
The deterministic random-input smoke check is run with address and undefined
behavior sanitizers. The optional macOS probe uses the installed private
framework only as a test oracle; no Apple framework is a Linux dependency.

Build the probe using xcrun clang++ with C++20, Objective-C ARC and the Foundation
framework from tools/opack_platform_probe.mm. Then run tools/check_opack_interop.py
with --platform-probe pointing to that binary and --reencoder pointing to
build/headunit-opack-reencode.

## Per-display UI envelope

The observed Ultra UI wrapper is an OPACK dictionary containing
displayUUID (string) and data (binary-plist bytes). That shape was
established in the earlier protocol investigation. This implementation accepts
only configured display UUIDs; incoming messages cannot create new displays.
UUID matching ignores hexadecimal case while returning the configured spelling.

The inner payload has a 512 KiB limit and must begin with the binary-plist
signature. **The envelope does not validate or execute the inner UI session
commands.** Session negotiation, sequence/acknowledgement rules, encrypted
channel routing, theme execution, timing and presentation are separate,
unfinished integration gates.


## Encrypted DataStream transport (offline)

AirplayDataStream adds original bidirectional record and APTransport framing.
It uses the paired secret plus a decimal stream seed for HKDF-SHA512, separate
input/output keys, authenticated two-byte lengths, and independent monotonic
ChaCha20-Poly1305 nonce counters. Each plaintext record is at most 16 KiB;
APTransport messages are bounded to 1 MiB and each feed call to 64 KiB.
The parser accepts fragmented or coalesced TCP data and preserves the message
kind, group, type, token, signed error, and opaque payload.

Authentication failures, malformed received packages and crypto failures latch
the instance closed and wipe its keys and buffered plaintext. A channel cannot
reset its counters. The future session registry must reject seed reuse within
a paired session; this module alone cannot enforce uniqueness across instances.

Tests cover independently generated fixtures for both directions and successive
nonces, every TCP split of a small record, coalesced packages, large messages,
replay, tampering, wrong seeds and bounded inputs. The transport tests also pass
with AddressSanitizer and UndefinedBehaviorSanitizer on the Mac. The transport is now wired into registered services in AirplaySessionSetup
and checked with synthetic socket peers. It does not replace the working iAP
channel or advertise unfinished Ultra services. See AIRPLAY_CONTROL.md.
