<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Source provenance

## Native artwork and renderer

The public candidate uses original GTK/GSK materials, vector controls and
system typography. Earlier private revisions experimented with Apple-derived
graphics and personal system asset exports. The source export excludes that
shader, those assets and the export utility. Continuous-corner geometry uses
independently authored cubic segments and documented Cairo and Skia APIs.

This repository was initialized on 2026-09-25 with newly authored C++20
source, tests, and documentation. The initial files use only the C++ standard
library and CMake. At initialization, no source files, generated assets, or build products were
copied from the LIVI repository. Later migration of our own research additions
is described below and recorded in THEME_REUSE.json. The project is not linked to LIVI binaries or
its runtime service.

The optional Linux preview uses the system Wayland client API and generated
`xdg-shell` protocol bindings from the installed `wayland-protocols` package.
No generated protocol source is checked into the repository.

The original cloned LIVI source is excluded. The migration covers only
Ultra research modules authored during this project, including work first
written in a separate research checkout. Each migrated slice records its source hash and adaptations. A future
public release must review each added dependency's license, and must not
redistribute Apple-signed theme assets or private MFi materials.

For each future third-party dependency, record its upstream source, pinned
version, license, use, and whether it is distributed in the product image.

The independent phone-clock estimator is newly authored from the timing packet
format and the public four-timestamp relationship in RFC 5905. Its display-owner
adapter reuses our migrated MediaClock without linking any original LIVI clock
or transport code. PHONE_CLOCK.md records bounds and live validation.

The MFi register driver uses interface facts from Apple's publicly released
HomeKit ADK register map and authentication flow. It is independently written
and does not link or copy HomeKit ADK implementation code. The Linux bus uses
the system's `linux/i2c-dev.h` and `linux/i2c.h` kernel interfaces.

The optional software MFi provider (8 October 2026) is independently authored
against OpenSSL 3's PKCS8, X.509/PKCS7, EVP signing and ECDSA encoding APIs. The
protocol-3 digest/raw-signature boundary already existed in our receiver;
DiPlay `v0.2.14` at `7887bb7bf2b52258e663a2a4ea1332382ed80ad8` supplied the
external experiment's identity provenance and software-provider observations.
No DiPlay, xcertplay or original LIVI implementation was copied. Tests create
fresh synthetic identities. The experimental published-APK identity is kept
outside Git and is not project source or part of a sealed release payload.
[MFi authentication](MFI_COPROCESSOR.md) records its origin and evidence limits.

The temporary AirPlay advertisement uses Avahi's installed D-Bus service and
the public TXT-field descriptions in the openairplay protocol notes. Its
Ed25519 test identity is generated locally with the installed OpenSSL CLI;
neither the generated key nor any Apple or LIVI identity is committed.
The diagnostic binary `/info` response uses the system libplist 2.6.0 API,
which the Pi package identifies as LGPL-2.1-or-later. It is linked as a shared
system library; no libplist source is vendored into this repository.

The independently written TLV8 codec uses the field framing described in
Apple's public HomeKit ADK. No ADK implementation was copied or linked.
The SRP-6a server follows public RFC 5054 group parameters and Apple's public
description of its HomeKit pairing stages. It links the system OpenSSL 3
crypto library; no SRP or pairing implementation source was copied.
The later HomeKit identity exchange, X25519 verification, encrypted control
framing, and private pairing store are newly authored around the installed
OpenSSL 3 API and public protocol descriptions. `tools/smoke_pair_setup.py`
uses the Pi's Python `cryptography` package only as a test client; it is not
linked into the C++ runtime. No private keys, pairing records, phone
identifiers, or packet captures are committed.
The optional authenticated pair-setup branch uses the same original C++
crypto and MFi register driver, guided by the public method and TLV layout in
Apple’s HomeKit ADK. No ADK implementation source is copied into this project.
The separate four-message AirPlay transient path follows the published
openairplay protocol description; it reuses our independently written SRP and
encrypted-frame components.
The AirPlay `/auth-setup` handler is original C++ based on the published
AirPlay exchange description and observed request/reply sizes. It uses the
existing MFi register driver and OpenSSL; no LIVI implementation code or
certificate data is copied into this repository.

