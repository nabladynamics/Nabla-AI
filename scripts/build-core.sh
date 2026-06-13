#!/usr/bin/env bash
# Configure and build the C++ solver core. Defaults to a Release build.
#
# Usage: scripts/build-core.sh [BUILD_TYPE]   (default: Release)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="${1:-Release}"
BUILD_DIR="${ROOT}/core/build"

echo "[build-core] configuring (${BUILD_TYPE}) …"
cmake -S "${ROOT}/core" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DNABLA_BUILD_TESTS=OFF

echo "[build-core] building …"
cmake --build "${BUILD_DIR}" -j

echo "[build-core] done → ${BUILD_DIR}/nabla_solve"
"${BUILD_DIR}/nabla_solve" --version
