#!/usr/bin/env bash
#
# Stage the prebuilt Zerobus SDK FFI library + header into a prefix that the
# Fluent Bit build can link against:
#
#   scripts/fetch-zerobus-ffi.sh [PREFIX]
#   cmake -B build -DFLB_OUT_ZEROBUS=On -DZEROBUS_FFI_PREFIX="$PREFIX" .
#
# This is a developer/CI convenience only — the build itself never downloads
# anything (so it stays hermetic and offline-friendly for upstream packaging).
# It downloads the FFI release tarball, verifies its SHA-256, and lays out the
# host platform's library + header as <PREFIX>/lib and <PREFIX>/include.
#
# Override the release with ZEROBUS_FFI_VERSION / ZEROBUS_FFI_SHA256.
set -euo pipefail

ZEROBUS_FFI_VERSION="${ZEROBUS_FFI_VERSION:-1.3.0}"
# SHA-256 of zerobus-ffi-${ZEROBUS_FFI_VERSION}.tar.gz. Update when bumping.
ZEROBUS_FFI_SHA256="${ZEROBUS_FFI_SHA256:-7c83904899178f176952b726fb26ff2d5e1ecdddca1bc5e86a385f3923546092}"

PREFIX="${1:-${PWD}/zerobus-ffi-prefix}"
URL="https://github.com/databricks/zerobus-sdk/releases/download/ffi/v${ZEROBUS_FFI_VERSION}/zerobus-ffi-${ZEROBUS_FFI_VERSION}.tar.gz"

# Map the host architecture onto the tarball's per-platform subdirectory.
case "$(uname -m)" in
  x86_64|amd64)  platform="linux-x86_64" ;;
  aarch64|arm64) platform="linux-aarch64" ;;
  *) echo "error: no prebuilt Zerobus SDK FFI for arch '$(uname -m)'" >&2; exit 1 ;;
esac

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

tarball="${workdir}/zerobus-ffi.tar.gz"
echo "Downloading ${URL}"
curl -fSL -o "$tarball" "$URL"

echo "Verifying SHA-256"
echo "${ZEROBUS_FFI_SHA256}  ${tarball}" | sha256sum -c -

tar xzf "$tarball" -C "$workdir"

mkdir -p "${PREFIX}/lib" "${PREFIX}/include"
cp "${workdir}/${platform}/zerobus.h" "${PREFIX}/include/"
# Copy both the static and shared libraries; find_library prefers the shared one.
cp "${workdir}/${platform}/libzerobus_ffi."* "${PREFIX}/lib/"

echo "Staged Zerobus SDK FFI ${ZEROBUS_FFI_VERSION} (${platform}) into ${PREFIX}"
echo "Configure with: -DFLB_OUT_ZEROBUS=On -DZEROBUS_FFI_PREFIX=${PREFIX}"
