<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Repository instructions

Read [README](README.md), [HANDOFF](docs/HANDOFF.md), [GOAL](docs/GOAL.md) and
[ARCHITECTURE](docs/ARCHITECTURE.md) before implementation or deployment. Use
the [documentation index](docs/README.md) to find the applicable contract and
qualification guide. Check Git and inspect the target's actual state before
deploying.

This repository owns Linux software, provider contracts, tests and appliance
deployment. Hardware design and MCU firmware are maintained separately. Coordinate
explicit interface changes across both sides.

- Preserve [source provenance](docs/PROVENANCE.md) and third-party notices.
  The original LIVI implementation is outside this source tree. Keep identity,
  credential, signed-asset and capture material outside Git.
- Preserve main/touch, cluster, fallback and Climate/settings paths. In
  connected Ultra, camera Punch-through keeps both panels, both streams,
  UI-Sync, audio and control active.
- Keep parsing and encoding bounded and keep VM and GPU work off the control
  thread. Vehicle and camera values carry original acquisition dates, freshness
  and provenance. Treat missing data as unavailable and require confirmation
  before reporting applied actuator state.
- Keep one display/radio owner. Publish a new generation from validated inputs
  and leave sealed releases immutable.
- Use clang-format 19.1.7 via `tools/format.sh` for C++. Use local qualification
  while hosted CI is paused. Report portable, Linux/GPU, simulated and
  real-device results separately. Exercise failure and retirement paths.
- README owns feature status, GOAL owns acceptance and ROADMAP owns planned
  work. Topic guides own contracts and scoped qualification.
