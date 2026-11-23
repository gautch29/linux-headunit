#!/bin/bash
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

# Local component evidence for every job in .github/workflows/ci.yml.
set -euo pipefail
umask 022
root=$(cd "$(dirname "$0")/../.." && pwd)
jobs=4
selected="format macos linux-gcc linux-sanitizers receiver-gcc receiver-asan"
output=""
usage() {
  cat <<'EOF'
Usage: tools/ci/run-local.sh [--job JOB] [--jobs N] [--output DIRECTORY]

Default: all six CI jobs, sequentially; each build/ctest uses -j4.
Jobs: format, macos, linux-gcc, linux-sanitizers, receiver-gcc, receiver-asan
--job runs one job. --jobs bounds build/test parallelism (default 4).
--output must be a new directory; default build/local-ci/<UTC timestamp>-<PID>.
Requires macOS build dependencies, pinned clang-format 19.1.7 and a running
Docker engine. Linux containers always use Debian trixie linux/arm64.
Exit 0 means all selected jobs passed; any failure exits 1 after the summary.
Only Git-tracked working-tree files (including staged additions) are copied.
See docs/CI.md for prerequisites, cache cleanup and evidence limits.
EOF
}
while [ "$#" -gt 0 ]; do
  case "$1" in
    --help|-h) usage; exit 0 ;;
    --job|--jobs|--output)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      case "$1" in
        --job)
          case "$2" in
            format|macos|linux-gcc|linux-sanitizers|receiver-gcc|receiver-asan) selected=$2 ;;
            *) echo "Unknown job: $2" >&2; exit 2 ;;
          esac ;;
        --jobs) jobs=$2 ;;
        --output) output=$2 ;;
      esac
      shift 2 ;;
    *) usage >&2; exit 2 ;;
  esac
done
case "$jobs" in ''|*[!0-9]*) echo '--jobs must be a positive integer' >&2; exit 2 ;; esac
case "$jobs" in *[1-9]*) ;; *) echo '--jobs must be a positive integer' >&2; exit 2 ;; esac
output=${output:-"$root/build/local-ci/$(date -u +%Y%m%dT%H%M%SZ)-$$"}
mkdir -p "$(dirname "$output")"
mkdir "$output"
output=$(cd "$output" && pwd)
snapshot="$output/source"
# Do not mount private inputs, ignored builds or the worktree's .git pointer.
python3 - "$root" "$snapshot" <<'PY'
import pathlib
import shutil
import subprocess
import sys

root, target = map(pathlib.Path, sys.argv[1:])
target.mkdir()
for raw in subprocess.check_output(["git", "-C", str(root), "ls-files", "-z"]).split(b"\0"):
    if not raw:
        continue
    relative = pathlib.Path(raw.decode())
    source = root / relative
    if not source.exists() and not source.is_symlink():
        continue  # Working-tree deletion.
    if source.is_symlink():
        raise SystemExit(f"Refusing tracked symlink in CI snapshot: {relative}")
    destination = target / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
PY
# format.sh uses git ls-files. This is a new local index, not private Git data.
git -C "$snapshot" init -q
git -C "$snapshot" add --all
summary="$output/summary.md"
{
  echo '# Local CI component results'
  echo
  printf 'Source: `%s` (tracked working-tree snapshot).\n\n' "$(git -C "$root" rev-parse HEAD)"
  printf 'Host: `%s`; Linux platform: `linux/arm64`; parallelism: `%s`.\n\n' "$(uname -sm)" "$jobs"
  printf 'Workflow SHA-256: `%s`.\n\n' "$(shasum -a 256 "$snapshot/.github/workflows/ci.yml" | cut -d' ' -f1)"
  echo 'Component evidence only; no phone, display, GPU hardware or deployment qualification.'
  echo
  echo '| Job | Result | Log |'
  echo '| --- | --- | --- |'
} > "$summary"
git -C "$root" diff HEAD --binary > "$output/tracked-changes.patch"
result=0
for job in $selected; do
  log="$output/$job.log"
  echo "Running $job; log: $log"
  if "$snapshot/tools/ci/run-local-job.sh" "$job" "$snapshot" "$output" "$jobs" > "$log" 2>&1; then
    status=PASS
  else
    code=$?
    status="FAIL (exit $code)"
    result=1
  fi
  printf '| %s | %s | [%s](%s) |\n' "$job" "$status" "$job.log" "$log" >> "$summary"
  echo "$job: $status"
done
echo
cat "$summary"
echo "Summary: $summary"
exit "$result"
