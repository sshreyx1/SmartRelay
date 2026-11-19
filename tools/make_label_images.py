#!/usr/bin/env python3
"""
make_label_images.py - turn photos sorted by sound label into the small JPEGs the node picks from.

Input:  ml/images_src/<prefix>/*.jpg   (prefix = a label's image prefix in common/sound_classes.py,
        e.g. dog, rooster, bird; or img for generic) + optional ml/images_src/credits.csv
        (file, prefix, title, author, licence, source; tools/fetch_label_images.py writes it)
Output: sd_card/samples/<prefix>_NN.jpg (NN = 00, 01, ... in file-name order), each <= --max-kb,
        longest side <= --max-px, quality stepped down (then size) until it fits;
        docs/sound/IMAGE_CREDITS.md listing every image's author, licence and source.
Copy sd_card/samples/ onto every field node's SD card (keep its existing img_NN.jpg too).

Use free-licence photos only (Wikimedia Commons, Unsplash, Pixabay) and keep their credits.
Small matters: every kB costs about 7 s over 2 LoRa hops (docs/results/e6_size.csv).

Run:      python tools/make_label_images.py
          python tools/make_label_images.py --max-kb 6 --max-px 200
"""
import argparse
import csv
import io
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "common"))
import sound_classes as sc  # noqa: E402

GENERIC = "img"


def shrink(src: Path, max_px: int, max_bytes: int) -> bytes:
    from PIL import Image
    im = Image.open(src).convert("RGB")
    scale = 1.0
    while True:
        w, h = im.size
        f = min(1.0, max_px * scale / max(w, h))
        small = im.resize((max(1, round(w * f)), max(1, round(h * f))), Image.LANCZOS) if f < 1 else im
        for q in range(85, 19, -5):
            buf = io.BytesIO()
            small.save(buf, "JPEG", quality=q, optimize=True, progressive=False)
            if buf.tell() <= max_bytes:
                return buf.getvalue()
        scale *= 0.8
        if max_px * scale < 32:
            raise ValueError(f"{src}: cannot get under {max_bytes} bytes")


def main() -> int:
    ap = argparse.ArgumentParser(description="Make <prefix>_NN.jpg label images for the SD cards.")
    ap.add_argument("--src", type=Path, default=ROOT / "ml" / "images_src")
    ap.add_argument("--out", type=Path, default=ROOT / "sd_card" / "samples")
    ap.add_argument("--max-kb", type=float, default=10.0)
    ap.add_argument("--max-px", type=int, default=240)
    ap.add_argument("--credits", type=Path, default=ROOT / "docs" / "sound" / "IMAGE_CREDITS.md")
    args = ap.parse_args()

    valid = {c.prefix for c in sc.CLASSES} | {GENERIC}
    credits = {}
    cpath = args.src / "credits.csv"
    if cpath.exists():
        with open(cpath, encoding="utf-8", newline="") as f:
            credits = {r["file"]: r for r in csv.DictReader(f)}
    if not args.src.is_dir():
        print(f"error: {args.src} not found; put photos in {args.src}/<prefix>/", file=sys.stderr)
        return 1
    args.out.mkdir(parents=True, exist_ok=True)
    made = []
    for d in sorted(p for p in args.src.iterdir() if p.is_dir()):
        if d.name not in valid:
            print(f"skipping {d.name}/: not an image prefix ({', '.join(sorted(valid))})")
            continue
        photos = sorted(p for p in d.iterdir() if p.suffix.lower() in (".jpg", ".jpeg", ".png"))
        for nn, p in enumerate(photos[:100]):
            data = shrink(p, args.max_px, int(args.max_kb * 1024))
            out = args.out / f"{d.name}_{nn:02d}.jpg"
            out.write_bytes(data)
            made.append((out.name, len(data), credits.get(p.relative_to(args.src).as_posix())))
    lines = ["# Image credits: SD card label images", "",
             "The node sends one of these stand-in \"camera\" images with each capture, chosen by the sound label "
             "it heard (`firmware/sensing/sound_image_pick.c`). Made by `tools/make_label_images.py` from photos in "
             "`ml/images_src/`; downloaded from Wikimedia Commons by `tools/fetch_label_images.py`. "
             "Only CC0, public-domain, CC BY and CC BY-SA files were used; each is resized and recompressed.", "",
             "| SD file | bytes | title | author | licence | source |", "|---|---:|---|---|---|---|"]
    for name, size, c in made:
        if c:
            author = c["author"].replace("|", "/").replace("\n", " ")[:80]
            lines.append(f"| {name} | {size} | {c['title'].replace('|', '/')} | {author} | {c['licence']} | "
                         f"[link]({c['source']}) |")
        else:
            lines.append(f"| {name} | {size} | (own photo, no credits.csv row) | | | |")
    args.credits.parent.mkdir(parents=True, exist_ok=True)
    args.credits.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    total = sum(s for _, s, _ in made)
    print(f"{len(made)} images -> {args.out} ({total / 1024:.0f} kB, max {max((s for _, s, _ in made), default=0)} B); "
          f"credits -> {args.credits}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
