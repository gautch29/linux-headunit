#!/bin/bash
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

# Release/appliance tooling refuses directories writable by other accounts, as
# it must on the CM5. Hosted runners mount the workspace with foreign owners, so
# build and test from a private copy owned by an unprivileged user.
set -euo pipefail
id builder >/dev/null 2>&1 || useradd -m builder
if [ ! -d /home/builder/src ]; then
  cp -a "$GITHUB_WORKSPACE" /home/builder/src
  chown -R builder:builder /home/builder/src
  chmod -R go-w /home/builder/src
fi
command=$(printf '%q ' "$@")
exec su builder -s /bin/bash -w LSAN_OPTIONS,CTEST_OUTPUT_ON_FAILURE -c "cd /home/builder/src && umask 022 && $command"
