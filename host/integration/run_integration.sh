#!/usr/bin/env bash
# Two processes = two Picos. Mridula's real lora_mesh.c on both, fake radio over UDP between them.
#   host/integration/run_integration.sh [loss]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$HERE/../.."; LM="$ROOT/third_party/lora_mesh"
OUT="${TMPDIR:-/tmp}/alfp_integ"; rm -rf "$OUT"; mkdir -p "$OUT/node1" "$OUT/node2/photos"
gcc -O1 -g -fsanitize=address,undefined -Wall -I "$HERE" -I "$LM/lora" -I "$ROOT/mesh" -I "$ROOT/alfp/include" \
    -I "$ROOT/common" -I "$ROOT/host/sim" \
    "$HERE/integ_node.c" "$HERE/fake_lora_radio.c" "$LM/lora/lora_mesh.c" "$ROOT/host/sim/storage_posix.c" \
    "$ROOT"/alfp/src/*.c -lm -o "$OUT/integ_node"
# a real JPEG-sized test image (random bytes so every byte matters)
head -c ${SIZE:-5000} /dev/urandom > "$OUT/node2/photos/test.jpg"
export LORA_LOSS="${1:-0}"
"$OUT/integ_node" 1 "$OUT/node1" > "$OUT/rx.log" 2>&1 & RX=$!
"$OUT/integ_node" 2 "$OUT/node2" photos/test.jpg > "$OUT/tx.log" 2>&1 || true
wait $RX || true
cat "$OUT/tx.log" "$OUT/rx.log"
F=$(grep RX_FILE "$OUT/rx.log" | cut -d= -f2)
if [ -n "$F" ] && cmp -s "$OUT/node2/photos/test.jpg" "$OUT/node1/$F"; then
  echo "INTEGRATION PASS: received file is byte-for-byte identical (loss ${LORA_LOSS})"
else
  echo "INTEGRATION FAIL"; exit 1
fi
