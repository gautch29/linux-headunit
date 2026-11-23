#!/bin/sh
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

# Debian trixie packages for hosted CI. Matches the CM5 bench userland
# (Raspberry Pi OS trixie, GCC 14) closely enough for compile and component tests.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential cmake ninja-build pkg-config git ca-certificates curl python3 \
  python3-gi gir1.2-gdkpixbuf-2.0 \
  clang-19 llvm-19 libclang-rt-19-dev \
  libssl-dev libplist-dev libarchive-dev libcairo2-dev libpng-dev \
  libglib2.0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav \
  libwayland-dev wayland-protocols libgbm-dev libegl-dev libgles-dev \
  nodejs ffmpeg
