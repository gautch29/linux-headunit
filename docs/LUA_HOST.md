<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Independent theme script hosts

The integrated Ultra worker uses our fuller native runtime. See
[architecture](ARCHITECTURE.md) and [Ultra integration](CARPLAY_ULTRA_GUIDE.md)
for current operation; [theme migration](THEME_MIGRATION.md) records its origin.
The smaller `headunit-lua-host` below remains a diagnostic bridge, not the
production integration target.

`headunit-lua-host` executes verified theme **text source** using upstream Luau
0.739. The reference theme uses Luau syntax (`continue`), so Lua 5.4 is not a
compatible interpreter. The host compiles source with its matching compiler;
external `.luac` files are not executed.

The **small diagnostic host** is an early CP interface subset, not the complete
Ultra renderer. It does
not advertise `CP.version`, foreground readiness, or an Ultra service to the
phone. Existing main/navigation video playback is separate and unchanged.

## Implemented

- `require`: only the supplied verified bundle reader, with canonical module
  caching, nested dependencies and cycle rejection; no native loaders or
  arbitrary filesystem access.
- `CP.createAssetRAW`: an immutable binary userdata distinct from Lua strings.
- `CP.status`, `receiveMessage`, `sendMessage`, `reset`: bounded host queues.
- `CP.getSystemUpTime`: an explicitly supplied monotonic clock.
- `CP.enumerateOutputStreams`: copies of the host's explicit display
  descriptors (identifier, width, height, frame duration). A headless host
  truthfully returns an empty list; this does not create a drawing surface.
- `CP.createPlist` / `parsePlist`: bounded binary property-list conversion,
  preserving nested binary data and empty arrays. Invalid incoming plists
  return `nil, "invalid plist"`; unsupported outbound values raise an error.
- `CP.DPLuaTask.create`, `run`, `stop`, `isBusy`: cooperative tasks, once per
  host tick, with completion, self-stop and self-replacement handling.
- Bounded logging and log-level access. Unknown CP calls fail explicitly.

The host links the upstream VM/compiler, not LIVI code. The private reference
package remains external data; the probe loads its connection/task modules
without copying their source into this repository.

## Drawing enum boundary

The full graphics binding keeps script enum values separate from renderer
enums. For the installed reference-pack dialect,
`CP.DPShapeLayer.LINECAP` uses `ROUND=0`, `SQUARE=1`, `BUTT=2`. The setter converts
to native `graphics::Cap`; the getter converts back. A cast between those two
orders incorrectly added square extensions to wide circular gauge tracks.

This compatibility contract is inferred from reference layout geometry and
verified with original pixel tests and private offline renders. It is not an
official Apple specification or proof that every pack uses the same ABI.
Native C++ paths retain their existing butt/round/square order and semantics.

## Resource boundaries

The VM allocator defaults to 64 MiB (configurable 1–256 MiB). Individual source
modules are limited to 1 MiB and raw resources to 16 MiB. Each message queue is
limited to 64 entries / 2 MiB, with a 512 KiB per-message cap. Tasks are limited
to 512 objects. Logs retain at most 128 entries of 4096 bytes.

The VM interrupt callback limits execution safepoints per call/tick. This is
not a wall-clock deadline for compiler or native library operations. Compiler
memory, reader buffers and C++ queues are outside the VM allocator cap. A
future production renderer should run in a separately limited process.

Luau's library/global sandbox is enabled. Filesystem/process libraries, filesystem/native loaders and script-triggered
garbage collection are unavailable. The reused animation support permits
`loadstring` for text only, at most 64 KiB per expression and 1 MiB cumulatively.
Coroutines inherit the host context. Native task destructors only mark work
for cleanup; registry references are released outside garbage collection.

## Probe

```
headunit-lua-probe ASSET_DIRECTORY IDENTIFIER VERSION LOCAL_REFERENCE_OR_DASH
```

The bundle is verified before its source is compiled. The optional reference
file follows the exact pinned, historical-acceptance policy described in
ULTRA_ASSETS.md; it does not enable acceptance of new expired assets.

The probe exercises the package's connection wrapper and task wrapper against
synthetic binary input, yielding/resuming, completion and reset. It then loads
`APL.ClusterControlSession` and exchanges version-2 reset/ready/command/ack
messages with an independently authored synthetic peer. It checks wrong-phone
identity rejection, stale-session rejection and reset after a sequence gap.
This does not
prove the complete theme, layout drawing, live phone UI synchronization or
vehicle data work. Those require additional CP APIs and live integration.

## Verified on the Pi, 2026-09-26

The private reference bundle `44C9FA31EC` version `985` passed the probe using
its previously recorded acceptance pins. Its unmodified
`APL.ClusterControlConnection` and `APL.Thread` source modules executed the
echo, yield/resume, completion and reset checks. The VM reported 927,812 bytes
allocated after that run. This is a narrow compatibility result, not a full
layout/session test.

Validation for this increment: 23/23 tests on the Mac, 31/31 tests on the Pi,
plus the real-package probe above. Synthetic fixtures cover nested/cyclic
imports, Luau-specific syntax, binary preservation, queue saturation, invalid
API errors, loop/memory limits, task self-stop/replacement and yielded values.


## Session bridge, 2026-09-26

The binary property-list codec links system libplist. Before invoking its tree
parser, the input graph is checked for byte ranges, reference validity, cycles,
depth, duplicate text keys and repeated-reference expansion. Limits are 512 KiB
on the wire, 4096 visited nodes (including dictionary keys), depth 32 and 4 MiB
of expanded string/data content. Unicode and binary data remain distinct.

Supported values are booleans, signed 64-bit integers, finite reals, UTF-8
strings without NUL, binary data, arrays and string-keyed dictionaries. Date,
UID, null, set and 128-bit integer objects are explicitly unsupported. Luau
integers are additionally restricted to ±(2^53−1) to prevent silent precision
loss. Outbound Lua tables must be either dense one-based arrays or string-keyed
dictionaries. Empty untagged tables encode as dictionaries; arrays decoded
from plists retain their array identity even when empty.

The verified reference theme completed reset/version/identity negotiation,
its ready callback, inbound command delivery, acknowledgment, and outbound
nested command serialization with our bridge on the Pi. This is an offline
session compatibility test, not a live phone/layout negotiation. No new Ultra
capabilities are advertised by the lab receiver yet.

Those limits describe the smaller diagnostic host. The migrated
`headunit-theme-runtime` supplies our existing drawing, vehicle, foreground and
4 MiB module support; it loads the complete main entry point through the verified
bundle reader. Do not extend the small host to duplicate that implementation.

Session-bridge validation: 24/24 Mac tests, 32/32 Pi tests, plus the extended
real-package probe. The final probe used 1,130,804 VM-allocated bytes. Address
and undefined-behavior sanitizer runs passed for the host and property-list
tests, including truncated/mutated inputs, repeated-reference amplification,
cycles, invalid Unicode, duplicate keys and a Python-generated reference.

## Reused native graphics, 2026-09-26

The user clarified that our earlier Ultra work should be reused. The original
cloned LIVI code remains excluded. The scene and CP graphics implementations
now come from our native research module, with renamed namespaces and host
adaptations recorded in THEME_REUSE.json. Their original pixel/video ownership
tests run against this host. See THEME_MIGRATION.md: the fuller runtime is now migrated; our GPU path and
live receiver integration are next.
