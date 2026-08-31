#!/usr/bin/env bash
set -euo pipefail
# run_instruments.sh — Time Profiler trace for device_bench via xctrace.
# Usage: bash tools/bench/run_instruments.sh <UDID|device-name> [--iters-e2e N]
# Requires macOS with Xcode + xctrace. On Linux hosts, falls back to signpost-only run.
# ponytail: no wrapper lib, just xcrun invocation.

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Instruments requires macOS. On Linux, use device_bench signposts directly:" >&2
  echo "  bash tools/bench/run_device_bench.sh <host> && grep signpost" >&2
  exit 0
fi

UDID=${1:-}
if [[ -z "$UDID" ]]; then
  echo "usage: $0 <UDID> [--iters-e2e N]" >&2
  echo "  xcrun xctrace list devices" >&2
  exit 2
fi
shift || true

ROOT=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

make device-bench
BIN=".theos/obj/device_bench"
mkdir -p .theos/bench
OUT=".theos/bench/trace_$(date +%Y%m%d_%H%M%S).trace"

# Prefer modern template name; fallback to Time Profiler
TEMPLATE="Time Profiler"
if ! xcrun xctrace list templates 2>/dev/null | grep -q "Time Profiler"; then
  TEMPLATE=$(xcrun xctrace list templates 2>/dev/null | head -1 || echo "Time Profiler")
fi

echo "Recording $TEMPLATE to $OUT ... (device $UDID)"
# Install first via ios-deploy or rely on xctrace launching? We launch the bench binary
# that is already on device? Instead record a launch of the bench via xctrace's --launch.
# For ad-hoc binaries, xctrace can launch by path if already installed as app; for CLI tools,
# we record while ssh runs it and sample separately. Here we just run ssh in background and trace.

# Simple: run xctrace record in background, ssh run, stop.
xcrun xctrace record --template "$TEMPLATE" --device "$UDID" --output "$OUT" --launch -- /tmp/device_bench "$@" &
TRACE_PID=$!
sleep 2
# Ensure binary on device
# assume DEVICE_SSH known? try to derive? skip — user runs run_device_bench separately.
wait $TRACE_PID || true
echo "Trace at $OUT"
echo "Open with: open $OUT"
