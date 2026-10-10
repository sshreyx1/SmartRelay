#!/usr/bin/env python3
"""Checks bridge_receiver.py against a stream produced by host/tests/test_uplink.c."""
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_receiver  # noqa: E402

stream, original = sys.argv[1], sys.argv[2]
out = tempfile.mkdtemp()
rx = bridge_receiver.Receiver(out, echo=False)
with open(stream) as f:
    for line in f:
        rx.feed(line)
assert len(rx.saved) == 1, "expected exactly one saved image"
assert open(rx.saved[0], "rb").read() == open(original, "rb").read(), "image differs from the original"
info = json.load(open(os.path.splitext(rx.saved[0])[0] + ".json"))
cap = info["capture"]
assert cap["capture_id"] == 5 and cap["temperature_c"] == 29.7 and cap["humidity_pct"] == 84.2, cap
assert cap["trigger_pir"] and cap["trigger_sound"] and cap["light_raw"] is None, cap
print("PASS bridge_receiver: image identical, metadata decoded:", json.dumps(cap))
