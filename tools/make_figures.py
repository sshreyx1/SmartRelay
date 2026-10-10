#!/usr/bin/env python3
"""
make_figures.py - turn docs/results/*.csv (from `experiments suite`) into report figures.

    python3 tools/make_figures.py            -> docs/figures/fig*.png (300 dpi, for the Word report)

Style: one blue accent on what each title is about, comparisons in grey, direct labels,
light grid, one y-axis per chart.
"""
import csv
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "docs", "results")
OUT = os.path.join(ROOT, "docs", "figures")

ACCENT = "#2a78d6"
GREY = "#8a8984"
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#e4e3df"
SHADES = ["#a9c9f0", "#6ea5e6", "#2a78d6", "#1a4f93"]  # one hue, light -> dark, for ordered series

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 10,
    "axes.edgecolor": GRID,
    "axes.labelcolor": INK2,
    "xtick.color": INK2,
    "ytick.color": INK2,
    "axes.grid": True,
    "grid.color": GRID,
    "grid.linewidth": 0.8,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "figure.dpi": 150,
})


def load(name):
    rows = list(csv.DictReader(open(os.path.join(RES, name))))
    out = {}
    for r in rows:
        out.setdefault(r["series"], []).append(r)
    return out


def xs(rows, scale=1.0):
    return [float(r["x"]) * scale for r in rows]


def col(rows, key, scale=1.0):
    return [float(r[key]) * scale if r[key] != "nan" else float("nan") for r in rows]


def title(ax, main, sub):
    ax.set_title(main, loc="left", fontsize=12, fontweight="bold", color=INK, pad=22)
    ax.text(0, 1.02, sub, transform=ax.transAxes, fontsize=9, color=INK2)


def label_end(ax, x, y, text, color, dy=0):
    ax.annotate(text, (x[-1], y[-1]), xytext=(6, dy), textcoords="offset points", color=color, fontsize=9,
                va="center", fontweight="bold" if color == ACCENT else "normal")


