"""boot_flash_capture.py UF2 SECS OUT - flash a board already in BOOTSEL and capture its boot log.

Copies UF2 to /Volumes/RPI-RP2 (macOS; no extended attributes, like `cp -X`), then waits for a
serial port that was not there before the copy, opens it at once and logs every line with its
time since the copy finished (the board reboots at that moment). The header records when the
port appeared and when it opened, so a missing boot log can be explained by timing: the SMP
firmware holds its start-up lines for only 3 s waiting for a host (main_smp.c usb_owner_task).

The port is opened with os.open + termios (TCSANOW), not pyserial: pyserial's open() raises DTR
and then flushes the input buffer, and the SMP firmware sends all its held start-up lines the
moment DTR rises, so pyserial throws exactly those lines away (2026-11-11, STATUS.md).
"""
import glob
import os
import shutil
import sys
import termios
import time

uf2, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
vol = "/Volumes/RPI-RP2"
if not os.path.isdir(vol):
    print("no RPI-RP2 drive: put the board in BOOTSEL first")
    sys.exit(1)
before = set(glob.glob("/dev/cu.usbmodem*"))
t_copy0 = time.time()
shutil.copyfile(uf2, os.path.join(vol, os.path.basename(uf2)))
t0 = time.time()  # board reboots when the last block lands
port = None
while time.time() - t0 < 30 and port is None:
    new = sorted(set(glob.glob("/dev/cu.usbmodem*")) - before)
    port = new[0] if new else None
    if port is None:
        time.sleep(0.01)
if port is None:
    print("no new port within 30 s of the copy")
    sys.exit(1)
t_appear = time.time() - t0
fd = None
while fd is None and time.time() - t0 < 40:
    try:
        fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)  # macOS raises DTR on open
    except OSError:
        time.sleep(0.005)
attrs = termios.tcgetattr(fd)
attrs[0] = 0                                    # iflag: raw
attrs[1] = 0                                    # oflag: raw
attrs[2] |= termios.CLOCAL | termios.CREAD      # keep HUPCL off the list: leave DTR alone
attrs[3] = 0                                    # lflag: no echo, no canonical mode
termios.tcsetattr(fd, termios.TCSANOW, attrs)   # TCSANOW: no flush of what already arrived
t_open = time.time() - t0
rows = [f"[host] copy {os.path.basename(uf2)} took {t0 - t_copy0:.3f} s",
        f"[host] new port {port} appeared {t_appear:.3f} s after the copy, opened at {t_open:.3f} s"]
print("\n".join(rows), flush=True)
buf = b""
while time.time() - t0 < secs:
    try:
        buf += os.read(fd, 4096)
    except BlockingIOError:
        time.sleep(0.005)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        rows.append(f"{time.time() - t0:8.3f} {line.decode('utf-8', 'replace').rstrip()}")
os.close(fd)
with open(out, "w") as f:
    f.write("\n".join(rows) + "\n")
print("\n".join(rows[2:30]))
print(f"wrote {out} ({port})")
