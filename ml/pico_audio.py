#!/usr/bin/env python3
"""
pico_audio.py - talk to audio_dump.uf2: record raw 16 kHz audio from the Pico mic, and ask the Pico
for its own feature vector and classification of that recording.

    from pico_audio import PicoAudio
    with PicoAudio() as pico:                 # auto-detects the Pico (USB VID 0x2E8A)
        x = pico.record(5)                    # np.ndarray[uint16], 80000 samples, CRC-checked
        f = pico.features()                   # np.ndarray[float32], on-device sound_features.c
        c = pico.classify()                   # {"id": 20, "name": "DOG", "group": "MAMMAL", "conf": 82, ...}

Protocol (firmware/audiodump/audiodump_main.c): r<sec> -> @AUD BEGIN / @A <base64> ... / @AUD END crc=..,
s = resend, f -> @FEAT, c -> @CLS, p -> @PONG. Lines may end in \\n or \\r\\n.

Run:      python ml/pico_audio.py --list
          python ml/pico_audio.py --test 2              (records 2 s, prints length, mean, std)
          python ml/pico_audio.py --test 2 --port COM5 --save take.npy
Close any serial monitor first: only one program can hold the port.
"""
import argparse
import base64
import re
import sys
import time
import zlib
from typing import Callable, Iterator, Optional

import numpy as np

PICO_VID = 0x2E8A
RATE = 16000
BAUD = 115200

_BEGIN = re.compile(r"^@AUD BEGIN rate=(\d+) n=(\d+) mean=(\d+)$")
_END = re.compile(r"^@AUD END n=(\d+) crc=([0-9a-fA-F]{8})$")
_KV = re.compile(r"(\w+)=(\S+)")


class PicoAudioError(RuntimeError):
    pass


class AudioBlock:
    def __init__(self, samples: np.ndarray, rate: int, mean: int, crc_ok: bool, crc: int):
        self.samples, self.rate, self.mean, self.crc_ok, self.crc = samples, rate, mean, crc_ok, crc


def find_pico_port() -> str:
    """The serial port of the first Pico (USB VID 0x2E8A). Works for COMx and /dev/cu.usbmodem*."""
    from serial.tools import list_ports
    picos = sorted(p.device for p in list_ports.comports() if p.vid == PICO_VID)
    if not picos:
        raise PicoAudioError("no Pico found (USB VID 0x2E8A). Is it plugged in and running audio_dump.uf2? "
                             "Or pass --port COM5 / --port /dev/cu.usbmodem1101")
    if len(picos) > 1:
        print(f"note: {len(picos)} Picos found ({', '.join(picos)}), using {picos[0]}; pass --port to choose",
              file=sys.stderr)
    return picos[0]