def save(fig, name):
    os.makedirs(OUT, exist_ok=True)
    p = os.path.join(OUT, name)
    fig.savefig(p, dpi=300, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("wrote", p)


def fig_selective():
    d = load("e4_selective_vs_full.csv")
    fig, ax = plt.subplots(figsize=(7, 3.8))
    s, f = d["lora_selective"], d["lora_resend_all"]
    ax.plot(xs(f, 100), col(f, "airtime_s"), color=GREY, lw=2, marker="o", ms=4)
    ax.plot(xs(s, 100), col(s, "airtime_s"), color=ACCENT, lw=2, marker="o", ms=4)
    label_end(ax, xs(f, 100), col(f, "airtime_s"), "resend whole file", GREY)
    label_end(ax, xs(s, 100), col(s, "airtime_s"), "ALFP selective repeat", ACCENT)
    i = 2
    ax.annotate(f"-{100 * (1 - col(s, 'airtime_s')[i] / col(f, 'airtime_s')[i]):.0f}% airtime at 10% loss",
                (xs(s, 100)[i], col(s, "airtime_s")[i]), xytext=(10, -28), textcoords="offset points",
                color=INK2, fontsize=9, arrowprops=dict(arrowstyle="-", color=INK2, lw=0.8))
    ax.set_xlabel("packet loss per hop (%)")
    ax.set_ylabel("LoRa airtime per 8 kB image (s)")
    ax.set_ylim(0, None)
    ax.set_xlim(0, 38)
    title(ax, "Selective repeat uses a third of the airtime of resending everything",
          "LoRa, 2 hops, 8 kB image, 40 simulated transfers per point")
    save(fig, "fig1_selective_vs_resend_all.png")


def fig_iterations():
    d = load("e2_design_iterations.csv")
    fig, ax = plt.subplots(figsize=(7, 3.8))
    v1, v2, fin = d["v1_first_design"], d["v2_fast_polls"], d["final"]
    ax.plot(xs(v1, 100), col(v1, "success", 100), color=GREY, lw=2, marker="o", ms=4)
    ax.plot(xs(fin, 100), col(fin, "success", 100), color=ACCENT, lw=2, marker="o", ms=4)
    label_end(ax, xs(v1, 100), col(v1, "success", 100), "first design", GREY)
    label_end(ax, xs(fin, 100), col(fin, "success", 100), "final design", ACCENT, dy=-8)
    ax.set_xlabel("packet loss per hop (%)")
    ax.set_ylabel("transfers completed first try (%)")
    ax.set_ylim(-3, 105)
    ax.set_xlim(0, 36)
    title(ax, "Faster, silence-based polling took completion at 20% loss from 45% to 100%",
          "LoRa, 2 hops, 8 kB. First design: 3 s poll timeout, gives up after 5 unanswered polls")
    save(fig, "fig2_design_iterations.png")


def fig_gap():
    d = load("e1_lora_gap.csv")
    fig, ax = plt.subplots(figsize=(7, 3.8))
    for k, hops in enumerate([1, 2, 3, 4]):
        r = d[f"lora_{hops}hop"]
        ax.plot(xs(r), col(r, "sent_per_fragment"), color=SHADES[k], lw=2, marker="o", ms=4)
        label_end(ax, xs(r), col(r, "sent_per_fragment"), f"{hops} hop{'s' if hops > 1 else ''}", SHADES[k])
    ax.axvline(500, color=INK2, lw=0.8, ls="--")
    ax.text(510, ax.get_ylim()[1] * 0.92, "chosen: 500 ms", color=INK2, fontsize=9)
    ax.set_xlabel("pause after each packet before the next (ms)")
    ax.set_ylabel("transmissions per fragment")
    ax.set_ylim(0, None)
    ax.set_xlim(0, 1500)
    title(ax, "Pausing for relays to finish cuts retransmissions by half or more past 1 hop",
          "LoRa, 10% loss per hop. Relays are half-duplex: sending too soon collides with the flood")
    save(fig, "fig3_lora_pacing.png")


def fig_radios():
    d = load("e6_size.csv")
    fig, axes = plt.subplots(1, 2, figsize=(8, 3.4))
    for ax, key, name, color in ((axes[0], "lora", "LoRa", ACCENT), (axes[1], "wifi", "WiFi (ESP-NOW)", ACCENT)):
        r = d[key]
        ax.plot(xs(r, 0.001), col(r, "t_median_s"), color=color, lw=2, marker="o", ms=4)
        ax.fill_between(xs(r, 0.001), col(r, "t_p10_s"), col(r, "t_p90_s"), color=color, alpha=0.15, lw=0)
        ax.set_title(name, loc="left", fontsize=10, color=INK, fontweight="bold")
        ax.set_xlabel("image size (kB)")
        ax.set_ylim(0, None)
    axes[0].set_ylabel("transfer time (s), median and 10-90%")
    fig.suptitle("LoRa takes about 7 s per kB, WiFi about 0.3 s per kB", x=0.02, ha="left", fontsize=12,
                 fontweight="bold", color=INK)
    fig.text(0.02, 0.9, "2 hops, 10% loss per hop. Separate y-axes: the two radios differ about 30x",
             fontsize=9, color=INK2)
    fig.tight_layout(rect=(0, 0, 1, 0.88))
    save(fig, "fig4_lora_vs_wifi_size.png")


def fig_cad():
    d = load("e3_lora_cad.csv")
    fig, ax = plt.subplots(figsize=(7, 3.8))
    off, on = d["cad_off"], d["cad_on"]
    ax.plot(xs(off, 100), col(off, "t_median_s"), color=GREY, lw=2, marker="o", ms=4)
    ax.plot(xs(on, 100), col(on, "t_median_s"), color=ACCENT, lw=2, marker="o", ms=4)
    label_end(ax, xs(off, 100), col(off, "t_median_s"), "no listen-before-talk", GREY)
    label_end(ax, xs(on, 100), col(on, "t_median_s"), "with CAD", ACCENT)
    ax.set_xlabel("packet loss per hop (%)")
    ax.set_ylabel("median transfer time (s)")
    ax.set_ylim(0, None)
    ax.set_xlim(0, 38)
    title(ax, "LoRa channel sensing (CAD) before sending cuts transfer time by about 18%",
          "LoRa, 2 hops, 8 kB. Recommendation for the LoRa mesh (Member 1)")
    save(fig, "fig5_lora_cad.png")


def fig_loss():
    d = load("e5_loss_sweep.csv")
    fig, axes = plt.subplots(1, 2, figsize=(8, 3.4), sharey=True)
    for ax, radio, name in ((axes[0], "lora", "LoRa"), (axes[1], "wifi", "WiFi (ESP-NOW)")):
        for k, hops in enumerate([1, 2, 3]):
            r = d[f"{radio}_{hops}hop"]
            ax.plot(xs(r, 100), col(r, "success", 100), color=SHADES[k + 1], lw=2, marker="o", ms=3.5)
            label_end(ax, xs(r, 100)[:6], col(r, "success", 100)[:6], "", SHADES[k + 1])
        ax.set_title(name, loc="left", fontsize=10, color=INK, fontweight="bold")
        ax.set_xlabel("packet loss per hop (%)")
        ax.set_ylim(-3, 105)
    axes[0].set_ylabel("completed first try (%)")
    handles = [plt.Line2D([], [], color=SHADES[k + 1], lw=2) for k in range(3)]
    axes[1].legend(handles, ["1 hop", "2 hops", "3 hops"], frameon=False, loc="lower left")
    fig.suptitle("Every transfer completes up to 30% loss per hop over 1-2 hops", x=0.02, ha="left", fontsize=12,
                 fontweight="bold", color=INK)
    fig.text(0.02, 0.9, "8 kB image, 40 transfers per point. A transfer that gives up stays on SD and is retried",
             fontsize=9, color=INK2)
    fig.tight_layout(rect=(0, 0, 1, 0.88))
    save(fig, "fig6_loss_sweep.png")


if __name__ == "__main__":
    fig_selective()
    fig_iterations()
    fig_gap()
    fig_radios()
    fig_cad()
    fig_loss()
