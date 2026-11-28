#!/usr/bin/env python3
"""con.py PORT SECS [KEYS] [LOG]
Open PORT raw (no pyserial), assert DTR, print timestamped lines for SECS seconds.
KEYS: "0:w,3:s,8:e" -> send key at second offset. LOG: append raw lines to file.
PORT may be 'wait' -> wait up to 60 s for a NEW /dev/cu.usbmodem* and open it immediately
(catches the boot log)."""
import fcntl, glob, os, select, sys, termios, time

def ports():
    return set(glob.glob("/dev/cu.usbmodem*"))

def open_port(p):
    fd = os.open(p, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0
    a[4] = a[5] = termios.B115200
    a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    TIOCMBIS = 0x8004746c; TIOCM_DTR = 0x002
    try:
        fcntl.ioctl(fd, TIOCMBIS, TIOCM_DTR.to_bytes(4, "little"))
    except OSError:
        pass
    return fd

port, secs = sys.argv[1], float(sys.argv[2])
keys = []
if len(sys.argv) > 3 and sys.argv[3]:
    for kv in sys.argv[3].split(","):
        t, k = kv.split(":", 1)
        keys.append((float(t), k))
log = open(sys.argv[4], "a") if len(sys.argv) > 4 else None

if port == "wait":
    before = ports(); end = time.time() + 60
    while time.time() < end:
        new = ports() - before
        if new:
            port = sorted(new)[0]; break
        time.sleep(0.05)
    else:
        print("no new port in 60 s; present:", sorted(ports())); sys.exit(1)
    print("NEW PORT:", port, flush=True)
    for _ in range(40):
        try:
            fd = open_port(port); break
        except OSError:
            time.sleep(0.05)
else:
    fd = open_port(port)

t0 = time.time(); buf = b""
while time.time() - t0 < secs:
    now = time.time() - t0
    while keys and keys[0][0] <= now:
        k = keys.pop(0)[1]
        os.write(fd, k.encode()); print(f"{now:8.3f} >>> sent '{k}'", flush=True)
    r, _, _ = select.select([fd], [], [], 0.05)
    if r:
        try:
            d = os.read(fd, 4096)
        except OSError as e:
            print("port error:", e); break
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode("utf-8", "replace").rstrip()
            if log: log.write(f"{now:8.3f} {s}\n"); log.flush()
            if s.startswith("@D"):
                continue
            print(f"{now:8.3f} {s}", flush=True)
os.close(fd)
