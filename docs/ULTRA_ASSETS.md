<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Independent theme transfer and verification

These components are newly authored. They use message-shape facts from the
recorded phone transfer and installed Apple interfaces; no LIVI implementation,
old experimental native module, Apple script, or theme is included in this tree.

## Transfer protocol

UltraAssetTransfer accepts OPACK dictionaries from the asset service payload:

- Header: messageType 1, payloadType 1, sessionID string, version integer,
  chunkCount integer, certificate and signature byte strings. An optional
  additions byte string remains opaque to the transfer layer.
- Chunks: messageType 2, matching sessionID, zero-based chunkIndex and data.
  The final chunk alone supplies a 64-character hexadecimal SHA-256 checksum.
- Final response: messageType 5 and numeric accepted 0 (committed) or 5 (failed).
  There is no header success response and no boolean acceptance conversion.

The archive is streamed into an explicitly supplied AssetSink. The receiver
never constructs a path from the phone's session ID and does not execute or
extract anything. Before acceptance, the sink must validate signature/trust,
signed asset identity/version, and local acceptance policy, then durably commit.
A matching SHA-256 is a corruption check, not permission to install a theme.

The parser bounds OPACK input to 384 KiB, expansion to 768 KiB, 64 nodes and
four nesting levels. It permits up to 4,096 chunks, 256 KiB per chunk and
64 MiB total archive size. Certificate/signature/additions limits are respectively
32 KiB, 4 KiB and 64 KiB. Session IDs have a 128-byte limit. Idle timeout is
30 seconds and total lifetime is five minutes, using a monotonic clock.

Missing fields, wrong types, mismatched sessions, reordered/duplicate chunks,
premature/missing checksums, failed writes/commit and timeouts terminate the
transfer and roll back staging. Disconnect/destruction also rolls back. One
transfer is allowed per instance; terminal instances cannot restart. The caller
must deliver any final reply and separately handle APTransport acknowledgements.

## Signature check

`verify_asset_signature` accepts a leaf-first PEM chain, DER ECDSA signature,
archive SHA-256 digest, and a separately configured PEM root. Only that root
enters the trust store. The supplied chain cannot establish trust by itself;
system CAs are not loaded. By default it checks the chain at current wall time, requires
an EC signing key of at least 256 bits and a non-CA leaf with digitalSignature
usage, and verifies the signature over the digest using SHA-256. PEM input is
bounded and trailing non-certificate data is rejected.

This is not a complete installation policy: revocation/authorization, additions policy, SDK compatibility and a durable
receiver-side installer remain to be implemented. The separate read-only bundle
reader now validates signed ZIP metadata and resources as described below. PEM is the current verifier input contract; transfer
certificate bytes stay opaque until an installer interprets them. No service
or fileTransfer feature is advertised by the live receiver yet.

`headunit-asset-check ARCHIVE SIGNATURE_DER CHAIN_PEM TRUSTED_ROOT_PEM` streams
the archive and runs this verification without installing or executing it.
Exit 0 means this signature/chain check passed; 2 means a verification rejection;
1 means invalid invocation or input/read failure. The checker requires nonempty
regular files, rejects final-component symlinks and special files without waiting
for a writer, and bounds archives to 64 MiB and signature/chain/root inputs to
4/32/32 KiB. Archive hashing remains streamed. Synthetic portable input tests
cover all four input roles; this is checker evidence only.

## Development option for an expired signing leaf

The private installed-UI profile accepts:

```ini
allowExpiredAssetCertificate=true
```

Omit the field or set it to `false` for strict current-time verification. It
applies only to that profile when the receiver next opens the bundle; it does
not change the system clock, system trust store, historical receipts or other
profiles. The [installed-UI example](../config/installed-ui.example.conf) keeps
the option off. The receiver JSON's `ultra_installed_ui` selects this profile.

This option waives **only expiry of the signing leaf**. The separately configured
root, chain signatures, issuer/root validity dates, signing usage, future
not-before dates and archive signature still have to pass. The immutable bundle
reader still checks ZIP bounds/paths/integrity and the exact signed asset
identifier/version before a runtime sees any resource. A modified archive, wrong
root, future-dated leaf or expired issuer remains rejected. No general
certificate-verification bypass is implemented.

