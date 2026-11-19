#!/usr/bin/env python3
"""
fetch_label_images.py - download a few free-licence photos per sound label from Wikimedia Commons
into ml/images_src/<prefix>/, with ml/images_src/credits.csv (file, prefix, title, author, licence,
source URL). Only CC0, public domain, CC BY and CC BY-SA files are kept. Then run
tools/make_label_images.py to shrink them for the SD cards and write docs/sound/IMAGE_CREDITS.md.

Uses 320 px thumbnails (small downloads) and the standard library only.

Run:      python tools/fetch_label_images.py --per-label 2
          python tools/fetch_label_images.py --only dog cat --per-label 3
"""
import argparse
import csv
import html
import json
import re
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "common"))
import sound_classes as sc  # noqa: E402

API = "https://commons.wikimedia.org/w/api.php"
UA = "SmartRelay-CSC09-coursework/1.0 (sound label images; https://github.com/)"
OK_LICENCES = re.compile(r"^(CC0|Public domain|PD|CC BY(-SA)? \d)", re.I)

# search terms per image prefix (Singapore-flavoured where it makes sense)
QUERIES = {
    "ambient": "Bukit Timah Nature Reserve forest",
    "bird": "bird perched branch",
    "mammal": "long-tailed macaque",
    "insect_frog": "tree frog",
    "human": "hiker with backpack walking",
    "vehicle": "car driving road",
    "weather": "tropical rain storm",
    "rooster": "red junglefowl rooster",
    "hen": "hen chicken",
    "crow": "house crow Corvus splendens",
    "songbird": "oriental magpie-robin",
    "dog": "dog running grass",
    "cat": "domestic cat",
    "cow": "cattle grazing field",
    "pig": "pig farm sow",
    "sheep": "sheep flock meadow",
    "frog": "Asian common toad",
    "cricket": "field cricket Gryllus",
    "insect_buzz": "cicada",
}
FIELDS = ["file", "prefix", "title", "author", "licence", "source"]


def api(params: dict) -> dict:
    url = API + "?" + urllib.parse.urlencode({**params, "format": "json"})
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode("utf-8"))


def strip_html(s: str) -> str:
    return html.unescape(re.sub(r"<[^>]+>", "", s or "")).strip()


def search(query: str, n: int) -> list:
    d = api({"action": "query", "generator": "search", "gsrsearch": f"filetype:bitmap {query}", "gsrnamespace": 6,
             "gsrlimit": 20, "prop": "imageinfo", "iiprop": "url|extmetadata|mime", "iiurlwidth": 320})
    out = []
    for page in sorted(d.get("query", {}).get("pages", {}).values(), key=lambda p: p.get("index", 0)):
        info = (page.get("imageinfo") or [{}])[0]
        meta = info.get("extmetadata", {})
        lic = meta.get("LicenseShortName", {}).get("value", "")
        if info.get("mime") != "image/jpeg" or not OK_LICENCES.match(lic) or "thumburl" not in info:
            continue
        out.append({"title": page["title"], "thumb": info["thumburl"], "source": info.get("descriptionurl", ""),
                    "licence": lic, "author": strip_html(meta.get("Artist", {}).get("value", "")) or "unknown"})
        if len(out) >= n:
            break
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Fetch free-licence label photos from Wikimedia Commons.")
    ap.add_argument("--per-label", type=int, default=2)
    ap.add_argument("--only", nargs="*", help="only these prefixes")
    ap.add_argument("--out", type=Path, default=ROOT / "ml" / "images_src")
    args = ap.parse_args()

    prefixes = args.only or [c.prefix for c in sc.CLASSES]
    credits_path = args.out / "credits.csv"
    rows = []
    if credits_path.exists():
        with open(credits_path, encoding="utf-8", newline="") as f:
            rows = [r for r in csv.DictReader(f) if r["prefix"] not in prefixes]
    for prefix in prefixes:
        q = QUERIES.get(prefix)
        if not q:
            print(f"{prefix}: no search term, skipped")
            continue
        try:
            hits = search(q, args.per_label)
        except Exception as e:
            print(f"{prefix}: search failed ({e})")
            continue
        d = args.out / prefix
        d.mkdir(parents=True, exist_ok=True)
        for i, h in enumerate(hits):
            p = d / f"{prefix}_src{i}.jpg"
            req = urllib.request.Request(h["thumb"], headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=60) as r:
                p.write_bytes(r.read())
            rows.append({"file": p.relative_to(args.out).as_posix(), "prefix": prefix, "title": h["title"],
                         "author": h["author"], "licence": h["licence"], "source": h["source"]})
            time.sleep(0.3)  # be polite to the API
        print(f"{prefix}: {len(hits)} image(s) for '{q}'")
    with open(credits_path, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(sorted(rows, key=lambda r: r["file"]))
    print(f"credits -> {credits_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
