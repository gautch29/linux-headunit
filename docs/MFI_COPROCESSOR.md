<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# MFi authentication coprocessor

Hardware authentication is the default. The receiver uses its own C++ register
driver for the authentication chip on the Pi. The Linux transport uses `/dev/i2c-1`, seven-bit address
`0x10`, and separate `I2C_RDWR` transfers for register selection and data.
Only one process should use the chip during authentication because register
selection and reading are separate transactions.

Apple's [public HomeKit ADK register map](https://github.com/apple/HomeKitADK/blob/master/HAP/HAPMFiHWAuth%2BTypes.h)
documents the version, certificate, challenge, control, response, and error
registers. Its [authentication flow](https://github.com/apple/HomeKitADK/blob/master/HAP/HAPMFiHWAuth.c)
describes a protocol-3 chip consuming a 32-byte digest and producing a 64-byte
signature. The driver is independently written from those interface facts and
observed bus behavior. It contains no HomeKit ADK or LIVI source.

On 2026-09-25 a private kernel I²C trace captured one successful LIVI
authentication cycle. Its metadata showed:

- Register `0x30` selected and a two-byte certificate length read. Register
  `0x31` was then selected and the whole 607-byte certificate read in one
  transfer.
- Register `0x20` received the two-byte challenge length `00 20`; register
  `0x21` received a 32-byte challenge. Register `0x10` received a one-byte
  start command.
- Register `0x10` was polled until ready. Many transfers returned `EIO` while
  the chip was busy. Register `0x11` then returned a two-byte response length,
  followed by a 64-byte response from register `0x12`.

The independent driver follows this sequence and retries temporary I²C NACKs
with a finite bound. On the Pi, its read-only probe reported device version 7,
protocol 3.0, and a 607-byte certificate. A synthetic all-zero digest returned
a 64-byte signature. Neither certificate nor signature bytes were printed.
These tests establish chip transport and signing, not yet phone acceptance of
the independent receiver.

The independent receiver now uses the same coprocessor owner for Bluetooth
iAP2 authentication and authenticated IP pairing or `/auth-setup`. Real wireless
CarPlay authentication is qualified in [the wireless guide](WIRELESS_HANDOFF.md);
that evidence is separate from a standalone signing probe. Synthetic register
buses verify the certificate/challenge exchange, HomeKit identity proof and
AirPlay encrypted signature without accessing a chip. The phone challenge
mapping and register sequence remain unchanged.

## Signature deadline and failure

One positive deadline of at most two seconds covers the complete signature
operation, starting before protocol lookup and challenge writes. The owner
checks the original deadline before and after every completed register operation,
including response/error reads. A late ready bit or signature is rejected;
retryable busy/NACK reads cannot renew the deadline. The poll sleep is capped
at the remaining time. An invalid deadline touches no register. Authentication
callers receive the failure through their existing retirement path.

This is a logical operation deadline, not kernel cancellation. The Linux bus
still performs synchronous `I2C_RDWR` operations with finite NACK retries. A
blocked driver call can exceed the logical limit before returning; its actual
whole-owner bound requires hardware measurement. Do not introduce a second bus
owner or detach an authentication thread that can outlive its connection.

On 7 October, strict portable component checks pass for delayed success at all
eight signature phases, busy/NACK timeout, recovery with a fresh attempt and
invalid deadline rejection. The fixture delays actual calls on a synthetic bus
using the host clock; it establishes late-result rejection, not a physical I2C
timeout. iAP2, authenticated pairing and AirPlay auth-setup fixtures also pass.

Raw traces, certificate data, challenge data, and signatures remain in ignored
private captures or process memory and are not committed to Git.

## Software authentication research

The feasibility study compared certificate handling and prehashed P-256
challenge signing with an external software implementation. Synthetic tests
check signature format, changed-digest rejection and matched-key admission.
[Source provenance](PROVENANCE.md) records the implementation boundaries.

## Optional software provider

As of 9 October, the user has deferred this experiment. Its implementation and
explicit research configurations remain, while software-provider CLI flags are
suppressed in normal launcher help. The native phone/settings interface offers
no MFi backend selector; hardware remains active/default. The setup below is
retained engineering documentation, not a supported user-facing option.

Implemented on **8 October 2026**. `MfiAuthenticator` is the certificate,
protocol-version and digest-signing boundary shared by Bluetooth/iAP2,
authenticated pair-setup and AirPlay `/auth-setup`. `MfiCoprocessor` retains
the hardware register driver and deadlines. `SoftwareMfiAuthenticator` loads
an explicit external identity once before listener/radio startup, uses OpenSSL
to sign the precomputed digest, and returns the same 64-byte signature format.
No DiPlay/xcertplay implementation was copied.

The default remains `mfi_backend: "hardware"`. A standalone test configuration
can explicitly select:

```json
{
  "mfi_backend": "software",
  "mfi_private_key": "/private/experimental-mfi/identity.pk8",
  "mfi_certificate": "/private/experimental-mfi/certificate.p7b"
}
```

These fields extend an otherwise complete receiver JSON; the fragment alone
is not a runnable profile. Both files must be distinct, canonical regular
files owned by the receiver user, mode **0600**, singly linked, nonempty and
at most **16 KiB** each. The private key is unencrypted DER PKCS8; the certificate
is DER X.509 or a DER PKCS7 container with exactly one certificate. Both keys
must be P-256 and match. Trailing data, links, oversized or changed files and
bad keys fail before startup. Encoded private bytes are cleansed after loading.
The software signer retains the positive logical deadline of at most two
seconds and rejects late completion; it does not preempt an OpenSSL call.

There is no automatic fallback between providers. Software mode does not open
the hardware bus. The launcher clears all inherited MFi provider/path/bus
environment and passes only the configured provider. Omitting the backend
restores the physical chip; software credential fields must also be removed.
Direct receiver equivalents are `HEADUNIT_MFI_BACKEND`,
`HEADUNIT_MFI_PRIVATE_KEY`, and `HEADUNIT_MFI_CERTIFICATE`, with the existing
`--mfi-auth` argument. Hardware keeps `HEADUNIT_MFI_DEVICE` and
`HEADUNIT_MFI_ADDRESS`.

`headunit-software-mfi-check PRIVATE.pk8 CERTIFICATE.p7b` checks local key
consistency and signing without radios. The Linux receiver offers the same
preflight via `headunit-control-probe --check-software-mfi PRIVATE.pk8 CERTIFICATE.p7b`;
the launcher invokes it as the receiver user during `--dry-run` and before a
live launch. It reports no key, certificate or signature bytes. Neither local
preflight nor a self-signed fixture establishes Apple trust.

Portable regression checks cover raw digest/signature verification, altered
challenges, certificate-copy ownership, DER/PKCS7 input, mismatched/wrong-curve
keys, malformed/oversized/trailing input, permission/link/FIFO rejection and
invalid deadlines. Synthetic software iAP2 covers phone rejection followed by
a fresh successful exchange; authenticated pair-setup verifies the SRP-derived
proof; AirPlay verifies the decrypted signature over the actual ephemeral keys.
Existing hardware register/deadline and protocol fixtures remain enabled.
Launcher checks cover default/inherited-provider isolation, complete/distinct
paths, and invalid-identity rejection before radio/session creation.

The experiment uses separately provisioned private credentials and an
isolated pairing store. The original V13 release used hardware
authentication. The checks above establish software behavior with synthetic
inputs. See the later phone trial for the live qualification boundary.

### Scoped verification — 8 October 2026

The full strict portable build passes **131/131 CTest checks**. The isolated
CM5 Linux/GPU/video build completes and all **174 Linux checks** pass across
the initial run and targeted reruns. The initial run passed 170/174; four
release/startup fixture suites rejected ordinary temporary-file permissions.
Two passed with the documented private `test-tmp/` and umask `0077`; the remaining
two passed after removing group/world write permission from the extracted
workspace. No validation rule was weakened. Linux uses the existing disclosed
GNU bench exception `-Wno-misleading-indentation` and RelWithDebInfo
`-O2 -g0 -DNDEBUG`; this is not a default warning-clean GNU build. An earlier
Debug build exhausted CM5 storage; only this experiment's incomplete build was
removed before the successful compact build.

The actual new CM5 control receiver passes a synthetic socket lifecycle using
the external experimental identity: authenticated SRP pair-setup and encrypted
`/auth-setup` signatures verify against its certificate. The fixture also passes
encrypted control/events, SETUP/RECORD, selective presentation survival, full
retirement and bad-signature rejection. Software mode succeeds with a deliberately
unusable I2C device path; a missing software key exits before opening the listener.
It uses fresh temporary controller records, port 7001 and no Bluetooth, AP or
display owner. This is synthetic receiver evidence, not real iAP2 radio or phone
evidence. The prepared private `receiver-software.json` passes the launcher's
production preflight and `--dry-run` without starting radios.

Frozen evidence is retained in `/home/headunit/headunit-software-mfi-20261008/`:
`input-receipt.json`, `qualification-receipt.json`, build/CTest logs,
`production-smoke.py`, its log and `dry-run.log`. Source archive SHA-256:
`d8b99e3950be8e0f32eb0e0dbc24c474d71e8376a704c050cfd8abb0e3c90f5b`.
Implementation input digest (excluding README/docs):
`08300a2ca9e43460b9ee39ae9f393f5981a86a8f2654b9960e0e586860a8c87e`.
Control receiver SHA-256:
`071f11c90fe61afac639d9113ec8cc3e4cf64c20e14aa0fcee68dd565285520b`.
All 29 MFi implementation/configuration/test inputs still match this frozen
source. Concurrent later artwork/startup edits are outside these results.
The sanitized receipt is also saved locally under
`~/.cache/headunit-qualification/20261008/software-mfi/`.

After the isolated checks, V13's exact manifest and installed unit bindings
verify, and compositor, outputs, cluster, phone page and receiver services are
active. No sealed release was edited, published or selected by this experiment.
The option is ready for later retained-candidate phone qualification.

### Real-phone trial — 9 October 2026

The CM5 ran the retained `cm5-20261009-software-mfi-v32`, manifest
`40534542d2b6293cb02098173f37e733a4a49ac3cd84da9b0bb837627c5b810f`,
with source `e523369`. Its complete installed unit binding verifies. The explicit
test identity passes startup validation. The receiver reports `mfi-backend=software`. The physical provider's controller records remain
separate from `pair-store-software-mfi-20261009`; no pairing was removed or reset.

Four observed real-phone connections reach identification acceptance, certificate
exchange, challenge signing and `mfi-authenticated`, followed by
`wifi-settings-prepared`. This includes an explicit Disconnect/Connect and a
whole-service retry after the user offered to unlock the nearby phone. The first
connection predates this takeover; its private log is retained. The first three
attempts record no `0x4300` CarPlay-start request. The fourth, with USB phone logs,
receives `0x4300` and reaches `carplay-start-prepared` about 3.03 seconds after
iAP2 engine creation. Its authenticated start reply includes the installed asset
metadata. That is preparation/queue evidence, not proof of phone consumption.

No attempt reaches IP control or Ultra readiness. The fourth trial remains
unready through its bounded 90-second observation, then retires normally. Its
iAP2 coverage includes authentication success `0xaa05`, Wi-Fi settings request
`0x5702` and CarPlay-start request `0x4300`, with no rejected control messages.
The bounded private phone log shows protocol-3 certificate parsing with return
code zero, but does not identify a specific continuation failure. Bluetooth MFi
acceptance and a subsequent CarPlay-start request are now observed; software
provider `/auth-setup`, new pairing and full CarPlay acceptance remain
unqualified. The continuation failure's cause remains unknown.

The complete software graph was retired and retained hardware-authenticated
presentation-clock V31 restored and verified. That hardware control immediately
reaches fresh Ultra readiness with matching launch/catalogue identity, MapsMedia
confirmed and no error. Both providers use the same identity/network inputs;
the receiver configurations differ only in the explicit MFi backend/files and
isolated controller-store path after release-path normalization. This comparison
does not establish the phone's internal reason for refusing continuation.

Private logs, sanitized status/retirement and selection receipts are under
`~/.cache/headunit-qualification/20261009/takeover-testing/`. The V32 trial is
older source than the seven-worker/clock/gear integration and is not evidence
that the newest source was deployed or accepted. No sealed payload was edited.

The newest source also publishes an unselected software candidate V34, manifest
`be138fa3e3af777aea9e1b061924941cb50411b52d72e4e54a899b91af5fa66b`.
Current-tool production preflight passes, but the actually installed sealed
`boot-helper-20261006-v1` rejects `mfi_backend` as an unknown receiver setting.
V34 was not selected. The older V32 live trial did not reboot and does not
qualify recovery-helper admission. A software appliance needs separately
qualified retained-helper migration before selection/boot qualification; never
edit the active helper in place. V34 also enables the greeting whose production
budget failure is recorded in [the greeting guide](ULTRA_GREETING.md#real-phone-budget-failure--9-october-2026);
use the normal profile for the next isolated software continuation trial.