The 9 October OpenSSL 4 compatibility repair replaces the deprecated current-time
comparison with `ASN1_TIME_cmp_time_t`. Only earlier/equal not-before results
admit the existing expired-leaf exception; a failed clock read or comparison
error rejects it. The chain verifier still owns certificate-format validation.
Synthetic certificate fixtures now construct and assign their own subject name
rather than mutating a borrowed getter. Asset and MFi tests pass with OpenSSL
4.0.3 on macOS and 3.5.7 on CM5, normally and with ASan+UBSan. A malformed
not-before fixture remains rejected. This is component evidence; the repair is
not in selected V31. See the [comparison return values](https://docs.openssl.org/master/man3/ASN1_TIME_set/).

For a newly obtained pack, configure its exact `assetDirectory`,
`assetIdentifier` and `assetVersion`, and **omit `acceptanceReference`** unless
there is a legitimate receipt for that exact pack. A supplied receipt remains
mandatory and hash-pinned even when this option is on; a bad or unrelated receipt
cannot fall back to the expiry exception. Keep `trusted-root.pem` separately
trusted rather than deriving trust from the candidate's accompanying chain.

The C++ API uses `AssetCertificateTimePolicy::allow_expired_leaf` in
`AssetBundlePolicy::certificate_time_policy`. `AssetBundleInfo::expired_signer_exception`
reports whether the exception was actually needed. The live loader logs
`expiredSigningLeafExceptionUsed=true` only after successful relaxed verification.
Current certificates and receipt-based reopening do not set that flag.

The read-only checker, inspector, Lua probe and theme probe accept a trailing
`--allow-expired-leaf`. For example:

```sh
build/headunit-asset-inspect /private/pack/archive.zip \
  /private/pack/signature.der /private/pack/certificate.pem \
  /private/pack/trusted-root.pem ASSET_ID VERSION --allow-expired-leaf
build/headunit-theme-probe /private/pack ASSET_ID VERSION - \
  --describe-layouts --allow-expired-leaf
```

The theme probe's `-` means no historical receipt. The physical benchmark reads
the same flag from its profile and preserves any supplied receipt. On the CM5,
`headunit-installed-ui-probe PRIVATE_PROFILE --validate-only` validates the real
loader without scripts, windows or a phone session.

### Validation — October 1, 2026

Mac and CM5 asset tests cover opt-in acceptance of an expired leaf, strict
rejection when disabled, altered archive/signature, wrong root, bad certificate
signature/usage, future or inverted leaf dates, expired/future roots and expired
intermediates. An invalid receipt cannot use the exception. Profile argument
tests check exact booleans, duplicates, optional receipts and benchmark policy
propagation. All four downloaded bundles pass the immutable reader with the
option on and still fail fresh strict verification. Their actual CM5 loader
profiles pass with `true`, reject with `false` or malformed `yes`, and the normal
receipt-based reference still opens without the option.

This remains a local development policy, not a new durable transfer installer,
revocation check, authorization decision or proof of phone compatibility. See
[the separate runtime qualification evidence](ASSET_DISCOVERY.md).

## Evidence, September 26, 2026

The C++ tests generate their own EC root, leaf and signed synthetic archive.
They cover a complete two-chunk transfer through signature verification, wrong
root, tampered digest/signature, expired/unsuitable certificates, malformed PEM,
missing metadata, chunk ordering and size bounds, 64 MiB total bound, storage
failure/rollback, disconnect, monotonic-clock rollback and idle/total deadlines.
The sink in these tests is deliberately a synthetic in-memory sink, not an
installer or proof that an Apple theme is ready to render.

Validation: Mac suite 22/22, Pi suite 30/30; the asset test also passes
AddressSanitizer and UndefinedBehaviorSanitizer on the Mac.

The private 30,821,866-byte reference archive was checked on the Pi using the
new independent checker. The result was certificate outside validity period.
No expiry exception, system-clock change or new installation was performed.
Previous research has a receipt for accepting that same archive before expiry;
any future reopening policy must independently validate that evidence and pin
the exact archive/signing material. It must not silently accept fresh expired
packages. The existing asset remains outside this repository.


## Reopening an exact previously accepted bundle

AcceptedAssetReference is an explicit local host attestation containing archive,
certificate-chain and signature SHA-256 values plus a recorded acceptance time.
It is never read from a phone message. All three hashes must match, the timestamp
must be in the past, and the chain and archive signature must verify at that
time. A current rejection other than certificate timing cannot fall back.
This is not a trusted Apple timestamp or revocation evidence. The ordinary
`verify_asset_signature` default still rejects expired fresh transfers.

AssetBundle::open takes an expected asset identifier/version and optionally that
receipt. It accepts bounded regular input files without following symlinks and takes
an immutable snapshot of the compressed archive (at most 64 MiB), verifies signing trust before parsing ZIP entries, and walks every
entry. Limits are 8,192 entries, 16 MiB per resource and 256 MiB expanded total.
It rejects duplicate paths, absolute/dot/traversal components, backslashes,
links, encrypted entries, special file types and file/directory conflicts.
Every regular entry is decompressed to verify its declared size and integrity.
The signed Info.plist must match the configured CFBundleIdentifier and
AccessoryContentVersion. MinimumSDKVersion is exposed as metadata; opening a
bundle does not claim that our unfinished runtime implements that SDK.

Resources are read by exact verified names from the immutable admitted resource
snapshot, not an extracted folder. Admission retains the exact decompressed
bytes it already checks, within the existing 256 MiB expanded-total limit, then
releases the compressed snapshot. Replacing inputs on disk or modifying a
returned resource copy cannot change the bundle. This moves memory retention
from compressed bytes to validated resource bytes; worst-case retained payload
is 256 MiB. No resource request reopens or rescans ZIP entries on the VM owner.
The runtime's separate bounded shared resource cache remains in place. Opening
and resource access never execute Lua.

headunit-asset-inspect ARCHIVE SIGNATURE CHAIN ROOT ASSET_ID VERSION
[LOCAL_ACCEPTANCE_REFERENCE] runs that read-only path. The optional local text
reference contains exactly four key=value lines: archiveSha256,
certificateSha256, signatureSha256 and acceptedAt (Unix seconds). The caller is
responsible for trusting that local receipt; the tool cannot turn untrusted
phone-provided metadata into acceptance history.

## Verified reference migration, September 26

On the Pi, the historical receipt records acceptance at 2026-09-19 08:46:46 UTC,
version 985, identifier 44C9FA31EC, with successful phone acknowledgement and
installed-store revalidation. Its time and archive hash agree with the earlier
local policy. All three installed-file hashes were independently recomputed
and matched that policy. The new reader then verified the signing chain at the
attested time, the signature, all archive entries and signed metadata.

Result: 3,262 regular resource files, 37,861,837 expanded bytes; main.lua is a
verified 3,119-byte resource. No scripts were executed. The exact previously
accepted archive, certificate, signature and configured root were copied into
independent private storage, reverified there, fsynced, and activated by a
directory rename. The source remained intact. The phone-facing receiver did
not accept an expired fresh transfer or advertise new Ultra capabilities.

Private Pi location: /home/headunit/ultra-headunit/captures/reference-asset/installed.
The adjacent acceptance reference/evidence files remain outside Git. Runtime
access no longer needs a LIVI asset folder. This one-time migration is not yet
a general durable installer for future phone transfers.

Tests cover exact historical reopening, wrong pins/timestamps, tampering,
expired fresh rejection, unsafe/linked/duplicate ZIP entries, signed metadata
mismatch, and immutable resource access after on-disk replacement. Mac 22/22
and Pi 30/30 pass, as do AddressSanitizer/UndefinedBehaviorSanitizer checks for
the expanded asset test on Mac.

## Additional packages discovered October 1

Apple's public catalog supplied three distinct additional signed bundles,
including `44C9FA31EC-Single / 547` with three additional cluster layouts.
Downloads and detached signatures verify. Their expired accessory-signing leaf
prevents fresh acceptance under the strict default; the explicit development
option above permits bounded runtime comparison without creating a historical
receipt. See
[asset discovery and qualification evidence](ASSET_DISCOVERY.md) for official
URLs, resource differences, private evidence locations and remaining test gates.
