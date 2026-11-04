#!/usr/bin/env bash
# run_size_ladder.sh - CURVEBALL round 3, Task D.3: run the real LoRa and WiFi mesh integration
# tests (run_integration.sh / run_wifi_integration.sh) across the size ladder
# (sd_card/test_payloads/MANIFEST.csv), to prove the real mesh code -- not just the ALFP
# simulator -- handles every rung end to end.
#
#   host/integration/run_size_ladder.sh [loss]
#
# Each size takes real wall-clock time proportional to ALFP's measured LoRa/WiFi pacing
# (docs/results/e6_size.csv): a rough extrapolation from this round's own measurements (2000 B in
# ~5 s, 20000 B in ~46 s over this harness's single-hop LoRa setup) puts the largest rung
# (120832 B) at roughly 4-5 minutes for LoRa alone. Budget time accordingly, or pass --quick to
# skip straight to a small/large pair instead of the full ladder.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
MANIFEST="$ROOT/sd_card/test_payloads/MANIFEST.csv"
LOSS="${1:-0}"

if [ "${2:-}" = "--quick" ] || [ "${1:-}" = "--quick" ]; then
    SIZES="1024 65536"
    LOSS="${LOSS:-0}"
    [ "$LOSS" = "--quick" ] && LOSS=0
else
    if [ ! -f "$MANIFEST" ]; then
        echo "error: $MANIFEST not found - run tools/make_test_payloads.py first" >&2
        exit 1
    fi
    SIZES=$(tail -n +2 "$MANIFEST" | cut -d, -f2)
fi

n=0
fail=0
for size in $SIZES; do
    n=$((n + 1))
    echo "=== size $size B: LoRa ==="
    start=$(date +%s)
    if SIZE="$size" bash "$HERE/run_integration.sh" "$LOSS" > /tmp/ladder_lora_$size.log 2>&1; then
        echo "  PASS ($(($(date +%s) - start)) s) -- log: /tmp/ladder_lora_$size.log"
    else
        echo "  FAIL -- see /tmp/ladder_lora_$size.log"
        fail=$((fail + 1))
    fi

    echo "=== size $size B: WiFi ==="
    start=$(date +%s)
    if SIZE="$size" bash "$HERE/run_wifi_integration.sh" "$LOSS" > /tmp/ladder_wifi_$size.log 2>&1; then
        echo "  PASS ($(($(date +%s) - start)) s) -- log: /tmp/ladder_wifi_$size.log"
    else
        echo "  FAIL -- see /tmp/ladder_wifi_$size.log"
        fail=$((fail + 1))
    fi
done

echo
echo "$n size(s) x 2 radios = $((n * 2)) runs, $fail failed"
[ "$fail" -eq 0 ]
