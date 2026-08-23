#!/bin/sh
# Run the device perf harness N times and aggregate min/median/p95 per metric.
#
#   scripts/device-perf-ab.sh <label> [samples] [host] [binary]
#
#   label    arm name, e.g. hk25 or hk3 — used for the local results file
#   samples  runs of the harness (default 15); first run is discarded as cold
#   host     device ssh host (default 10.0.1.160)
#   binary   local harness path (default .theos/obj/debug/device_smoke_performance)
#
# Output: raw per-sample lines in artifacts/perf-<label>.log plus a summary
# table on stdout. Interleaved A/B procedure: install the 2.5 package, run an
# arm, reinstall the 3.0 package, run an arm, repeat — both arms then share
# thermal/load conditions. Same-package swaps need: dpkg -i --force-downgrade.
set -eu

LABEL="${1:?usage: device-perf-ab.sh <label> [samples] [host] [binary]}"
SAMPLES="${2:-15}"
HOST="${3:-10.0.1.160}"
BINARY="${4:-.theos/obj/device_smoke_performance}"

SSH="sshpass -p alpine ssh -o StrictHostKeyChecking=no -o IdentitiesOnly=yes -o PreferredAuthentications=password -o PubkeyAuthentication=no"
# Unique remote name per invocation: the device can cache a bad exec verdict
# against a reused path (SIGKILL/137 before main runs); a fresh name sidesteps it.
REMOTE_BIN="/var/mobile/hkperf_${LABEL}_$$"
OUT="artifacts/perf-$LABEL.log"
KILLS=0

mkdir -p artifacts
sshpass -p alpine scp -o StrictHostKeyChecking=no -o IdentitiesOnly=yes \
    -o PreferredAuthentications=password -o PubkeyAuthentication=no \
    "$BINARY" "mobile@$HOST:$REMOTE_BIN" >/dev/null
$SSH "mobile@$HOST" "chmod +x $REMOTE_BIN"

: > "$OUT"
i=0
while [ "$i" -lt "$SAMPLES" ]; do
    if [ "$i" -eq 0 ]; then
        # cold run: warm caches, discard from aggregation but keep in log
        $SSH "mobile@$HOST" "$REMOTE_BIN || true" >> "$OUT" || true
    else
        $SSH "mobile@$HOST" "$REMOTE_BIN" >> "$OUT" || {
            # 137 = device exec-verdict kill (see REMOTE_BIN note): retry once
            # under a fresh name; count it either way.
            KILLS=$((KILLS + 1))
            NEW_BIN="${REMOTE_BIN}_r$KILLS"
            $SSH "mobile@$HOST" "cp $REMOTE_BIN $NEW_BIN && chmod +x $NEW_BIN"
            $SSH "mobile@$HOST" "$NEW_BIN" >> "$OUT" || true
        }
    fi
    i=$((i + 1))
done
$SSH "mobile@$HOST" "/var/jb/bin/sh -c 'rm -f $REMOTE_BIN ${REMOTE_BIN}_r*'" >/dev/null || true

awk '
    {
        phase = $2
        if ($3 == "PASS" || $3 == "FAIL" || $3 == "SKIP") status[phase] = $3
        for (i = 3; i <= NF; i++) {
            split($i, kv, "=")
            if (kv[1] == "version" || kv[1] == "count" || kv[2] == "") continue
            v = kv[2] + 0
            if (v == kv[2] && kv[2] !~ /[A-Za-z-]/) {
                vals[phase "." kv[1]] = vals[phase "." kv[1]] " " v
                n[phase "." kv[1]]++
            }
        }
    }
    END {
        printf "%-28s %10s %10s %10s %6s\n", "metric", "min", "median", "p95", "n"
        for (k in vals) {
            m = split(vals[k], a, " ")
            # insertion sort: sample counts are tiny
            for (p = 2; p <= m; p++) { x = a[p]; q = p - 1
                while (q > 0 && a[q] > x) { a[q+1] = a[q]; q-- }
                a[q+1] = x }
            min = a[1]; med = a[int((m+1)/2)]
            idx = int((m*95 + 99)/100); if (idx > m) idx = m
            printf "%-28s %10.1f %10.1f %10.1f %6d\n", k, min, med, a[idx], m
        }
        for (s in status) printf "status %-14s %s\n", s, status[s]
    }
' "$OUT"
[ "$KILLS" -gt 0 ] && echo "exec-kills retried: $KILLS" || true
