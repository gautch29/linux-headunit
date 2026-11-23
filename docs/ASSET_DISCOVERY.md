<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Additional CarPlay Ultra asset packages

## Discovery result — October 1, 2026

**A second, substantive package has been obtained from Apple's public delivery
service: `44C9FA31EC-Single`, accessory version `547`.** It is a distinct signed
bundle, rather than another copy or version of the installed `44C9FA31EC / 985`.
Its cluster catalog adds **SingleDigiRing, SingleDigiBandTicks and SingleDigiBand**.
Despite the `Single` suffix, its accessory metadata still declares both
`Cluster_Display` and `Center_Display`, each at **1920 × 720**, density 157.
The suffix must not be interpreted as a one-display receiver requirement.

Two further signed identity variants were downloaded: `44C9FA31ECCN / 985` and
`44C9FA31ECCN-Single / 547`. These belong to the same Aston Martin visual family
as the installed reference, which the user identifies as Aston Martin. This discovery
does **not** establish availability of a Porsche or another automaker's theme.
The CN designation's intended market was not independently qualified; its suffix
alone is not evidence of a new manufacturer or design.

The downloads and detached signatures are verified, but **fresh strict
verification rejects all four packs**: their accessory-signing certificate
expired on September 22, 2026. The discovery run left the active CM5 configuration
unchanged and executed no downloaded scripts. A subsequent, user-requested
per-profile development option now permits expired signing leaves while keeping
chain/archive verification. Bounded runtime checks are described below. No
acceptance receipt was manufactured. Finding a package and fully qualifying it
for runtime use remain separate gates.

## Official delivery provenance

The evaluated samples came from Apple's asset catalogue. The public source
candidate contains documentation of the observations. The catalogues, signed
archives, certificates, scripts and imagery remain external.

## Accessory verification results

All three accessory ZIP signatures verify over SHA-256 with the accompanying
Apple signing leaf. Their signed inner `Info.plist` identifiers/versions agree
with the catalog and outer metadata. The leaf is `Carplay Asset Production
Signing`, issued by `Apple Application Integration CA 5 - G1`, valid from
August 27, 2025, 01:57:05 UTC through September 22, 2026, 19:54:11 UTC.

| Accessory bundle | Regular resources | Expanded bytes | Inner ZIP SHA-256 |
| --- | ---: | ---: | --- |
| `44C9FA31EC-Single / 547` | 3,225 | 38,065,100 | `4fae17e431ff2912e4a18067a83b3a20975837215cefca7aba86f8fcf95558b2` |
| `44C9FA31ECCN-Single / 547` | 3,225 | 38,065,104 | `df5efefcacb88b6ffe29ee23f0a5c75ab569c66773ff2758cf79d5cd6ccd8a5b` |
| `44C9FA31ECCN / 985` | 3,262 | 37,861,841 | `8fb05037c9fadf11678c80d772be9fd2db81d4648fab00e1e4196d49ade30c20` |

The default `headunit-asset-check` rejects a candidate with **certificate outside
validity period**. `headunit-asset-inspect` likewise rejects fresh signing trust
without an explicit development option. Those default rejections were retained.
The installed reference's old receipt is specific to its three pinned files and
was not used for any newly obtained variant.

A separate forensic chain check at August 31, 2026, 12:00 UTC succeeds to our
pinned root. This proves that the chain was structurally valid at that chosen
time; it does not prove historical acceptance or supply an installation receipt.
The discovery run did not change the system clock or verifier policy. Revocation and authorized
use are not established by this diagnostic.

Read-only ZIP checks found no CRC errors, duplicate names, absolute/traversal
paths, backslashes, encrypted entries or symbolic links. Every resource remains
below the reader's 16 MiB limit, and both expanded size and entry count remain
inside its limits. These checks do not replace the reader's trust gate or prove
Lua/renderer compatibility.

## What is actually different

The comparison hashes every regular accessory resource against the privately
installed reference, without executing downloaded Lua.

| Candidate | Changed shared resources | Added / removed paths | Catalog difference |
| --- | ---: | ---: | --- |
| `44C9FA31EC-Single / 547` | 952 | 74 / 111 | Three extra single-gauge cluster layouts |
| `44C9FA31ECCN-Single / 547` | 952 | 74 / 111 | Same three additional layouts |
| `44C9FA31ECCN / 985` | 2 | 0 / 0 | Same layouts as reference |

