#!/usr/bin/env bash
# Build the pinned external dependencies (aasdk, which bundles aap_protobuf)
# from deps.lock into .deps/install.
#
# Used by plain distro / Raspberry Pi OS builds and by CI. Nix users don't need
# this; flake.nix builds the same pinned revision itself.
#
# Usage:
#   ./scripts/build-deps.sh
#
# Then configure the bridge against the result:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH="$PWD/.deps/install"
#
# Idempotent: skips the build when .deps/install already matches the pinned
# revision and current patches. Delete .deps/ to force a full rebuild.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="$ROOT/.deps"
SRC_DIR="$DEPS_DIR/src/aasdk"
BUILD_DIR="$DEPS_DIR/build/aasdk"
INSTALL_DIR="$DEPS_DIR/install"

# shellcheck source=deps.lock
source "$ROOT/deps.lock"

# Flags shared with the Nix build intent: skip bundled tests and use system
# protobuf/absl instead of rebuilding them.
CMAKE_FLAGS=(
    # aasdk bundles modules with 'cmake_minimum_required(VERSION 3.0.0)',
    # which CMake >= 4 rejects; this restores compatibility. Harmless on
    # older CMake.
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    -DAASDK_TEST=OFF
    -DSKIP_BUILD_PROTOBUF=ON
    -DSKIP_BUILD_ABSL=ON
    -DCMAKE_BUILD_TYPE=Release
)

PATCH_HASH=$(cat "$ROOT"/patches/aasdk/*.patch | sha256sum | cut -d' ' -f1)
# Include toolchain versions in the stamp: protobuf-generated headers and
# C++ objects must match the compiler/protoc that aa2acp itself will use.
TOOLCHAIN=$(protoc --version; cmake --version | head -1; ${CC:-gcc} --version | head -1; ${CXX:-g++} --version | head -1)
STAMP_FILE="$INSTALL_DIR/.built-stamp"
STAMP_EXPECTED="rev=$AASDK_REV patches=$PATCH_HASH toolchain=$(printf '%s\n' "$TOOLCHAIN" | sha256sum | cut -d' ' -f1)"

if [[ -f "$STAMP_FILE" && "$(cat "$STAMP_FILE")" == "$STAMP_EXPECTED" ]]; then
    echo "dependencies up to date ($STAMP_EXPECTED), nothing to do"
    exit 0
fi

echo "building aasdk $AASDK_REV into $INSTALL_DIR"

# Wipe the install prefix too: a new revision may stop shipping files that
# a stale prefix would keep exporting to consumers.
rm -rf "$SRC_DIR" "$BUILD_DIR" "$INSTALL_DIR"
mkdir -p "$SRC_DIR" "$BUILD_DIR" "$INSTALL_DIR"

git clone "$AASDK_URL" "$SRC_DIR"
git -C "$SRC_DIR" checkout --detach "$AASDK_REV"

for patch in "$ROOT"/patches/aasdk/*.patch; do
    echo "applying $(basename "$patch")"
    git -C "$SRC_DIR" apply --whitespace=nowarn "$patch"
done

cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    "${CMAKE_FLAGS[@]}"
cmake --build "$BUILD_DIR" -j"$(nproc)"
cmake --install "$BUILD_DIR"

printf '%s\n' "$STAMP_EXPECTED" > "$STAMP_FILE"
echo "done: configure aa2acp with -DCMAKE_PREFIX_PATH=$INSTALL_DIR"
