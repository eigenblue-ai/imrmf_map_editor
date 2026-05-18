#!/usr/bin/env bash
set -euo pipefail

if [ -z "${BUILD_WORKSPACE_DIRECTORY:-}" ]; then
  echo "dev.sh must be invoked via 'bazel run //:dev'" >&2
  exit 1
fi
cd "$BUILD_WORKSPACE_DIRECTORY"

echo "[dev] rebuilding wasm bundle..."
bazel build --config=wasm //app:editor_wasm >/dev/null

WASM_DIR="$BUILD_WORKSPACE_DIRECTORY/bazel-bin/app/editor_wasm"

echo "[dev] starting server..."
exec ./bazel-bin/server_rust/imrmf_map_editor \
  --wasm-dir "$WASM_DIR" \
  "$@"