The optional phone-video preview links the Pi's shared system GStreamer 1.26.2
libraries (core, app and video APIs) and installed base/good/bad plugins.
Upstream is https://gstreamer.freedesktop.org/; the installed Debian copyright
files record LGPL licensing for the APIs/plugins used here. The current lab
uses the system V4L2 H.264 decoder and X11/Wayland sinks. No GStreamer source is
vendored. Deployment versions and their license files must be recorded in the
future product image manifest.

The AVC configuration/NAL converter, single-contact HID descriptor, input
report encoder, and video replay utility were newly authored in this repository
using format/interface facts and captured phone behavior. They do not reuse
LIVI code or the earlier experimental native modules. Private video and command
samples remain outside version control.

The independent OPACK codec uses format facts from pyatv's primary protocol
documentation and synthetic tests against the installed macOS CoreUtils codec.
No pyatv implementation or previous LIVI/native implementation was copied.
See ULTRA_MESSAGES.md for observed differences from the public format table.
The optional Objective-C++ platform probe is newly authored test tooling; it
does not ship or link Apple libraries into the Linux head unit.


The showUI event encoder is newly written from the command's observed shape
in the installed Apple CarPlay SDK and the instrument-cluster URLs advertised
by the test phone. No SDK or LIVI implementation was copied or linked. The
Apple SDK is not part of the Linux receiver or its build dependencies.

The music `duckAudio`/`unduckAudio` command parser, stream-owned latest request
and sample-count mixer envelope are original code. Exact command names,
`durationMs` units/default and duck `volume` decibel conversion/default were
established by inspection of the installed SDK during this project's research
(SHA-256 `ecde230aeabc015c90bcf15513023839b60db2649dd4107dffcac00503fe8912`;
annotated research `bf9db6455aeea7b0ba6f7c4a53397dab4f061c6dc13d46485d7bb122dfb7b152`).
No SDK implementation or original LIVI source was copied or linked. SDK files,
disassembly and phone captures stay outside Git; the new tests use synthetic
plists and encrypted AAC fixtures. The 60000 ms maximum is an explicit local
policy, not a discovered Apple limit. See [audio output](AUDIO_OUTPUT.md).

The generic DataStream/APTransport encoder and decoder are original C++ using
wire-format facts from the earlier protocol investigation. Synthetic fixtures
were independently generated using Python cryptography, with no phone secret
or captured payload included. The session adapter uses clientTypeUUID and streamID interface facts from
the installed SDK and earlier protocol notes. Its original implementation is
checked with synthetic sockets. That initial transport checkpoint did not
advertise live Ultra. The current opt-in receiver attaches independent UI,
asset and vehicle consumers before advertising their services; see
[Ultra integration](CARPLAY_ULTRA_GUIDE.md). Component provenance is separate
from the live qualification recorded there.


The independent theme-transfer state machine and signature verifier use only
recorded protocol field/limit observations and OpenSSL's public APIs. They do
not reuse the LIVI research implementation. Tests generate their own signing
keys/certificates and archive bytes. The read-only asset checker can inspect a
private external reference package; neither that package nor its signing
material is committed. See ULTRA_ASSETS.md for limits and verification scope.


AssetBundle and the read-only inspection utility are original code. They link
system libarchive for ZIP reading (3.8.9 on the Mac, 3.7.4 on the Pi) and libplist
for signed metadata parsing (2.7.0 / 2.6.0 respectively). No library source is
vendored. libarchive is distributed under BSD licenses; libplist remains a
shared LGPL dependency. Deployment must retain the packages' actual license
notices. The private reference bundle is third-party Apple content, not source
for this repository; its migration includes a local attestation of prior
acceptance and does not establish redistribution rights.

The new theme host embeds upstream Luau 0.739 (MIT), fetched from
https://github.com/luau-lang/luau with the source archive pinned to SHA-256
`7eca9d2e4362588e9ce95f2fa976e46252231564b29a88fe77f0806a4eae9b40`.
No Luau source is vendored. Distribution must retain its `LICENSE.txt` and
`lua_LICENSE.txt`. The new CP bridge, scheduler and synthetic fixtures were
written here from observed interface behavior. The runtime choice follows the
private theme's syntax; no Apple implementation or code from the original
cloned LIVI repository is copied into the host. Luau's public embedding/sandbox documentation is at
https://luau.org/getting-started/ and https://luau.org/sandbox/.