def parse_audio(lines: Iterator[str]) -> AudioBlock:
    """Consume lines up to and including @AUD END. Lines before @AUD BEGIN (logs) are skipped."""
    m = None
    for line in lines:
        m = _BEGIN.match(line.strip())
        if m:
            break
    if not m:
        raise PicoAudioError("no @AUD BEGIN line")
    rate, n, mean = int(m.group(1)), int(m.group(2)), int(m.group(3))
    chunks = []
    for line in lines:
        line = line.strip()
        if line.startswith("@A "):
            try:
                chunks.append(base64.b64decode(line[3:], validate=True))
            except ValueError:
                chunks.append(b"")  # a corrupted line: the CRC check below fails, the caller resends
            continue
        e = _END.match(line)
        if e:
            raw = b"".join(chunks)
            crc = int(e.group(2), 16)
            ok = int(e.group(1)) == n and len(raw) == 2 * n and (zlib.crc32(raw) & 0xFFFFFFFF) == crc
            samples = np.frombuffer(raw[: len(raw) // 2 * 2], dtype="<u2").astype(np.uint16)
            return AudioBlock(samples, rate, mean, ok, crc)
    raise PicoAudioError("recording ended without @AUD END (timeout?)")


def parse_feat(line: str) -> np.ndarray:
    line = line.strip()
    if not line.startswith("@FEAT "):
        raise PicoAudioError(f"not a @FEAT line: {line!r}")
    return np.array([float(v) for v in line[6:].split(",")], dtype=np.float32)


def parse_cls(line: str) -> dict:
    line = line.strip()
    if not line.startswith("@CLS "):
        raise PicoAudioError(f"not a @CLS line: {line!r}")
    out = dict(_KV.findall(line))
    for k in ("id", "conf", "model_us"):
        if k in out:
            out[k] = int(out[k])
    return out


class PicoAudio:
    """One audio_dump Pico. serial_factory is for tests (returns an object with write/readline/close)."""

    def __init__(self, port: Optional[str] = None, timeout: float = 15.0,
                 serial_factory: Optional[Callable] = None):
        self.timeout = timeout
        if serial_factory is None:
            import serial
            self.port = port or find_pico_port()
            self._ser = serial.Serial(self.port, BAUD, timeout=0.5)
            time.sleep(0.2)
            self._ser.reset_input_buffer()  # drop the boot banner / old output
        else:
            self.port = port or "fake"
            self._ser = serial_factory()

    def close(self) -> None:
        self._ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _send(self, cmd: str) -> None:
        self._ser.write((cmd + "\n").encode("ascii"))

    def _lines(self, timeout: float) -> Iterator[str]:
        """Yield decoded lines until timeout seconds pass with no complete line."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = self._ser.readline()
            if not raw:
                continue
            deadline = time.monotonic() + timeout
            yield raw.decode("utf-8", errors="replace").rstrip("\r\n")

    def _wait_prefix(self, prefix: str, timeout: float) -> str:
        for line in self._lines(timeout):
            if line.startswith(prefix):
                return line
            if line.startswith("@ERR"):
                raise PicoAudioError(line)
        raise PicoAudioError(f"timeout waiting for {prefix}")

    def ping(self) -> bool:
        self._send("p")
        return self._wait_prefix("@PONG", 3.0) == "@PONG"

    def record(self, seconds: int, retries: int = 2) -> np.ndarray:
        """Record seconds (1-5) on the Pico and return the CRC-checked samples (uint16, 0-4095)."""
        if not 1 <= seconds <= 5:
            raise ValueError("seconds must be 1-5")
        self._send(f"r{seconds}")
        block = parse_audio(self._lines(seconds + self.timeout))
        for _ in range(retries):
            if block.crc_ok:
                break
            self._send("s")  # resend the same recording, do not record again
            block = parse_audio(self._lines(self.timeout))
        if not block.crc_ok:
            raise PicoAudioError(f"CRC/length check failed after {retries} resends")
        if len(block.samples) != seconds * RATE:
            raise PicoAudioError(f"expected {seconds * RATE} samples, got {len(block.samples)}")
        self.last_mean = block.mean
        return block.samples

    def features(self) -> np.ndarray:
        """The Pico's sound_features.c vector for the last recording."""
        self._send("f")
        return parse_feat(self._wait_prefix("@FEAT", 10.0))

    def classify(self) -> dict:
        """The Pico's classification of the last recording: id, name, group, conf, model_us."""
        self._send("c")
        return parse_cls(self._wait_prefix("@CLS", 10.0))


def main() -> int:
    ap = argparse.ArgumentParser(description="Record raw 16 kHz audio from a Pico running audio_dump.uf2.")
    ap.add_argument("--port", help="COM5 or /dev/cu.usbmodem1101 (default: auto-detect by USB VID 0x2E8A)")
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    ap.add_argument("--test", type=int, metavar="SECONDS", help="record 1-5 s and print length, mean, std")
    ap.add_argument("--save", help="with --test: save the samples to this .npy file")
    ap.add_argument("--features", action="store_true", help="with --test: also print the Pico's features + class")
    args = ap.parse_args()

    if args.list:
        from serial.tools import list_ports
        for p in list_ports.comports():
            print(f"{p.device}  {p.description}" + ("  <- Pico" if p.vid == PICO_VID else ""))
        return 0
    if args.test is None:
        ap.print_help()
        return 0
    try:
        with PicoAudio(args.port) as pico:
            print(f"port {pico.port}: recording {args.test} s ...")
            x = pico.record(args.test)
            xf = x.astype(np.float64)
            print(f"length={len(x)} mean={xf.mean():.1f} (pico says {pico.last_mean}) std={xf.std():.1f} "
                  f"min={x.min()} max={x.max()} crc=ok")
            if args.save:
                np.save(args.save, x)
                print(f"saved {args.save}")
            if args.features:
                print("features:", ",".join(f"{v:.4g}" for v in pico.features()))
                print("class:", pico.classify())
    except PicoAudioError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
