#!/usr/bin/env python3
"""
dashboard_media.py - screenshots and GIFs of the dashboard for README.md and docs/DASHBOARD.md.

Drives the real dashboard in a headless browser against a running backend, so every image shows
what the code actually renders. All data on the images is SIMULATED (simulator/demo_feed.py) and
the dashboard labels it so.

Needs:    pip install playwright pillow requests     (uses the installed Google Chrome; or run
          "playwright install chromium" once and pass --browser chromium)
Run:      1. start a backend on an empty database, serving the built dashboard:
                cd dashboard && npm run build && cd ../backend
                SMARTRELAY_DB=/tmp/media.db SMARTRELAY_CAPTURES_DIR=/tmp/media_rx uvicorn app:app --port 8000
          2. python simulator/demo_feed.py --burst 14 --label-images sd_card/samples
             python simulator/demo_feed.py --scenario bridge-down     (then lora, normal, loss)
          3. python tools/dashboard_media.py
Writes:   docs/media/dashboard/*.png, *.gif
"""
from __future__ import annotations

import argparse
import io
import subprocess
import sys
import threading
import time
from pathlib import Path

from PIL import Image
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "docs" / "media" / "dashboard"
VIEW = {"width": 1440, "height": 900}
GIF_WIDTH = 1100

PAGES = [  # (file name, route, a selector that only appears once the page has data)
    ("overview", "/?data=sim", ".kpi"),
    ("gallery", "/gallery?data=sim", ".card"),
    ("captures", "/captures?data=sim", ".tbl tbody tr"),
    ("nodes", "/nodes/4?data=sim", "main h1"),
    ("sounds", "/sounds?data=sim", "main h1"),
    ("log", "/log", ".log-tbl tbody tr"),
]


def shot(page) -> Image.Image:
    return Image.open(io.BytesIO(page.screenshot())).convert("RGB")


def save_gif(frames: list[Image.Image], name: str, ms: int = 250, hold_last_ms: int = 2500) -> None:
    small = [f.resize((GIF_WIDTH, round(f.height * GIF_WIDTH / f.width)), Image.LANCZOS) for f in frames]
    pal = [f.quantize(colors=128, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE) for f in small]
    durations = [ms] * (len(pal) - 1) + [hold_last_ms]
    pal[0].save(OUT / name, save_all=True, append_images=pal[1:], duration=durations, loop=0, optimize=True)
    print(f"wrote {OUT / name} ({len(pal)} frames, {(OUT / name).stat().st_size // 1024} KB)")


def record(page, seconds: float, every: float = 0.25) -> list[Image.Image]:
    frames, end = [], time.time() + seconds
    while time.time() < end:
        t0 = time.time()
        frames.append(shot(page))
        time.sleep(max(0.0, every - (time.time() - t0)))
    return frames


def scenario(backend: str, name: str, node: int) -> threading.Thread:
    cmd = [sys.executable, str(ROOT / "simulator" / "demo_feed.py"), "--backend", backend, "--scenario", name,
           "--node", str(node)]
    t = threading.Thread(target=subprocess.run, args=(cmd,), kwargs={"capture_output": True})
    t.start()
    return t


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="http://127.0.0.1:8000", help="backend serving the built dashboard")
    ap.add_argument("--browser", default="chrome", choices=["chrome", "chromium"])
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    with sync_playwright() as pw:
        browser = pw.chromium.launch(channel="chrome" if args.browser == "chrome" else None)
        page = browser.new_page(viewport=VIEW)

        # 1. one screenshot per page; each must render its heading (doubles as a smoke test)
        for name, path, ready in PAGES:
            page.goto(args.url + path)
            page.locator(ready).first.wait_for(timeout=10_000)
            page.wait_for_timeout(1500)
            page.screenshot(path=str(OUT / f"{name}.png"))
            print(f"wrote {OUT / name}.png")
        page.goto(args.url + "/captures?data=sim")
        page.locator(".tbl tbody tr").first.wait_for(timeout=10_000)
        page.locator(".tbl tbody tr").first.click()
        page.wait_for_url("**/capture/**")
        page.wait_for_timeout(1500)
        page.set_viewport_size({"width": VIEW["width"], "height": 1400})
        page.wait_for_timeout(500)
        page.screenshot(path=str(OUT / "capture-detail.png"))
        page.set_viewport_size(VIEW)
        print(f"wrote {OUT / 'capture-detail.png'}")

        # 2. GIF: a photo arriving live over WiFi, relayed by nodes 3 and 4
        page.goto(args.url + "/?data=sim")
        page.wait_for_timeout(2000)
        t = scenario(args.url, "loss", 5)
        frames = record(page, 6.5)
        t.join()
        frames += record(page, 2.0)
        save_gif(frames, "live-transfer.gif")

        # 3. GIF: the Tests page running the live pipeline check
        page.goto(args.url + "/tests")
        page.get_by_role("heading", name="Tests").first.wait_for(timeout=10_000)
        page.wait_for_timeout(1000)
        frames = [shot(page)] * 4
        page.locator(".suite", has_text="Live pipeline check").get_by_role("button", name="Run").click()
        frames += record(page, 2.0)
        page.locator(".pill", has_text="Passed").first.wait_for(timeout=60_000)
        page.locator(".checks").scroll_into_view_if_needed()
        page.wait_for_timeout(800)
        frames += record(page, 1.5)
        save_gif(frames, "tests-page.gif")
        page.set_viewport_size({"width": VIEW["width"], "height": 1500})  # whole page, header at the top
        page.evaluate("window.scrollTo(0, 0)")
        page.wait_for_timeout(500)
        page.screenshot(path=str(OUT / "tests.png"))
        print(f"wrote {OUT / 'tests.png'}")

        browser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