The binary property-list adapter and preflight graph validation were newly
written against the installed libplist headers and binary-format facts. The
Lua conversion bridge and display descriptor API are original host code.
Tests use manually constructed malformed graphs and a fixture generated by
Python's standard `plistlib`, without phone data. The session probe exercises
the privately verified Apple script as an external resource, using synthetic
peer messages; the script itself is neither copied nor distributed here.


The first reuse slice comprises our native scene model, Cairo reference
renderer, alpha masks, video frame ownership, Luau graphics bindings and their
synthetic tests. Source hashes and mappings are in THEME_REUSE.json. Those
files were local additions absent from the cloned repository's HEAD. Their
namespace is now `headunit::theme`; unused asset-store coupling was removed,
Luau destructor signatures were adapted, and tests now use the independent
host. No external LIVI includes, symbols, process, library or service is needed.
System Cairo (1.18.4 here, LGPL/MPL licensing) supplies reference pixels and PNG
reading. The full theme runtime, vehicle bridge and GPU compositor are now reused
as well. GPU rendering is verified separately in GPU_RENDERING.md. The independent
CM5 receiver has partial live theme/map success; full qualification is pending.

The 9 October PNG alpha fix links system libpng 1.6+ for bounded RGBA decoding.
It reads the pack's existing `hasPremultipliedAlpha` text declaration and keeps
premultiplied pixels intact; undeclared PNGs retain Cairo-compatible rounding.
No libpng source or signed PNG assets are vendored. Dependency/license inventory
must include the installed shared libpng alongside Cairo in a future product image.

The full runtime, script display channel, UI protocol codecs, vehicle state and
script bindings, local profile decoder, Linux sensor socket, and their tests
and private-package probe are also our earlier native additions. They are
listed separately in THEME_REUSE.json. The production resource callback reads
through the independent signature-checked AssetBundle; the old extracted-file
constructor is explicitly called TrustedDirectory and retained for controlled
fixtures. No original LIVI transport or app dependency was imported.

Our earlier numeric-table-key fix is reused in cmake/PatchLuau.cmake. Luau
0.739's two affected source files match the patch's pinned hashes exactly.
The patch guards floating-point-to-integer conversions used by table lookups;
it does not replace Luau's MIT license. CMake refuses unreviewed source changes.
This was required by the real theme's 2^31 table keys and verified with UBSan
configured to stop at the first error.

The GPU compositor, Skia adapter, Wayland window/presentation feedback, clocks
and original graphics/presentation tests were migrated from our own native
additions. The new masked-backdrop replacement/coverage path and its synthetic
pixel oracles were written in this independent repository. No original LIVI
application or transport implementation is linked.

Skia remains an external BSD-licensed dependency. The previously compiled Pi
archive was copied, with its headers, source/build receipt and license tree, to
`/home/headunit/ultra-deps/skia-a8363ad14693`. See SKIA_BUILD.json for the upstream revision and build recipe
scope and artifact checksum; this is reuse of a third-party library, not a
fresh verification of Git history. System GBM/EGL/GLES/Wayland supply GPU and
window interfaces. Their package license notices remain deployment obligations.

The receiver-to-theme UI transport and its encrypted synthetic tests are new
independent code. They combine our receiver codecs and reused script channel
through typed messages, without copying a LIVI transport bridge. The runtime's
new binary-plist preflight calls the independent bounded graph checker; its
per-display message generation prevents unsent packets surviving a script reset.

The connection-scoped UI worker, bounded inter-thread mailboxes, socket service
lifecycle snapshot and their tests were written in this independent repository.
They invoke our reused runtime through UiTransport; they contain no original
LIVI implementation or Apple asset source.

