#!/bin/sh
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

# Build the pinned Skia exactly as cmake/Skia.cmake expects:
#   $1/src  Skia source tree at the pinned revision (headers)
#   $1/out  libskia.a, args.gn and source-receipt.json
set -eu
prefix=${1:?usage: build-skia.sh PREFIX}
root=$(cd "$(dirname "$0")/../.." && pwd)
revision=$(sed -n 's/^set(_skia_revision "\([0-9a-f]*\)")/\1/p' "$root/cmake/Skia.cmake")
mkdir -p "$prefix"
git init -q "$prefix/src"
cd "$prefix/src"
git fetch -q --depth 1 https://skia.googlesource.com/skia.git "$revision"
git checkout -q FETCH_HEAD
python3 bin/fetch-gn
# Every optional third-party library is disabled in skia-pi.gnargs, so only
# the build tooling is needed; skip the multi-gigabyte dependency sync.
mkdir -p "$prefix/out"
cp "$root/cmake/skia-pi.gnargs" "$prefix/out/args.gn"
case "$(uname -m)" in
  aarch64|arm64) ;;
  *) sed -i 's/^target_cpu = .*/target_cpu = "x64"/' "$prefix/out/args.gn" ;;
esac
bin/gn gen "$prefix/out"
ninja -C "$prefix/out" skia
args_sha=$(sha256sum "$prefix/out/args.gn" | cut -d' ' -f1)
semantic_sha=$(sha256sum "$root/cmake/skia-pi.gnargs" | cut -d' ' -f1)
printf '{"revision":"%s","argsSha256":"%s","semanticArgsSha256":"%s"}\n' \
  "$revision" "$args_sha" "$semantic_sha" > "$prefix/out/source-receipt.json"
rm -rf "$prefix/src/.git"
