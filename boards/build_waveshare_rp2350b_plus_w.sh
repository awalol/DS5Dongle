#!/usr/bin/env bash
# Convenience build script for the Waveshare RP2350B-Plus-W target.
#
# Requires:
#   - ARM toolchain (arm-none-eabi-gcc)
#   - Ninja
#   - A pico-sdk checkout pinned to 2.2.0 with TinyUSB checked out to 0.20.0.
#     If you don't already have one, see the README for setup. By default this
#     script uses PICO_SDK_PATH from the environment.
#
# Usage:
#   ./boards/build_waveshare_rp2350b_plus_w.sh [Release|Debug]   # default: Release

set -euo pipefail

BUILD_TYPE="${1:-Release}"
BUILD_DIR="build/waveshare"
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [[ -z "${PICO_SDK_PATH:-}" ]]; then
    echo "PICO_SDK_PATH is not set. Set it to a pico-sdk checkout pinned to 2.2.0+TinyUSB 0.20.0." >&2
    exit 1
fi

cd "${PROJECT_ROOT}"
cmake -S . -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DWAVESHARE_RP2350B_PLUS_W_BUILD=ON \
    -DPICO_SDK_PATH="${PICO_SDK_PATH}"
cmake --build "${BUILD_DIR}" --target ds5-bridge

echo
echo "Build complete. UF2 at:"
echo "  ${PROJECT_ROOT}/${BUILD_DIR}/ds5-bridge.uf2"
