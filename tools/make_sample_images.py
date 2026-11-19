#!/usr/bin/env python3
"""
make_sample_images.py - prepare the stand-in "camera" images for the SD cards.

Takes any photos you have (e.g. wildlife pictures from the web or your phone) and writes small
JPEGs named the way the firmware expects:

    python3 tools/make_sample_images.py photos/*.jpg --out sd_card/samples
    -> sd_card/samples/img_00.jpg, img_01.jpg, ...   (copy the samples folder onto each SD card)

Why small: every kB costs about 7 s over 2 LoRa hops (docs/results/e6_size.csv).
160x120 at quality 60 is usually 3-6 kB, i.e. 13-25 fragments.

Options: --width 160 --height 120 --quality 60     Needs: pip install pillow
"""
import argparse
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("photos", nargs="+")
    ap.add_argument("--out", default="sd_card/samples")
    ap.add_argument("--width", type=int, default=160)
    ap.add_argument("--height", type=int, default=120)
    ap.add_argument("--quality", type=int, default=60)
    a = ap.parse_args()
    try:
        from PIL import Image
    except ImportError:
        print("Pillow missing: pip install pillow", file=sys.stderr)
        return 2
    os.makedirs(a.out, exist_ok=True)
    for i, p in enumerate(a.photos[:100]):
        im = Image.open(p).convert("RGB")
        im.thumbnail((a.width * 4, a.height * 4))
        # centre-crop to the target aspect ratio, then resize
        tw, th = a.width, a.height
        w, h = im.size
        scale = max(tw / w, th / h)
        im = im.resize((max(tw, int(w * scale)), max(th, int(h * scale))))
        w, h = im.size
        im = im.crop(((w - tw) // 2, (h - th) // 2, (w - tw) // 2 + tw, (h - th) // 2 + th))
        out = os.path.join(a.out, f"img_{i:02d}.jpg")
        im.save(out, "JPEG", quality=a.quality, optimize=True)
        size = os.path.getsize(out)
        print(f"{out}: {size} bytes, {-(-size // 241)} LoRa fragments")
    return 0


if __name__ == "__main__":
    sys.exit(main())
