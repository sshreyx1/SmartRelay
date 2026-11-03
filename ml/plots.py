#!/usr/bin/env python3
"""
plots.py - box plots of the most informative features per sound group, for the design doc.

Ranks features by ANOVA F-score between groups and draws the top --top of them, one panel each.

Run:      python ml/plots.py
          python ml/plots.py --features ml/data/features_esc50_direct.csv --out docs/sound/figures/feature_boxplots_direct.png
"""
import argparse
import sys
from pathlib import Path

from mlcommon import DATA_DIR, REPO_ROOT, sc


def main() -> int:
    ap = argparse.ArgumentParser(description="Per-group box plots of the most informative features.")
    ap.add_argument("--features", type=Path, default=DATA_DIR / "features_v1.csv")
    ap.add_argument("--out", type=Path, default=REPO_ROOT / "docs" / "sound" / "figures" / "feature_boxplots_v1.png")
    ap.add_argument("--top", type=int, default=6)
    args = ap.parse_args()

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import pandas as pd
    from sklearn.feature_selection import f_classif

    if not args.features.exists():
        print(f"error: {args.features} not found", file=sys.stderr)
        return 1
    df = pd.read_csv(args.features)
    feats = [c for c in df.columns if c.startswith(("band_", "centroid", "zcr", "loud", "active", "flux", "log_rms"))]
    groups = df["group_id"].astype(int)
    f_scores, _ = f_classif(df[feats].values, groups.values)
    order = sorted(zip(f_scores, feats), reverse=True)[:args.top]
    gids = sorted(groups.unique())

    cols = 3
    rows = (len(order) + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(4.2 * cols, 3.2 * rows), squeeze=False)
    for ax, (score, name) in zip(axes.flat, order):
        ax.boxplot([df.loc[groups == g, name].values for g in gids], showfliers=False)
        ax.set_xticks(range(1, len(gids) + 1), [sc.name(g)[:7] for g in gids], rotation=45, fontsize=8)
        ax.set_title(f"{name}  (F={score:.0f})", fontsize=9)
    for ax in list(axes.flat)[len(order):]:
        ax.axis("off")
    fig.suptitle(f"Most informative features per group ({args.features.name}, n={len(df)})", fontsize=10)
    fig.tight_layout()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=110)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
