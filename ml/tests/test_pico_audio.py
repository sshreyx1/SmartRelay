"""Parser and protocol tests for ml/pico_audio.py on synthetic audio_dump transcripts (no hardware)."""
import base64
import sys
import zlib
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import pico_audio  # noqa: E402
from pico_audio import PicoAudio, PicoAudioError, parse_audio, parse_cls, parse_feat  # noqa: E402


def transcript(samples: np.ndarray, eol: str = "\r\n", corrupt: bool = False) -> list:
    raw = samples.astype("<u2").tobytes()
    crc = zlib.crc32(raw) & 0xFFFFFFFF
    lines = ["some log line", f"@AUD BEGIN rate=16000 n={len(samples)} mean={int(samples.mean())}"]
    for off in range(0, len(raw), 48):
        lines.append("@A " + base64.b64encode(raw[off:off + 48]).decode())
    if corrupt:
        lines[3] = "@A " + base64.b64encode(b"\x00" * 48).decode()
    lines.append(f"@AUD END n={len(samples)} crc={crc:08x}")
    return [ln + eol for ln in lines]


def samples(n=1000, seed=0):
    return np.random.default_rng(seed).integers(1900, 2300, n).astype(np.uint16)


@pytest.mark.parametrize("eol", ["\n", "\r\n"])
def test_parse_audio_roundtrip(eol):
    x = samples()
    b = parse_audio(iter(ln.rstrip("\r\n") for ln in transcript(x, eol)))
    assert b.crc_ok and b.rate == 16000 and np.array_equal(b.samples, x)


def test_parse_audio_detects_corruption():
    x = samples()
    b = parse_audio(iter(transcript(x, corrupt=True)))
    assert not b.crc_ok


def test_parse_audio_odd_length_lines():
    x = samples(n=1001)  # 2002 bytes: last base64 line is short and padded
    assert parse_audio(iter(transcript(x))).crc_ok


def test_parse_audio_missing_end():
    x = samples()
    with pytest.raises(PicoAudioError):
        parse_audio(iter(transcript(x)[:-1]))


def test_parse_feat_and_cls():
    f = parse_feat("@FEAT 1.5,-2,3e-05\r\n")
    assert f.dtype == np.float32 and f.tolist() == pytest.approx([1.5, -2, 3e-05])
    c = parse_cls("@CLS id=20 name=DOG group=MAMMAL conf=82 model_us=310")
    assert c == {"id": 20, "name": "DOG", "group": "MAMMAL", "conf": 82, "model_us": 310}


class FakeSerial:
    """Answers r<sec> with a transcript (first one corrupt if asked), s with a clean resend."""

    def __init__(self, x, corrupt_first=False):
        self.x, self.corrupt_first, self.out, self.sent = x, corrupt_first, [], []

    def write(self, data):
        cmd = data.decode().strip()
        self.sent.append(cmd)
        if cmd.startswith("r"):
            self.out += transcript(self.x, corrupt=self.corrupt_first)
        elif cmd == "s":
            self.out += transcript(self.x)
        elif cmd == "f":
            self.out.append("@FEAT 1,2,3\r\n")
        elif cmd == "c":
            self.out.append("@CLS id=1 name=BIRD group=BIRD conf=60 model_us=100\r\n")
        elif cmd == "p":
            self.out.append("@PONG\r\n")

    def readline(self):
        return self.out.pop(0).encode() if self.out else b""

    def close(self):
        pass


def test_record_resends_on_crc_failure(monkeypatch):
    monkeypatch.setattr(pico_audio, "RATE", 200)  # 1 s = 200 samples keeps the test small
    x = samples(n=200)
    fake = FakeSerial(x, corrupt_first=True)
    with PicoAudio(serial_factory=lambda: fake, timeout=0.2) as p:
        got = p.record(1)
        assert np.array_equal(got, x)
        assert fake.sent == ["r1", "s"]
        assert p.features().tolist() == [1, 2, 3]
        assert p.classify()["name"] == "BIRD"
        assert p.ping()
