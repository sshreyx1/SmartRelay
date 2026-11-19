"""flash_node.py UF2 [--port COMx] [--secs N] [--keys "5:s,20:e"] [--no-flash] [--bootsel-key]

Flash a node and watch its console (docs/SMP_HANDOFF.md section 4).

Waits for an RPI-RP2 drive, copies UF2, reopens the node's serial port, captures for N seconds,
and sends console keys at the given second offsets ("5:s,20:e" = 's' at 5 s, 'e' at 20 s).
--bootsel-key first sends 'B' to --port, which makes a node running the SMP firmware reboot
itself into BOOTSEL (no button). A node running the FreeRTOS firmware is rebooted into BOOTSEL by
opening its port at 1200 baud instead (see the handoff doc). --no-flash: only watch the console.
Drive: the RPI-RP2 drive letter on Windows, /Volumes/RPI-RP2 on macOS.
"""
import argparse
import ctypes
import os
import shutil
import string
import sys
import time

import serial
from serial.tools import list_ports


MAC_VOLUME = "/Volumes/RPI-RP2"


def mac_drive_ready(timeout=3.0):
    """macOS: the volume's folder shows up before Finder has finished mounting it, and a copy
    started then fails with 'Permission denied' (seen 2026-11-11). Wait up to `timeout` seconds
    for INFO_UF2.TXT to be readable and the volume to be writable."""
    end = time.time() + timeout
    while True:
        if os.path.isfile(os.path.join(MAC_VOLUME, "INFO_UF2.TXT")) and os.access(MAC_VOLUME, os.W_OK):
            return True
        if time.time() >= end:
            return False
        time.sleep(0.1)


def find_drive():
    if sys.platform == "darwin":
        if os.path.isdir(MAC_VOLUME) and mac_drive_ready():
            return MAC_VOLUME
        return None
    for letter in string.ascii_uppercase:
        root = f"{letter}:\\"
        if os.path.exists(root):
            buf = ctypes.create_unicode_buffer(261)
            ctypes.windll.kernel32.GetVolumeInformationW(root, buf, 261, None, None, None, None, 0)
            if buf.value == "RPI-RP2":
                return root
    return None


def copy_uf2(uf2, drive):
    if sys.platform != "darwin":
        shutil.copy(uf2, drive)
        return
    # macOS: data only (like `cp -X`: no ._ AppleDouble file, no chmod on the FAT volume, which
    # vanishes when the board reboots at the end of the copy). One retry for a late mount.
    dst = os.path.join(drive, os.path.basename(uf2))
    try:
        shutil.copyfile(uf2, dst)
    except PermissionError:
        time.sleep(1.0)
        shutil.copyfile(uf2, dst)


def pico_ports():
    return sorted(p.device for p in list_ports.comports() if p.vid == 0x2E8A)


ap = argparse.ArgumentParser()
ap.add_argument("uf2", nargs="?")
ap.add_argument("--port")
ap.add_argument("--secs", type=float, default=60)
ap.add_argument("--keys", default="")
ap.add_argument("--no-flash", action="store_true")
ap.add_argument("--bootsel-key", action="store_true")
ap.add_argument("--wait", type=float, default=240)
a = ap.parse_args()

if not a.no_flash:
    if a.bootsel_key and a.port:
        try:
            with serial.Serial(a.port, 115200, timeout=0.2) as s:
                s.write(b"B")
                time.sleep(0.5)
        except Exception as e:
            print("could not send B:", e)
    print("waiting for RPI-RP2 drive...", flush=True)
    end = time.time() + a.wait
    drive = None
    while time.time() < end and not drive:
        drive = find_drive()
        if not drive:
            time.sleep(0.2)
    if not drive:
        print("TIMEOUT: no RPI-RP2 drive")
        sys.exit(1)
    before = set(pico_ports())
    print(f"found {drive}, copying {a.uf2}", flush=True)
    copy_uf2(a.uf2, drive)

port = a.port
ser = None
end = time.time() + 20
time.sleep(1.0)
while time.time() < end and ser is None:
    ports = pico_ports()
    cand = port if port in ports else (ports[0] if ports and not port else None)
    if cand:
        try:
            ser = serial.Serial(cand, 115200, timeout=0.1)
            ser.dtr = True
            port = cand
        except Exception:
            ser = None
    if ser is None:
        time.sleep(0.2)
if ser is None:
    print("TIMEOUT: could not open port", port, pico_ports())
    sys.exit(1)
print(f"opened {port}", flush=True)

keys = []
for item in filter(None, a.keys.split(",")):
    t, k = item.split(":", 1)
    keys.append((float(t), k))
t0 = time.time()
total = 0
while time.time() - t0 < a.secs:
    while keys and time.time() - t0 >= keys[0][0]:
        ser.write(keys.pop(0)[1].encode())
    chunk = ser.read(4096)
    if chunk:
        total += len(chunk)
        # raw bytes: a Windows console's cp1252 cannot encode the replacement character
        sys.stdout.buffer.write(chunk.replace(b"\r\n", b"\n"))
        sys.stdout.flush()
ser.close()
print(f"\n=== captured {total} bytes from {port} ===")