For each Single variant, the changed set includes 778 images, 171 Lua text
resources, two compiled Lua resources and `Info.plist`. Resource changes are
not a claim that every shared layout has a different design; the older build
also changes packaging and implementation data.
The full CN variant changes only `Info.plist` and `r/PackageData.lua`. The exact
differences are `CFBundleIdentifier` and the package's `identifier` string:
`44C9FA31EC` becomes `44C9FA31ECCN`. Its images, compiled Lua and every other
resource match the reference byte for byte. No Chinese-specific artwork or
different cluster design is established by this accessory comparison; phone-side
resource behavior remains a separate question.

The Single variants retain the six primary cluster styles already known:
Cobranded, DigiRing, Modular, DigiBandTicks, DigiBand and MapsMedia. They add
SingleDigiRing, SingleDigiBandTicks and SingleDigiBand. The center display retains
Overlay and Wallpaper. Minimum SDK remains `19.0` in all candidates.

The added entries are declared by `displayName`, `id` and `layoutStyleID` under
the cluster's `layouts` in the detached-signed resource `r/PackageData.lua`:

| Display name | Layout ID | Underlying style |
| --- | --- | --- |
| SingleDigiRing | `12F8041E-F089-54D5-ABD2-68C6975A5AA5` | DigiRing |
| SingleDigiBandTicks | `572441BE-CD84-5A8A-926F-C0B1BF4BC472` | DigiBandWithTicks |
| SingleDigiBand | `D1040B8C-C916-5F45-9C76-B0460DD5CB19` | DigiBand |

## Development runtime checks — October 1, 2026

