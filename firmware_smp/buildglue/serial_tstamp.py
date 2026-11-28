"""serial_tstamp.py PORT SECS - print each console line from PORT prefixed with its host arrival time
(seconds since start). Run one per node at the same time to line up sender and bridge events
(docs/MICROKERNEL.md)."""
import sys
import time

import serial

port, secs = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=0.05)
s.dtr = True
t0 = time.time()
buf = b""
while time.time() - t0 < secs:
    buf += s.read(4096)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        print(f"{time.time() - t0:8.3f} {line.decode('utf-8', 'replace').rstrip()}", flush=True)
