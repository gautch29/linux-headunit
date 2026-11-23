#!/bin/sh
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

# Format (or with --check, verify) all first-party C++ with the pinned
# clang-format 19 (`pipx install clang-format==19.1.7`).
set -eu
cd "$(dirname "$0")/.."
files=$(git ls-files '*.cpp' '*.hpp' '*.h' | grep -v '^protocols/')
if [ "${1-}" = "--check" ]; then
  exec clang-format --dry-run --Werror $files
fi
exec clang-format -i $files
