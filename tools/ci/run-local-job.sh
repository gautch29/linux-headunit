#!/bin/bash
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

# Internal job executor. Keep flags aligned with .github/workflows/ci.yml.
set -euo pipefail
umask 022
job=${1:?job required}
source=${2:?source snapshot required}
output=${3:?output directory required}
jobs=${4:?parallelism required}
export CTEST_OUTPUT_ON_FAILURE=1
export LSAN_OPTIONS=suppressions=/home/builder/src/tools/ci/lsan.supp

if [ "$source" = /workspace ]; then
  # Root installs dependencies/Skia; all project configure/build/tests use the
  # exact hosted helper's private, unprivileged, non-group-writable copy.
  cd /workspace
  case "$job" in
    linux-gcc|receiver-gcc) cc=gcc; cxx=g++; flags='' ;;
    linux-sanitizers)
      cc=clang-19; cxx=clang++-19
      flags='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined' ;;
    receiver-asan)
      cc=clang-19; cxx=clang++-19
      flags='-fsanitize=address -fno-omit-frame-pointer' ;;
    *) exit 2 ;;
  esac
  type=Debug
  extra=(-DHEADUNIT_BUILD_THEME_VIDEO=ON)
  case "$job" in
    receiver-*)
      type=RelWithDebInfo
      flock -x /opt/skia/.build.lock bash -c '
        if [ ! -f /opt/skia/out/source-receipt.json ]; then
          tools/ci/build-skia.sh /opt/skia
        fi'
      extra+=(-DHEADUNIT_BUILD_THEME_GPU=ON -DHEADUNIT_THEME_SKIA_SOURCE=/opt/skia/src
              -DHEADUNIT_THEME_SKIA_BUILD=/opt/skia/out) ;;
  esac
  set -x
  cat /etc/os-release
  "$cc" --version
  tools/ci/run-as-builder.sh cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE="$type" \
    -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
    -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags" "${extra[@]}"
  tools/ci/run-as-builder.sh cmake --build build -j"$jobs"
  tests=(--test-dir build -j"$jobs" --output-on-failure)
  case "$job" in receiver-*) tests+=(-LE hardware) ;; esac
  tools/ci/run-as-builder.sh ctest "${tests[@]}"
  exit 0
fi

cd "$source"
case "$job" in
  format)
    # A newer formatter can disagree with the hosted pinned version.
    test "$(clang-format --version)" = 'clang-format version 19.1.7'
    set -x
    tools/format.sh --check ;;
  macos)
    test "$(uname -s)" = Darwin
    set -x
    # Dependencies are prerequisites on a shared development Mac. Running
    # brew install here can upgrade libraries underneath other local builds.
    for dependency in ninja pkg-config libplist libarchive cairo libpng openssl@3; do
      brew --prefix "$dependency"
    done
    sw_vers
    cmake -S . -B "$output/macos-build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
      -DCMAKE_PREFIX_PATH="$(brew --prefix libarchive);$(brew --prefix openssl@3)"
    cmake --build "$output/macos-build" -j"$jobs"
    ctest --test-dir "$output/macos-build" -j"$jobs" --repeat until-pass:3 ;;
  linux-*|receiver-*)
    docker info
    deps_key=$(shasum -a 256 tools/ci/install-debian-deps.sh | cut -d' ' -f1)
    image="headunit-local-ci:deps-arm64-$deps_key"
    if ! docker image inspect "$image" >/dev/null 2>&1; then
      docker build --platform linux/arm64 -t "$image" -f - . <<'EOF'
FROM debian:trixie
COPY tools/ci/install-debian-deps.sh /tmp/install-debian-deps.sh
RUN /tmp/install-debian-deps.sh && rm -rf /var/lib/apt/lists/*
EOF
    fi
    docker image inspect "$image" --format '{{.Id}} {{.Architecture}}'
    skia_key=$(cat cmake/Skia.cmake cmake/skia-pi.gnargs tools/ci/build-skia.sh | shasum -a 256 | cut -d' ' -f1)
    volume="headunit-local-ci-skia-arm64-$skia_key"
    mounts=(--mount "type=bind,source=$source,target=/workspace,readonly")
    case "$job" in receiver-*) mounts+=(--mount "type=volume,source=$volume,target=/opt/skia") ;; esac
    container=""
    cleanup() {
      if [ -n "$container" ]; then
        # Retain detailed CTest failure logs before retiring the isolated owner.
        docker cp "$container:/home/builder/src/build/Testing" "$output/$job-Testing" 2>/dev/null || true
        docker rm -f "$container" >/dev/null || true
      fi
    }
    trap cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    container=$(docker create --platform linux/arm64 "${mounts[@]}" \
      -e GITHUB_WORKSPACE=/workspace -e CTEST_OUTPUT_ON_FAILURE -e LSAN_OPTIONS \
      -e TMPDIR=/tmp "$image" bash /workspace/tools/ci/run-local-job.sh "$job" /workspace /unused "$jobs")
    docker start -a "$container"
    # docker start -a itself does not reliably propagate the container's code.
    code=$(docker inspect --format '{{.State.ExitCode}}' "$container")
    exit "$code" ;;
  *) echo "Unknown job: $job" >&2; exit 2 ;;
esac