GpuDisplay adapts lifecycle and presentation scheduling from our own local
`native/carplay-ultra/src/ui_service.cpp` DisplayOutput class. The entire source
file was confirmed absent from upstream HEAD before reuse. It replaces the old
pipe clock with ReceiverClock and uses the independent worker; native input-pipe,
CPU reference rendering and capture code are excluded from this adaptation.

The gauge adapter is newly authored against this project's `VehicleSource`
interface. Its five semantic bindings and speed conversion follow our own
`BenchGaugeSimulation` in the earlier `native/carplay-ultra/src/ui_service.cpp`.
It reuses this repository's simulator and Unix socket adapter. No original
LIVI source or numeric Apple vehicle-protocol identifiers are imported.

Native Ultra touch uses this independent repository's `PreviewTouch` mapping and
Wayland callbacks adapted from its own `src/wayland_preview.cpp`. The connection
mailbox, generation cancellation and GPU display lifecycle wiring are newly
written here. No original LIVI input or transport code is imported.

Receiver/display configuration and authored-canvas mapping were written in this
independent repository. Canvas mapping shallow-copies a scene root and applies
an explicit configured transform; it neither edits nor redistributes the signed
asset. The pixel tests use original synthetic squares/edge markers. No original
LIVI configuration or rendering implementation was imported.

The iAP2 route-guidance component, parser, state model, subscription and native
directions UI are original code. Protocol facts were cross-checked against
public independent research and then observed on the user’s iPhone; references
and scope are recorded in [ROUTE_GUIDANCE.md](ROUTE_GUIDANCE.md). No research
repository source or private route capture is included.

## Climate and settings protocol research

The optional vehicle-state session, bounded transport queues, bench model and
tests were authored independently here. Numeric semantic identifiers were
observed in public iOS framework analysis; local SDK inspection supplied some
format and channel facts. Pinned sources and remaining inferences are recorded
in [CLIMATE_RESEARCH.md](CLIMATE_RESEARCH.md). No Apple implementation or
original LIVI source was copied, and the receiver does not link Apple's SDK.

The semantic capability builder, bounded JSON reader, configured model, provider
interface, examples and tests are original project code. Broader Climate/seat
and settings identifiers and typed getters were observed at the pinned iOS 26.1
analysis revision listed in [VEHICLE_CAPABILITIES.md](VEHICLE_CAPABILITIES.md).
Wider integer format tags remain explicitly recorded inferences, guarded by the
experimental configuration switch. No downloaded framework implementation is
stored in this repository.

## iPhone interface and drive-mode audit

The 9 October 2026 [interface audit](IPHONE_PROTOCOL_AUDIT.md) and
[vehicle type inventory](VEHICLE_PROTOCOL_CATALOGUE.json) are independently
authored protocol-fact records. The JSON retains name/type pairs from the same
pinned iOS 26.1 `CAFAccessoryTypes` and `CAFServiceTypes` listings used by the
Climate research, with listing URLs and SHA-256 hashes. Receiver support
categories come from this project's configuration builder; no framework
implementation is copied or linked.

The [drive-mode guide](DRIVE_MODES.md) adds field and outbound-command facts
inspected in the external verified reference asset, identifying exact resource
hashes. Signed source remains outside Git. Retained phone-session inspection
publishes aggregate protocol names, counts and field shapes; no phone identities,
URLs, captures or keys are included. Apple's WWDC architecture and independent
SDK/binary investigation are cited as contextual sources; research-only
interfaces are not claimed as live acceptance.

The broader [gap review](IPHONE_PROTOCOL_GAPS.md) inspects the declarations in
all 112 pinned service-class listings. The catalogue adds 500 declared
characteristic accessor names/getter types, characteristic type literals and
per-listing URLs/hashes, plus available shared-base facts. No implementation is
copied or linked. Inherited fields, action contracts and wire requiredness are
not inferred from a getter. Two unavailable proposed shared-class names are
recorded explicitly. Additional external signed-pack inspection extracts CP API
names and vehicle-path literals only; neither signed source nor phone content
is redistributed. Read-only live-state checks distinguish subsequent V26 from
the first audit's V24 snapshot.

## Camera interface research

