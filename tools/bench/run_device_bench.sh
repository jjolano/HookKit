#!/usr/bin/env bash
set -euo pipefail
# run_device_bench.sh — build + install + run device_bench on jailbroken device.
# Usage: bash tools/bench/run_device_bench.sh [user@host] [--iters N] [--iters-e2e N]
# Host arg via $DEVICE_SSH or first positional.
# ponytail: no new deps, reuses Makefile device-bench pattern.

DEVICE_SSH=${DEVICE_SSH:-}
EXTRA_ARGS=()
for arg in "$@"; do
  case "$arg" in
    *@*|*.*.*.*) DEVICE_SSH="$arg" ;;
    *) EXTRA_ARGS+=("$arg") ;;
  esac
done
if [[ -z "$DEVICE_SSH" ]]; then
  echo "usage: $0 <user@host> [--iters N] ..." >&2
  echo "  or set DEVICE_SSH env" >&2
  exit 2
fi

ROOT=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

# Build
make device-bench

BIN=".theos/obj/device_bench"
if [[ ! -f "$BIN" ]]; then
  echo "device_bench binary not found at $BIN" >&2
  exit 1
fi

# rm before scp is load-bearing (code sign cache) — see Makefile:539
ssh "$DEVICE_SSH" "rm -f /tmp/device_bench" || true
scp "$BIN" "$DEVICE_SSH:/tmp/device_bench"
# Run — JSON lines + human lines
mkdir -p .theos/bench
ssh "$DEVICE_SSH" "/tmp/device_bench ${EXTRA_ARGS[*]:-}" | tee ".theos/bench/device_bench_$(date +%Y%m%d_%H%M%S).log"
# Also capture pure JSON for baseline
# ssh "$DEVICE_SSH" "/tmp/device_bench ${EXTRA_ARGS[*]:-}" | grep '^{' > .theos/bench/device-baseline-candidate.json || true
# echo "JSON baseline candidate at .theos/bench/device-baseline-candidate.json"
