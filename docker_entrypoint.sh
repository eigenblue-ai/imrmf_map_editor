#!/usr/bin/env bash
set -e

CACHE_ROOT="${IMRMF_CACHE_ROOT:-/var/imrmf/cache}"
PORT="${IMRMF_PORT:-30010}"
WASM_DIR="${IMRMF_WASM_DIR:-/opt/imrmf/www}"
EDITOR_BIN="${IMRMF_EDITOR_BIN:-/opt/imrmf/imrmf_map_editor}"

mkdir -p "$CACHE_ROOT"

RUNFILES="${EDITOR_BIN}.runfiles"
BMS_BIN="$(find "$RUNFILES" -maxdepth 5 -name building_map_server_bin \( -type f -o -type l \) -print -quit 2>/dev/null || true)"
if [ -z "${BMS_BIN}" ]; then
  echo "warning: building_map_server_bin not found under $RUNFILES. ROS2 map service won't run." >&2
fi

cleanup() {
  trap - INT TERM
  if [ -n "${EDITOR_PID:-}" ] && kill -0 "$EDITOR_PID" 2>/dev/null; then
    kill "$EDITOR_PID" 2>/dev/null || true
  fi
  if [ -n "${BMS_PID:-}" ] && kill -0 "$BMS_PID" 2>/dev/null; then
    kill "$BMS_PID" 2>/dev/null || true
  fi
}
trap cleanup INT TERM

echo "starting imrmf_map_editor on :$PORT (cache: $CACHE_ROOT)"
"$EDITOR_BIN" \
  --port "$PORT" \
  --wasm-dir "$WASM_DIR" \
  --cache-root "$CACHE_ROOT" \
  &
EDITOR_PID=$!

YAML_PATH="${RMF_BUILDING_MAP:-}"
if [ -z "$YAML_PATH" ] && [ -n "${IMRMF_AUTO_BUILDING:-}" ]; then
  YAML_PATH="$CACHE_ROOT/$IMRMF_AUTO_BUILDING/$IMRMF_AUTO_BUILDING.building.yaml"
fi
if [ -z "$YAML_PATH" ]; then
  YAML_PATH="$(find "$CACHE_ROOT" -maxdepth 3 -name '*.building.yaml' -print -quit 2>/dev/null || true)"
  [ -z "$YAML_PATH" ] && YAML_PATH="$CACHE_ROOT/active.building.yaml"
fi

if [ -n "${BMS_BIN}" ]; then
  echo "starting building_map_server watching $YAML_PATH"
  # rules_ros2 dlopens the rmw layer via a path relative to <runfiles>/_main/.
  (cd "$RUNFILES/_main" && "$BMS_BIN" "$YAML_PATH" --watch --wait-for-file) &
  BMS_PID=$!
  wait -n "$EDITOR_PID" "$BMS_PID"
else
  wait "$EDITOR_PID"
fi
EXIT_CODE=$?
echo "child exited with $EXIT_CODE, shutting down"
cleanup
exit "$EXIT_CODE"