`allowExpiredAssetCertificate=true` in a private installed-UI profile opts into
the [leaf-expiry policy](ULTRA_ASSETS.md#development-option-for-an-expired-signing-leaf).
It retains a separately pinned root and validates the archive's detached
signature. It does not manufacture past acceptance, change the clock, modify
signed resources or disable chain verification.

- All four packs pass the immutable signed-bundle reader and bounded layout
  catalog probe under this option. The two regular packs declare six primary
  cluster layouts; the two Single variants declare nine.
- All three newly downloaded packs pass the bounded two-panel script/UI exchange
  probe. This uses a synthetic peer, not an iPhone.
- All nine `44C9FA31EC-Single / 547` styles pass CPU rendering with an explicitly
  simulated basic dark vehicle fixture. This fixture includes gear but avoids
  the still-unqualified extended temperature-marker composition.
- The three additional Single layouts pass CM5 GPU worker preflight on both
  physical outputs. This proves startup/render compatibility with synthetic
  timing, not live video, visual completeness or sustained frame rate.
- Each of the four real CM5 installed-UI profiles opens with `true`, rejects
  with `false` or malformed `yes`, and the original receipt-based profile still
  opens unchanged without the option.

Fresh iPhone setup, live maps/touch, style switching and richer controls remain
separate qualification checks. The Single candidate has its own private profile
and layout-selection file; the original receiver configuration is backed up
before live comparison.

### Initial Single phone test

The receiver advertises `44C9FA31EC-Single / 547` from the verified bundle and
starts `SingleDigiRing`. The user confirms main CarPlay and the single-gauge
cluster on the two physical CM5 screens, then confirms main touch and Apple's
Climate controls work. Retained Climate writes were not retested in this pack.
The captured cluster shows the centered
speed gauge with simulated speed and a fuel icon; no fuel reading is bound in
this basic fixture. This is not a full-car sensor test.

The session's live layout status lists all nine choices but reports
`ready=false`; live style switching is therefore not yet qualified for this
pack. Initial cluster video is decoded but no cluster video frame has been
presented in the sampled reports. Do not infer Maps compatibility or complete
UI synchronization merely from visible gauges. The reference pack's previously
verified live switching/maps remain separate evidence.

After this initial comparison, the receiver's original receipt-based profile
was restored. Candidate profiles remain private and recoverable for the next
tests. The default/reference profile has no expiry exception enabled.

### CN regular phone comparison

At the user's request, the receiver then advertises `44C9FA31ECCN / 985`. The
user confirms both physical views. The cluster capture shows Cobranded with
simulated speed, fixed gear `3` and the Aston Martin artwork. The sampled layout
status still reports `ready=false` and the center has no embedded map, so this
comparison verifies display startup rather than completed UI synchronization,
Maps or live style switching.

### CN Single phone comparison

The receiver then advertises `44C9FA31ECCN-Single / 547`, with
`SingleDigiBandTicks` selected at startup. The user confirms both physical views;
the capture shows its centered speed gauge, orange fill and tick marks. The
sampled live selection still reports `ready=false`. This is another successful
main/gauge startup comparison, not qualification of Maps, live switching or
complete UI synchronization. CN-specific Climate, retained writes, media and
long-running behavior have not been retested.

CN Single was left active at the user's request to see the other packs. The
original receipt-based installed-UI profile remains unchanged, and the private
`receiver-before-single-20261001.json` preserves its receiver selection. Only
the explicit candidate profiles enable the expiry exception.

### Full-screen map restored, 2 October

The next CN Single comparison selected Cobranded, then MapsMedia. Local
instruments drew, but the user saw no map even with Apple Maps open and a route
active. The live selector reported `ready=false`; early cluster counters showed
decoded video with no staged/presented video. This does not establish a capture
or GPU-import failure: the signed view also gates video visibility on its
UI-Sync session and layout-transition completion.

Selecting the original regular `44C9FA31EC / 985` profile with MapsMedia restored
the map. The user confirms the full-screen map and the receiver counted actual
cluster video presentations. The regular pack and MapsMedia preference are now
saved. This lab profile uses its explicit expired-leaf policy; source defaults
and the original receipt-based profile are unchanged. Live switching and map
activation in the alternative packs remain open qualification gates.

## Private evidence and the next qualification gates

All third-party files remain under ignored `captures/asset-discovery-20261001/`.
Only this discovery report is repository documentation.

- `<identifier>-<version>.zip` and `.metadata.json`: outer download and provenance.
- `<identifier>-<version>/CarPlayAccessory.assets.zip`: signed accessory archive.
- The adjacent `CarPlayAccessory.assets.sha256`, `Certificate.chain.pem`,
  `AccessoryAdditions` and `Info.plist`: original delivery inputs.
- `pallas.jwt`, `pallas.payload.json`, `catalog-verification.json`: official
  catalog response and verification result.
- `verification.json`, `zip-integrity.json`, `resource-comparison.json` and
  `compatibility-summary.json`: signatures, current rejection and offline facts.
- `single-layout-identities.json`: exact signed resource, line locations and
  identifiers for the three additional styles.
- `development-policy-check.json`, `development-runtime-check.json`: exact
  reader/probe outcomes, catalog identities and nine CPU style checks.
- CM5 `cm5-profile-check.json`, `cm5-gpu-preflight.json` and per-style logs:
  real loader policy checks and synthetic GPU startup results.
- CM5 `single-live-check.json` and `live-SingleDigiRing.png`: initial phone
  comparison, user confirmations, exact selection status and display capture.
- CM5 `cn-regular-live-check.json` and `live-CN-regular.png`: CN regular phone
  comparison and exact selection/video state.
- CM5 `cn-single-live-check.json` and `live-CN-SingleDigiBandTicks.png`: CN Single
  phone comparison and the final active candidate.

Before a candidate becomes a qualified runtime pack:

1. Prefer a currently acceptable signing chain/package. For this user-requested
   comparison, explicitly opt into the implemented leaf-expiry development
   policy while keeping chain and signature checks. Do not fabricate past
   acceptance or silently bypass expiry; strict fresh qualification remains open.
2. Run the immutable signed-bundle reader and bounded runtime probe against the
   exact identifier/version. Check host calls, resources and all nine cluster
   layouts; the older Single package is not automatically runtime-compatible.
3. Validate phone-side and accessory-side version pairing, additions, ownership,
   both panel descriptions and their canvases in a fresh iPhone session.
4. Verify main touch, live layout switching, maps and media, Climate/settings,
   disconnect fallback, rendering cost and sustained two-display operation.
5. Keep the working reference and receiver configuration recoverable throughout
   the comparison. Do not redistribute Apple's bundle with the source tree.

## Broader search outcome

Eight additional public catalog queries using iOS versions 18.5, 26.0 and 27.0,
SDK ceilings 18.0, 19.0 and 26.0, and Luck/Clearwater train values all returned
the same four entries and posting date. A server may ignore unsupported filters;
this is not proof that other internal or future catalogs cannot exist.

The iOS and macOS Mesu paths for this asset type returned HTTP 403. No attempt
was made to bypass that response. Public GitHub searches for CarDDPAssets,
CarPlayAccessory asset archives and the known identifier produced framework
analysis/entitlements rather than another delivered theme. The installed
Xcode beta CarPlay Simulator plug-in exposes ordinary vehicle configurations;
no signed Ultra accessory ZIP was found in its inspected resources.

The useful discovery is the official Single package with three additional
layouts. Another manufacturer's package and a currently acceptable fresh
accessory-signing chain remain unverified.
