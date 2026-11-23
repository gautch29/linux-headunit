<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Development handoff

Start with the [architecture](ARCHITECTURE.md), [acceptance criteria](GOAL.md)
and [roadmap](ROADMAP.md). This source candidate targets the CM5 bench described
in [bring-up](CM5_BRINGUP.md). Each installation supplies its own host, service
account, device paths, credentials, pairing store and external signed inputs.

## Inspect an installation

From the bench, inspect the active units and the selected generation before
building or publishing. This is a read-only check.

```sh
systemctl is-active headunit-compositor headunit-outputs headunit-phone-page \
  headunit-cluster headunit-receiver
systemctl show headunit-receiver --property=FragmentPath,ActiveState,SubState,NRestarts
```

Use your installed generation's verifier and manifest digest to check the payload
and installed units. Keep local identities, source/build logs, release receipts
and captures out of the public tree. The [release workflow](APPLICATION_RELEASES.md)
covers preflight, publication and unit installation. Check free storage and
ensure that one owner controls each display and radio before making changes.

## Qualify new work

Use the portable build for source and protocol checks. Exercise Linux
and GPU behavior on a separate build workspace. A successful build or a
passing replay fixture establishes only the tested component. On the CM5,
measure actual phone recovery, display presentation and touch separately from
local redraws and simulated tests. Record the build inputs, display/profile,
observed state and failure paths for each test run.

The next live integration work is camera Punch-through with continuous main,
cluster, touch, audio and control, followed by cluster preferences, pre-phone
gauges, turn signals and display formats. The bench has no physical vehicle
provider, audible speaker or microphone qualification.
