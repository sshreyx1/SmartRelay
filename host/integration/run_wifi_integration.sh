#!/usr/bin/env bash
# Three processes = three Picos running Goutham's real wifi_mesh.c + ALFP, fake ESP-01 over UDP.
# His topology filter is ON, so E (node 5) cannot hear A (node 1): every packet goes E -> C -> A.
#   host/integration/run_wifi_integration.sh [loss]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$HERE/../.."; WM="$ROOT/third_party/wifi_mesh"
OUT="${TMPDIR:-/tmp}/alfp_wifi_integ"; rm -rf "$OUT"; mkdir -p "$OUT/node1" "$OUT/node3" "$OUT/node5/samples"
gcc -O1 -g -fsanitize=address,undefined -Wall -DUSE_WIFI -I "$HERE" -I "$WM" -I "$ROOT/mesh" -I "$ROOT/alfp/include" \
    -I "$ROOT/common" -I "$ROOT/host/sim" \
    "$HERE/integ_node.c" "$HERE/fake_wst_link.c" "$WM/wifi_mesh.c" "$ROOT/host/sim/storage_posix.c" \
    "$ROOT"/alfp/src/*.c -lm -o "$OUT/integ_node_wifi"
head -c ${SIZE:-20000} /dev/urandom > "$OUT/node5/samples/img_00.jpg"
export WIFI_LOSS="${1:-0}"
"$OUT/integ_node_wifi" 1 "$OUT/node1" > "$OUT/rx.log" 2>&1 & RX=$!
"$OUT/integ_node_wifi" 3 "$OUT/node3" relay > "$OUT/relay.log" 2>&1 & RELAY=$!
sleep 0.5
"$OUT/integ_node_wifi" 5 "$OUT/node5" samples/img_00.jpg > "$OUT/tx.log" 2>&1 || true
wait $RX || true
kill $RELAY 2>/dev/null || true; wait $RELAY 2>/dev/null || true
cat "$OUT/tx.log" "$OUT/rx.log"
F=$(grep RX_FILE "$OUT/rx.log" | cut -d= -f2)
if [ -n "$F" ] && cmp -s "$OUT/node5/samples/img_00.jpg" "$OUT/node1/$F"; then
  echo "WIFI INTEGRATION PASS: E -> C -> A, received file is byte-for-byte identical (loss ${WIFI_LOSS})"
else
  echo "WIFI INTEGRATION FAIL"; exit 1
fi