The camera state bridge and owner/lease recovery are original project code. The
resource identifiers and button enum/selection facts come from read-only
inspection of the pinned iOS 26.1 CarAccessory framework and CarCamera app
analysis at revision `90aa0cfe59d9682b4265e1354c8b19ec3c7823ab`. The
[camera guide](CAMERA_ULTRA_GUIDE.md#resource-topology-and-provenance) links the
exact primary listings and distinguishes observed facts from unresolved focus,
permissions and applied action/reset semantics. Private fact manifests hash 19
inspected files; no framework/app implementation or phone capture is committed,
linked or redistributed. Generic installed-SDK AirPlay focus calls do not
establish the signed Ultra camera transition.

## OEM widget resource research

The 11 October blank-widget follow-up inspects binary-analysis listings at the
same pinned iOS revision `90aa0cfe59d9682b4265e1354c8b19ec3c7823ab`.
The [tyre consumer listing](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/Applications/TirePressure.app/TirePressure.mm)
identifies vehicle-layout and top-down exterior resources. The
[image-archive control listing](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFGetImageArchiveControl.mm)
identifies a byte-valued `imageArchive` response. Read-only asset utility,
vehicle variant and custom archive listings provide related API facts. No
implementation is copied, and these facts do not establish an archive schema,
complete wire contract or the cause of live blank widgets.

| Inspected listing | SHA-256 |
| --- | --- |
| `TirePressure.mm` | `f9e2d9690518641916b6a2e38e11108be26bad9e0f82b96b1c8f1544ef88017c` |
| `CarAssetUtils.mm` | `9a609e1d1eb8c378d77f672e268e9b15ac8680e3eabcfafacfb5dbfa8d95b91f` |
| `CAFGetImageArchiveControl.mm` | `1e2a22b9ae7b01a36cdac0a7f51e5ed01646fce052c711471dae9c7ed5f89738` |
| `CAFCustomImageArchive.mm` | `053d0f800ae7b8b627d4d0bb012a575bcdb4089a27ebffce9546233bb3608ef7` |
| `CAFVehicleVariant.mm` | `0225615304b6fa5a702eeebdb6c0c58994c7df15587a422863e08d672c97166d` |
| `CAUAssetLibrary.mm` | `8a7aa8500df078a27b28bb6bb2200eb412b10969055d950075f58dc25d212480` |

Further read-only inspection separates existing phone-side vehicle resources
from the optional custom-image control. The
[car resource accessor](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFCar.mm),
VehicleResources and VehicleVariant listings establish accessory/service and
three string characteristic identifiers. The pinned `caraccessoryd` variant
agent reads exterior/interior style traits and a separate layout selector;
the resource manager treats custom image archives as a distinct optional path.
The source selector builder is our original implementation using those bounded
interface facts, not copied framework/daemon code.

| Additional inspected listing | SHA-256 |
| --- | --- |
| `CAFCar.mm` | `55bcd9e4cb9cbd94a2adfe6dae0d27389983c3351e6f8a135d890cbf0be6ff7d` |
| `CarAssetUtils_01.mm` | `55f1577544ff737933c4b923724774994bd35f6565f47cea28a6b92d32866e85` |
| `caraccessoryd.mm` | `4c0f55644dd823f206f250d0832ca64a8d2e84f5e7cca389ca9ab87381ee043e` |
| `caraccessoryd_01.mm` | `51081f10d584f6a3b1c8726adf1b898e826358661fee6b0f64356c3f84e4a47c` |

The current regular pack's [original Apple delivery](ASSET_DISCOVERY.md#official-delivery-provenance)
was read without executing assets. Its 50,870,896-byte outer ZIP has catalogue
SHA-1 `322190b04ae6fb84845f8513e2372f8420830237` and SHA-256
`872c2a33ea16b481da6515f5c19c5ea736a23ed4ae5be97d6994ac2649cdb385`.
Its inner accessory ZIP equals the installed reference. Phone-side
`Extras/manifest.json` SHA-256 is
`adb5810b82a3a63e73b69256ca6e32e7441a86161c60524bba099d590c8ac6ed`;
the DBX layout is
`871cc3fcd268a510677328697b6b79e98b763c014614378a6d51c59428a9fa67`.
Only resource selector/layout-key facts enter our bench example. Manufacturer
artwork, manifests, framework/daemon listings, signed bundles and phone/output
captures remain private, outside Git. This does not prove live widget rendering.
[Vehicle views](VEHICLE_VIEWS.md#phone-side-vehicle-resources--11-october) owns
the findings and remaining acceptance.

## Dashboard indicator artwork

The 24 light/warning symbols in `src/indicator_symbols.cpp` are our original
Cairo paths, consolidated from this project's warning overlay on 8 October
2026 for reuse by the native cluster. Position, fog-light and hazard-triangle
geometry were refined locally. Conventional meanings were cross-checked against public
[Ford owner documentation](https://www.fordservicecontent.com/Ford_Content/vdirsnet/OwnerManual/Home/Content?ProcUid=G2103156&Uid=G2103154&buildtype=web&countryCode=USA&div=f&languageCode=en&moidRef=G539493&userMarket=USA&vFilteringEnabled=True&variantid=9834)
and [Volvo indicator documentation](https://www.volvocars.com/en-ca/support/car/xc90/16w46/article/e3f280365b62ae60c0a801515331476a/).
No manufacturer icon files, stock icon library or original LIVI artwork was
imported. Cairo's existing text API draws the intrinsic brake markings (`!`,
`P` and `ABS`); no new font asset is distributed.

## Linux Head Unit startup artwork

The three polygons in `assets/branding/headunit/logo.svg` are
original project artwork. The same geometry is embedded in the GTK startup
animation, the native greeting component and the optional manufacturer-slot
decoration. The signed scene and its underlying assets are external.

The OEM Home icon in `assets/oem/home-icon.png` is an original monochrome
house graphic. `src/oem_home_icon.inc` embeds its PNG bytes for the receiver.

## CarPlay Ultra bench screenshots

[Main display](images/ultra-main.png) shows the iPhone app launcher during V47
Climate qualification on 10 October 2026. [Instrument cluster](images/ultra-cluster.png)
shows the signed DigiRing gauges with simulated vehicle inputs during V38
bench qualification on 9 October 2026. Both are screenshots of the
CM5 displays from live CarPlay Ultra sessions using the `44C9FA31EC / 985`
reference package. The PNGs document those two sessions. The signed package
remains an external runtime input.

## Hardware boundary

Board and harness specifications and MCU firmware are maintained separately.
This tree covers the Linux software and the software-facing vehicle contracts.

## Resource observation and media permission

The 9 October protocol-gaps pass is independently authored in this repository.
Apple's [WWDC16 resource-manager description](https://developer.apple.com/videos/play/wwdc2016/723/)
supplies current/permanent ownership and independent alternate-audio semantics.
Resource/app/entity numbers are cross-checked against the independent author's
[SDK/binary fact inventory at `fbbfa594`](https://github.com/lvalen91/carlink_linux/blob/fbbfa59400dac4704f34a5d76e745080ce7d6338/docs/CARPLAY_CAPABILITIES.md#8-modes--resources-arbitration-sdk)
and numeric-only observations from the retained private command archive. No
SDK, xcertplay, carlink, signed-script or original LIVI implementation is copied,
linked or redistributed. Phone transition timing and physical use remain
unqualified. [AirPlay control](AIRPLAY_CONTROL.md) owns the bounded implementation
and evidence; no optional capability is added by this permission gate.


## Voice and microphone transport — 11 October 2026

The PCM/Opus decoder, explicit ALSA/Pulse capture adapter, encrypted microphone
sender, shared playback owner and synthetic fixtures are original project code.
They use the installed GStreamer plugins according to their primary contracts:
[Opus encoding](https://gstreamer.freedesktop.org/documentation/opus/opusenc.html),
[Opus RTP decoding](https://gstreamer.freedesktop.org/documentation/rtp/rtpopusdepay.html)
and [ALSA capture](https://gstreamer.freedesktop.org/documentation/alsa/alsasrc.html).
The earlier independent [format inventory at `6c343d36`](https://github.com/openairplay/airplay2-receiver/blob/6c343d3679ddb561c61566985acaaf587d0a3bd3/ap2/connections/audio.py#L294)
provided the initial bit-30 Opus lead.

Numeric interface facts were then checked against the locally installed CarPlaySDK
binary SHA-256 `ecde230aeabc015c90bcf15513023839b60db2649dd4107dffcac00503fe8912`
and retained first-party annotated disassembly SHA-256
`bf9db6455aeea7b0ba6f7c4a53397dab4f061c6dc13d46485d7bb122dfb7b152`.
`AirPlayAudioFormatToASBD` was called only in a private inspection process to
verify PCM rate/channel masks and Opus mono 16/24/48 kHz, 20 ms frame sizes.
Session SETUP analysis at `0x8f84` and main/alternate setup at `0x11484`
establish request `dataPort` as the optional microphone destination. Sender
analysis at `0xf3fc` and `0xf49c` establishes payload type = stream type,
SSRC zero, separately keyed input encryption and an appended tag/64-bit nonce.
For the current wireless phone generation the existing eight-byte timestamp/SSRC
AAD contract is retained. Only bounded numeric/framing facts are used; no SDK
instructions, implementation or proprietary headers are copied or linked into
the receiver. Original cloned LIVI/AirLib implementations were not read or used.
Apple's [wireless audio overview](https://developer.apple.com/videos/play/wwdc2016/722/)
corroborates Opus/AAC-ELD voice versus AAC-LC media. Apple assets, keys, captures,
reference binaries/disassembly and private receipts remain outside Git.

Synthetic capture and successful compilation do not establish physical microphone,
Siri, speaker or call acceptance. [Audio output](AUDIO_OUTPUT.md) owns the scope
and commissioning steps.

The 11 October camera adaptation, original lane/seat/door vector artwork and
bounded vehicle-view producer are newly authored project code. The existing
qualified clock/lease/presentation adapter is reused for explicit local widgets.
Signed pack declarations supply slot identity/geometry and the observed
car-body-status widget URI; no signed artwork, Lua implementation, car photograph
or original LIVI source is copied into these renderers. USB acquisition uses the
existing independent Linux V4L2 driver and declared colour metadata. Private
pack fixtures and hardware receipts remain outside Git.

The 11 October centre-content configuration, private request lease and local scene
bridge are newly authored. Apple's public WWDC24 design (10112) and architecture
(10111) sessions establish the UI/ownership targets. Type, characteristic and unit
facts come from the existing pinned framework-analysis catalogue at
`EthanArbuckle/iPhone18-3_26.1_23B85_Restore`, revision
`90aa0cfe59d9682b4265e1354c8b19ec3c7823ab`. No framework implementation was copied.
The three widget URI strings are facts observed in the externally retained,
unmodified signed reference pack. Neither declarations nor analysis facts prove
phone consumption or official OEM compliance. Private analysis remains outside Git.

The 11 October cluster navigation descriptor uses USB-IF Consumer Menu usage
facts from [HID Usage Tables 1.5](https://www.usb.org/sites/default/files/hut1_5.pdf).
The connection-bound request, press/release scheduler and native controls are
original project code. Actual phone page transitions qualify the vertical route;
they do not establish an official Apple carousel API.

The incoming transition timestamp adapter uses the real phone's integer
`current_ntp_time` and the installed signed runtime's required `pts` field. It
converts the observed 32.32 value to seconds without manufacturing a receive
clock or modifying the signed archive. The audio command parser's `speechMode`
sentinel `-1` is likewise observed in authenticated real-phone `modesChanged`
captures. Only these field/type facts are retained; capture bytes, identities,
signed Lua and framework implementations remain outside Git. Regression fixtures
are independently authored with synthetic clocks and ownership values.

The 13 October video investigation uses read-only interface/diagnostic facts
from privately cached phone firmware to distinguish local encoder-reference
acknowledgements from receiver presentation feedback. Raw firmware, disassembly
and phone logs remain outside Git. The worker pacing repair, regression and
synthetic benchmark are independently authored C++; no Apple or LIVI
implementation is copied or linked. See [performance](ULTRA_PERFORMANCE.md#phone-frame-timeouts-and-shared-owner-pacing--13-october-2026).
