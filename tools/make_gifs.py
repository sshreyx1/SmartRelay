#!/usr/bin/env python3
"""
make_gifs.py - draws the animated diagrams used by README.md and docs/ into docs/media/.

Every number on the diagrams comes from the source: wifi_mesh_config.h (neighbour table),
alfp_config.h / alfp_wire.h / alfp_profiles.c (protocol), main_smp.c / rtos_port.h (tasks),
sound_features.h / sensing.h (sensing).

Run:      pip install pillow
          python tools/make_gifs.py
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).resolve().parents[1] / "docs" / "media"
SCALE = 2  # draw at 2x and downsample for smooth edges

BG = (250, 250, 247)
INK = (25, 33, 28)
MUTED = (110, 118, 112)
LINE = (196, 202, 196)
CARD = (255, 255, 255)
GREEN = (31, 94, 61)
GREEN_SOFT = (226, 240, 232)
LORA = (217, 119, 6)
LORA_SOFT = (253, 237, 210)
WIFI = (37, 99, 235)
WIFI_SOFT = (222, 233, 253)
RED = (200, 40, 40)
RED_SOFT = (252, 226, 226)
PURPLE = (109, 66, 193)
PURPLE_SOFT = (237, 230, 250)

FONT_DIRS = ["C:/Windows/Fonts", "/usr/share/fonts/truetype/dejavu", "/Library/Fonts",
             "/System/Library/Fonts/Supplemental"]
_fonts = {}


def font(size, bold=False, mono=False):
    key = (size, bold, mono)
    if key not in _fonts:
        names = (["consolab.ttf", "DejaVuSansMono-Bold.ttf"] if bold else ["consola.ttf", "DejaVuSansMono.ttf"]) \
            if mono else (["segoeuib.ttf", "DejaVuSans-Bold.ttf", "Arial Bold.ttf"] if bold
                          else ["segoeui.ttf", "DejaVuSans.ttf", "Arial.ttf"])
        for d in FONT_DIRS:
            for n in names:
                if (Path(d) / n).exists():
                    _fonts[key] = ImageFont.truetype(str(Path(d) / n), size * SCALE)
                    return _fonts[key]
        _fonts[key] = ImageFont.load_default(size * SCALE)
    return _fonts[key]


class Canvas:
    def __init__(self, width, height):
        self.size = (width, height)
        self.im = Image.new("RGB", (width * SCALE, height * SCALE), BG)
        self.d = ImageDraw.Draw(self.im)

    @staticmethod
    def _s(values):
        return [v * SCALE for v in values]

    def box(self, x, y, w, h, fill=CARD, edge=LINE, width=1.5, r=10):
        self.d.rounded_rectangle(self._s([x, y, x + w, y + h]), radius=r * SCALE, fill=fill, outline=edge,
                                 width=int(width * SCALE))

    def circle(self, x, y, r, fill=CARD, edge=LINE, width=1.5):
        self.d.ellipse(self._s([x - r, y - r, x + r, y + r]), fill=fill, outline=edge, width=int(width * SCALE))

    def line(self, a, b, color=LINE, width=1.5, dash=0):
        if not dash:
            self.d.line(self._s([*a, *b]), fill=color, width=int(width * SCALE))
            return
        length = ((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5
        pos = 0.0
        while pos < length:
            self.line(lerp(a, b, pos / length), lerp(a, b, min(1, (pos + dash) / length)), color, width)
            pos += dash * 2

    def arrow(self, a, b, color=INK, width=2.0, head=9):
        self.line(a, b, color, width)
        dx, dy = b[0] - a[0], b[1] - a[1]
        n = (dx * dx + dy * dy) ** 0.5 or 1
        ux, uy = dx / n, dy / n
        left = (b[0] - ux * head - uy * head * 0.55, b[1] - uy * head + ux * head * 0.55)
        right = (b[0] - ux * head + uy * head * 0.55, b[1] - uy * head - ux * head * 0.55)
        self.d.polygon([tuple(self._s(p)) for p in (b, left, right)], fill=color)

    def text(self, x, y, s, size=14, color=INK, bold=False, mono=False, anchor="mm"):
        self.d.text((x * SCALE, y * SCALE), s, font=font(size, bold, mono), fill=color, anchor=anchor)

    def packet(self, x, y, color, label="", r=7):
        self.circle(x, y, r, fill=color, edge=CARD, width=1.5)
        if label:
            self.text(x, y - r - 9, label, 11, color, bold=True)

    def frame(self):
        return self.im.resize(self.size, Image.LANCZOS)


def lerp(a, b, t):
    return a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t


def along(points, t):
    """Position at fraction t of a poly-line."""
    t = max(0.0, min(0.9999, t))
    n = len(points) - 1
    i = int(t * n)
    return lerp(points[i], points[i + 1], t * n - i)


def save(name, frames, ms=90, hold_last=1500):
    OUT.mkdir(parents=True, exist_ok=True)
    sample = frames[::max(1, len(frames) // 8)]
    sheet = Image.new("RGB", (sample[0].width, sample[0].height * len(sample)))
    for i, frame in enumerate(sample):
        sheet.paste(frame, (0, i * sample[0].height))
    palette = sheet.quantize(colors=256, method=Image.MEDIANCUT, dither=Image.Dither.NONE)
    pal = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f in frames]
    durations = [ms] * (len(pal) - 1) + [hold_last]
    pal[0].save(OUT / name, save_all=True, append_images=pal[1:], duration=durations, loop=0, optimize=True)
    print(f"{name}: {len(frames)} frames, {(OUT / name).stat().st_size // 1024} kB")


# ---------------------------------------------------------------- 1. the mesh ------------------
POS = {1: (270, 270), 2: (470, 130), 3: (690, 130), 4: (690, 340), 5: (880, 235)}
NEIGHBOURS = {1: {2, 3, 4}, 2: {1, 3, 4}, 3: {1, 2, 4, 5}, 4: {1, 2, 3, 5}, 5: {3, 4}}
LAPTOP = (85, 270)


def mesh_base(c, active=()):
    c.text(24, 28, "SmartRelay trail mesh", 21, INK, True, anchor="lm")
    c.text(24, 54, "Grey lines: who can hear whom over WiFi (topology filter ON)", 13, MUTED, anchor="lm")
    for a, nbs in NEIGHBOURS.items():
        for b in nbs:
            if a < b:
                c.line(POS[a], POS[b], LINE, 1.5)
    c.arrow((POS[1][0] - 60, 270), (LAPTOP[0] + 58, 270), GREEN, 3)
    c.text(172, 254, "USB serial", 11, GREEN, True)
    c.box(LAPTOP[0] - 56, 238, 112, 64)
    c.text(LAPTOP[0], 260, "Laptop", 14, INK, True)
    c.text(LAPTOP[0], 281, "backend + UI", 11, MUTED)
    for node, (x, y) in POS.items():
        if node == 1:
            c.box(x - 60, y - 34, 120, 68, GREEN, GREEN)
            c.text(x, y - 11, "Node 1 · bridge", 13, CARD, True)
            c.text(x, y + 11, "receives + CRC check", 10, (205, 230, 214))
            continue
        on = node in active
        c.circle(x, y, 39, GREEN_SOFT if on else CARD, GREEN if on else (150, 158, 150), 3 if on else 1.5)
        c.text(x, y - 7, f"Node {node}", 14, INK, True)
        c.text(x, y + 13, "field", 11, MUTED)
    c.circle(30, 438, 6, LORA, LORA)
    c.text(44, 438, "LoRa: files under 1024 B", 12, INK, anchor="lm")
    c.circle(250, 438, 6, WIFI, WIFI)
    c.text(264, 438, "WiFi (ESP-NOW): files of 1024 B or more", 12, INK, anchor="lm")
    c.circle(580, 438, 6, GREEN, GREEN)
    c.text(594, 438, "USB to laptop", 12, INK, anchor="lm")


def mesh_gif():
    frames = []

    def scene(caption, active, packets):
        c = Canvas(960, 460)
        mesh_base(c, active)
        c.box(300, 384, 636, 34, CARD, LINE)
        c.text(618, 401, caption, 13, INK, True)
        for pos, color, label in packets:
            c.packet(*pos, color, label)
        frames.append(c.frame())

    for _ in range(8):
        scene("PIR and sound trigger fire together on node 5", {5}, [])
    for i in range(18):
        t = i / 17
        scene("27-byte telemetry is under 1024 B, so it goes over LoRa", {5},
              [(along([POS[5], (480, 262), POS[1]], t), LORA, "telemetry")])
    for i in range(46):
        pk = []
        for k in range(7):
            t = (i - k * 3) / 22
            if 0 <= t <= 1:
                pk.append((along([POS[5], POS[4], POS[1]], t), WIFI, "photo fragments" if k == 0 and 0.2 < t < 0.8 else ""))
        scene("Photo goes over WiFi. Node 5 cannot reach node 1, so node 4 relays it", {4, 5}, pk)
    for i in range(14):
        scene("Node 1 checks CRC-32, saves to microSD and streams to the laptop", {5},
              [(along([(210, 270), (143, 270)], i / 13), GREEN, "file")])
    for _ in range(6):
        scene("Dashboard shows the capture", {5}, [])
    save("mesh.gif", frames)


# ---------------------------------------------------------------- 2. ALFP ----------------------
def alfp_gif():
    left, right, top = 170, 790, 118
    steps = [  # (direction, label, colour, lost)
        (1, "MANIFEST  size, fragment count, CRC-32, capture metadata", PURPLE, False),
        (1, "DATA seq 0  + CRC-16", WIFI, False),
        (1, "DATA seq 1  + CRC-16", WIFI, False),
        (1, "DATA seq 2  + CRC-16", WIFI, True),
        (1, "DATA seq 3  + CRC-16", WIFI, False),
        (1, "DATA seq 4  + CRC-16", WIFI, True),
        (1, "POLL  \"what are you missing?\"", INK, False),
        (-1, "NACK  missing: 2, 4", RED, False),
        (1, "DATA seq 2  (resent, sent twice when 4 or fewer remain)", LORA, False),
        (1, "DATA seq 4  (resent)", LORA, False),
        (-1, "ACK OK  whole-file CRC-32 matches", GREEN, False),
    ]
    frames = []

    def scene(done, current, t):
        c = Canvas(960, 520)
        c.text(24, 28, "ALFP: sender-polled selective repeat", 21, INK, True, anchor="lm")
        c.text(24, 54, "Only the missing fragments are sent again (example: a 5-fragment file)", 13, MUTED,
               anchor="lm")
        for x, name, sub in ((left, "Field node (sender)", "reads the file from microSD"),
                             (right, "Bridge (receiver)", "writes each fragment at seq × size")):
            c.box(x - 110, 72, 220, 40, GREEN_SOFT, GREEN)
            c.text(x, 84, name, 13, INK, True)
            c.text(x, 101, sub, 10, MUTED)
            c.line((x, 112), (x, 500), LINE, 1.5, dash=5)
        have = []
        for i in range(done + (1 if current else 0)):
            direction, label, color, lost = steps[i]
            y = top + 22 + i * 33
            a, b = (left, right) if direction > 0 else (right, left)
            moving = current and i == done
            frac = t if moving else 1.0
            end = lerp((a, y), (b, y), frac * (0.56 if lost else 1.0))
            if moving:
                c.line((a, y), end, color, 2)
                c.packet(*end, color)
            else:
                if lost:
                    c.line((a, y), end, color, 2, dash=6)
                    c.text(end[0] + 14, y, "x lost", 12, RED, True, anchor="lm")
                else:
                    c.arrow((a, y), end, color, 2)
            c.text((left + right) / 2 - (150 if lost else 0), y - 10, label, 12, color, True)
            if label.startswith("DATA") and not lost and not moving:
                have.append(int(label.split()[2]))
        for seq in range(5):
            y = 430
            filled = seq in have
            c.box(842 + (seq % 5) * 22, y, 18, 18, GREEN if filled else CARD, GREEN if filled else LINE, r=3)
            c.text(851 + seq * 22, y + 28, str(seq), 10, MUTED)
        c.text(895, 416, "bridge bitmap", 11, MUTED, True)
        frames.append(c.frame())

    for i in range(len(steps)):
        for k in range(1, 7):
            scene(i, True, k / 6)
    scene(len(steps), False, 1)
    save("alfp.gif", frames, ms=95, hold_last=2800)


# ---------------------------------------------------------------- 3. dual-core kernel -----------
def smp_gif():
    frames = []
    for i in range(72):
        c = Canvas(960, 500)
        c.text(24, 28, "µT-Kernel 3 SMP: both RP2040 cores", 21, INK, True, anchor="lm")
        c.text(24, 54, "Each hardware resource has exactly one owning task", 13, MUTED, anchor="lm")
        c.box(24, 76, 574, 312, (244, 247, 244), GREEN, 2)
        c.text(40, 96, "CORE 0", 15, GREEN, True, anchor="lm")
        c.box(618, 76, 318, 312, (245, 242, 251), PURPLE, 2)
        c.text(634, 96, "CORE 1", 15, PURPLE, True, anchor="lm")

        def task(x, y, w, h, name, prio, lines, edge, fill, lit):
            c.box(x, y, w, h, fill if lit else CARD, edge, 3 if lit else 1.5)
            c.text(x + 14, y + 20, name, 15, INK, True, anchor="lm")
            c.text(x + w - 14, y + 20, prio, 11, MUTED, anchor="rm")
            for n, s in enumerate(lines):
                c.text(x + 14, y + 46 + n * 20, s, 12, INK, anchor="lm")

        phase = (i // 24) % 3
        t = (i % 24) / 23
        task(40, 114, 270, 168, "comms", "priority 10 · 8 KB",
             ["SPI1: microSD + LoRa", "UART0: ESP-01 WiFi", "mesh poll, ALFP, outbox", "bridge USB uplink data"],
             GREEN, GREEN_SOFT, phase in (0, 2))
        task(330, 114, 252, 168, "usb_owner", "priority 20 · 3 KB",
             ["the only task that", "writes to USB", "reads console keys"], GREEN, GREEN_SOFT, phase == 1)
        task(634, 114, 286, 168, "sensors", "priority 10 · 8 KB",
             ["PIR, sound trigger, mic", "DHT22, light, weather", "16 kHz audio + classifier"],
             PURPLE, PURPLE_SOFT, phase in (0, 1))

        c.box(330, 306, 300, 62, CARD, PURPLE)
        c.text(480, 323, "capture queue (message buffer, depth 4)", 12, INK, True)
        for s in range(4):
            c.box(388 + s * 46, 338, 38, 20, PURPLE if (phase == 0 and s == 0 and t > 0.45) else CARD, PURPLE, r=4)
        c.box(40, 306, 270, 62, CARD, GREEN)
        c.text(175, 323, "log ring", 12, INK, True)
        c.text(175, 347, "whole lines, never blocks the sender", 11, MUTED)

        if phase == 0:
            c.packet(*along([(700, 282), (700, 337), (630, 337), (330, 337), (240, 282)], t), PURPLE, "capture")
            caption = "sensors requests a capture: a non-blocking send. A full queue refuses and counts it"
        elif phase == 1:
            c.packet(*along([(640, 282), (480, 400), (200, 400), (175, 368)], t), PURPLE, "log line")
            c.packet(*along([(310, 337), (330, 300), (400, 282)], t), GREEN)
            caption = "other tasks printf() into the log ring; usb_owner drains it to USB"
        else:
            c.packet(*along([(175, 282), (175, 306)], t), GREEN, "")
            caption = "UART0 interrupt only fills a ring under a spinlock; comms parses it in task context"
        c.box(24, 420, 912, 46, CARD, LINE)
        c.text(480, 443, caption, 14, INK, True)
        frames.append(c.frame())
    save("smp-dual-core.gif", frames, ms=110, hold_last=110)


# ---------------------------------------------------------------- 4. sense -> send --------------
def pipeline_gif():
    stages = [
        ("Trigger", ["PIR (GP0) and", "sound (GP14) high", "5 s cooldown"]),
        ("Record", ["MAX9814 mic, GP26", "16 kHz for 5 s", "ADC + DMA ring"]),
        ("Features", ["256-sample frames", "256-point FFT", "25 features"]),
        ("Classify", ["decision-tree", "ensembles on the", "Pico: sound group"]),
        ("Pick photo", ["samples/<label>_NN", "else group, else", "generic img_NN"]),
        ("Send", ["telemetry: LoRa", "photo: WiFi", "other radio if down"]),
    ]
    frames = []
    per = 11
    for i in range(per * len(stages) + 6):
        c = Canvas(960, 250)
        c.text(24, 26, "One capture on a field node", 20, INK, True, anchor="lm")
        c.text(24, 50, "Environment readings (DHT22, light, weather label) travel in the same record", 12, MUTED,
               anchor="lm")
        active = min(len(stages) - 1, i // per)
        for n, (title, lines) in enumerate(stages):
            x = 20 + n * 156
            lit = n == active
            done = n < active
            c.box(x, 82, 140, 138, GREEN_SOFT if lit else CARD, GREEN if (lit or done) else LINE, 3 if lit else 1.5)
            c.text(x + 70, 106, title, 14, GREEN if (lit or done) else INK, True)
            for k, s in enumerate(lines):
                c.text(x + 70, 140 + k * 22, s, 11, INK)
            if n < len(stages) - 1:
                c.arrow((x + 141, 151), (x + 155, 151), GREEN if done else LINE, 2.5, 7)
        c.box(20 + active * 156 + 20, 206, 100 * min(1.0, (i % per + 1) / per), 6, GREEN, GREEN, r=3)
        frames.append(c.frame())
    save("capture-pipeline.gif", frames, ms=120)


if __name__ == "__main__":
    mesh_gif()
    alfp_gif()
    smp_gif()
    pipeline_gif()
