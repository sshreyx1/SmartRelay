#!/usr/bin/env python3
"""
train.py - train the two-stage on-device sound classifier (SOUND_PLAN.md A5). Deterministic.

  stage 1: group (AMBIENT, BIRD, MAMMAL, INSECT_FROG, HUMAN, VEHICLE, WEATHER)
  stage 2: one model per animal group (BIRD 4 labels, MAMMAL 5, INSECT_FROG 3), trained on that group only
  rejection: a stage-2 label is kept only if its confidence >= SOUND_STAGE2_MIN_CONF, else the group id

Splits by split_key (train_config.yaml): select models on train -> val, pick the rejection threshold on val,
then refit the chosen configurations on train + val and report once on test. Selection rule: per model, the
smallest (total nodes) candidate within select_within macro-F1 of the best. All metrics are computed with the
quantised model (qmodel.py), i.e. exactly what the node outputs.

Outputs: ml/models/model_<phase>.pkl, ml/models/metrics_<phase>.json, docs/sound/model_card.md,
docs/sound/figures/confusion_{group,label}_test.png. Next: python ml/export_c.py --vectors

Run:      python ml/train.py
          python ml/train.py --features data/features_esc50_direct.csv --grid fast --note "BACKUP MODEL ..."
"""
import argparse
import json
import pickle
import sys
from datetime import date
from pathlib import Path

import numpy as np

from mlcommon import ML_DIR, REPO_ROOT, rel, resolve, sc
from qmodel import QModel, TwoStage

FIG_DIR = REPO_ROOT / "docs" / "sound" / "figures"
GROUP_IDS = sc.GROUP_IDS
ANIMAL_GROUPS = sc.ANIMAL_GROUPS


def candidates(grid: str, rs: int) -> list:
    from sklearn.ensemble import ExtraTreesClassifier, RandomForestClassifier
    from sklearn.tree import DecisionTreeClassifier
    out = []
    if grid == "fast":
        for d in (6, 8):
            out.append((f"DT d={d} leaf=2", lambda d=d: DecisionTreeClassifier(
                max_depth=d, min_samples_leaf=2, class_weight="balanced", random_state=rs)))
        out.append(("RF n=10 d=8", lambda: RandomForestClassifier(
            n_estimators=10, max_depth=8, min_samples_leaf=2, class_weight="balanced", random_state=rs)))
        return out
    for d in (4, 6, 8, 10):
        for leaf in (2, 5):
            out.append((f"DT d={d} leaf={leaf}", lambda d=d, leaf=leaf: DecisionTreeClassifier(
                max_depth=d, min_samples_leaf=leaf, class_weight="balanced", random_state=rs)))
    for name, cls in (("RF", RandomForestClassifier), ("ET", ExtraTreesClassifier)):
        for n in (5, 10, 20):
            for d in (6, 8):
                out.append((f"{name} n={n} d={d}", lambda cls=cls, n=n, d=d: cls(
                    n_estimators=n, max_depth=d, min_samples_leaf=2, class_weight="balanced", random_state=rs)))
    return out


def macro_f1(y, p) -> float:
    from sklearn.metrics import f1_score
    return float(f1_score(y, p, average="macro", zero_division=0))


def select(stage: str, cands: list, Xtr, ytr, Xva, yva, within: float) -> tuple:
    """Fit every candidate on train, score on val; return (chosen row, table rows)."""
    rows = []
    for name, make in cands:
        est = make().fit(Xtr, ytr)
        q = QModel.from_sklearn(est)
        f1 = macro_f1(yva, q.predict(Xva)[0]) if len(yva) else 0.0
        rows.append({"stage": stage, "model": name, "val_macro_f1": round(f1, 4), "nodes": q.n_nodes,
                     "flash_bytes": q.flash_bytes(), "make": make})
    best = max(r["val_macro_f1"] for r in rows)
    ok = [r for r in rows if r["val_macro_f1"] >= best - within]
    chosen = min(ok, key=lambda r: (r["nodes"], -r["val_macro_f1"]))
    for r in rows:
        r["chosen"] = r is chosen
    return chosen, rows


def pick_threshold(model: TwoStage, X, y_label, max_wrong: float) -> tuple:
    """Smallest min_conf where wrong stage-2 labels are <= max_wrong of those emitted (val)."""
    _, _, g, l2, c2 = model.predict(X)
    has2 = l2 >= 0
    curve = []
    for t in range(0, 102):
        emit = has2 & (c2 >= t)
        n = int(emit.sum())
        wrong = int((emit & (l2 != y_label)).sum())
        curve.append({"t": t, "emitted": n, "wrong": wrong, "wrong_share": wrong / n if n else 0.0})
    for row in curve:
        if row["emitted"] and row["wrong_share"] <= max_wrong:
            return row["t"], curve
    return 101, curve  # never emit a stage-2 label


def confusion_png(y, p, ids: list, title: str, path: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from sklearn.metrics import ConfusionMatrixDisplay, confusion_matrix
    cm = confusion_matrix(y, p, labels=ids)
    size = max(5.0, 0.42 * len(ids) + 2)
    fig, ax = plt.subplots(figsize=(size, size))
    ConfusionMatrixDisplay(cm, display_labels=[sc.name(i) for i in ids]).plot(
        ax=ax, xticks_rotation=60, colorbar=False, values_format="d")
    ax.set_title(title, fontsize=10)
    fig.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=110)
    plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser(description="Train the two-stage on-device sound classifier.")
    ap.add_argument("--config", type=Path, default=ML_DIR / "train_config.yaml")
    ap.add_argument("--features", nargs="+", help="features CSV(s) (overrides the config); several are concatenated, "
                                                  "e.g. mic recordings + the esc50_direct backup")
    ap.add_argument("--grid", choices=["full", "fast"], help="candidate grid (overrides the config)")
    ap.add_argument("--note", help="data note for the model card (overrides the config)")
    args = ap.parse_args()

    import pandas as pd
    import yaml
    from sklearn.metrics import accuracy_score

    with open(args.config, encoding="utf-8") as f:
        cfg = yaml.safe_load(f)
    for k in ("features", "grid", "note"):
        if getattr(args, k):
            cfg["data_note" if k == "note" else k] = getattr(args, k)
    phase, rs = cfg["phase"], int(cfg["random_state"])
    paths = [resolve(f) for f in (cfg["features"] if isinstance(cfg["features"], list) else [cfg["features"]])]
    for fp in paths:
        if not fp.exists():
            print(f"error: {fp} not found", file=sys.stderr)
            return 1
    df = pd.concat([pd.read_csv(fp) for fp in paths], ignore_index=True)
    feat_label = " + ".join(rel(fp) for fp in paths)
    feats = [c for c in df.columns if c.startswith(("band_", "centroid", "zcr", "loud", "active", "flux", "log_rms"))]
    X = df[feats].to_numpy(np.float32)
    yg = df["group_id"].to_numpy(int)
    yl = df["label_id"].to_numpy(int)
    key = df["split_key"].astype(int)
    tr, va, te = (key.isin(cfg[k]).to_numpy() for k in ("train_keys", "val_keys", "test_keys"))
    trva = tr | va
    cands = candidates(cfg["grid"], rs)
    print(f"{len(df)} takes, {len(feats)} features | train {tr.sum()} val {va.sum()} test {te.sum()} | "
          f"grid {cfg['grid']} ({len(cands)} candidates)")

    # ---- selection on train -> val ----
    table = []
    ch1, rows = select("stage1 group", cands, X[tr], yg[tr], X[va], yg[va], cfg["select_within"])
    table += rows
    chosen = {"stage1": ch1}
    for g in ANIMAL_GROUPS:
        m_tr, m_va = tr & (yg == g), va & (yg == g)
        ch, rows = select(f"stage2 {sc.name(g)}", cands, X[m_tr], yl[m_tr], X[m_va], yl[m_va], cfg["select_within"])
        table += rows
        chosen[g] = ch
    for k, c in chosen.items():
        print(f"  chosen {c['stage']:<18} {c['model']:<16} val F1 {c['val_macro_f1']:.3f}  nodes {c['nodes']}")

    # ---- rejection threshold on val (models fitted on train) ----
    sel = TwoStage(QModel.from_sklearn(chosen["stage1"]["make"]().fit(X[tr], yg[tr])),
                   {g: QModel.from_sklearn(chosen[g]["make"]().fit(X[tr & (yg == g)], yl[tr & (yg == g)]))
                    for g in ANIMAL_GROUPS}, 0)
    min_conf, curve = pick_threshold(sel, X[va], yl[va], cfg["max_wrong_share"])
    print(f"  SOUND_STAGE2_MIN_CONF = {min_conf} (val: wrong stage-2 labels <= {cfg['max_wrong_share']:.0%})")

    # ---- refit on train + val, report on test ----
    final = TwoStage(QModel.from_sklearn(chosen["stage1"]["make"]().fit(X[trva], yg[trva])),
                     {g: QModel.from_sklearn(chosen[g]["make"]().fit(X[trva & (yg == g)], yl[trva & (yg == g)]))
                      for g in ANIMAL_GROUPS}, min_conf)
    label, conf, g1, l2, c2 = final.predict(X[te])
    out_group = np.array([sc.group(int(i)) for i in label])
    yt_g, yt_l = yg[te], yl[te]
    animal_pred = np.isin(g1, ANIMAL_GROUPS)
    finer = np.array([sc.get(int(i)).kind != sc.KIND_GROUP for i in label])
    sk_agree = float(np.mean(final.stage1.est.predict(X[te]) == g1)) if te.any() else 0.0
    flash = final.stage1.flash_bytes() + sum(m.flash_bytes() for m in final.stage2.values())

    # legacy mic.c thresholds, animal vs not-animal (class 1 LOUD_ANIMAL_CALL = "animal")
    legacy = None
    if "legacy_energy" in df:
        legacy_animal = df.loc[te, "legacy_energy"].to_numpy() > 50000.0
        legacy = round(float(accuracy_score(np.isin(yt_g, ANIMAL_GROUPS), legacy_animal)), 4)
    new_animal_acc = round(float(accuracy_score(np.isin(yt_g, ANIMAL_GROUPS), np.isin(out_group, ANIMAL_GROUPS))), 4)

    metrics = {
        "phase": phase, "date": date.today().isoformat(), "features_csv": feat_label, "grid": cfg["grid"],
        "data_note": cfg.get("data_note", ""), "n_takes": int(len(df)),
        "n_train_val": int(trva.sum()), "n_test": int(te.sum()), "feature_names": feats,
        "stage2_min_conf": int(min_conf),
        "test": {
            "group_accuracy": round(float(accuracy_score(yt_g, out_group)), 4),
            "group_macro_f1": round(macro_f1(yt_g, out_group), 4),
            "label_accuracy": round(float(accuracy_score(yt_l, label)), 4),
            "label_macro_f1": round(macro_f1(yt_l, label), 4),
            "coverage": round(float(finer[animal_pred].mean()) if animal_pred.any() else 0.0, 4),
            "finer_label_precision": round(float((label[finer] == yt_l[finer]).mean()) if finer.any() else 0.0, 4),
            "animal_vs_not_accuracy": new_animal_acc,
            "legacy_animal_vs_not_accuracy": legacy,
            "stage1_quantised_vs_sklearn_agreement": round(sk_agree, 4),
        },
        "chosen": {str(k): {"model": c["model"], "val_macro_f1": c["val_macro_f1"], "nodes": c["nodes"]}
                   for k, c in chosen.items()},
        "model_flash_bytes": int(flash),
        "candidates": [{k: v for k, v in r.items() if k != "make"} for r in table],
        "rejection_curve_val": curve,
    }
    t = metrics["test"]
    print(f"  test: group acc {t['group_accuracy']:.3f} F1 {t['group_macro_f1']:.3f} | label acc "
          f"{t['label_accuracy']:.3f} F1 {t['label_macro_f1']:.3f} | coverage {t['coverage']:.2f} | "
          f"animal-vs-not {new_animal_acc:.3f} (legacy {legacy}) | flash {flash} B")
    if flash > cfg["flash_budget"]:
        print(f"WARNING: models need {flash} B of flash, budget {cfg['flash_budget']}")
    if t["group_macro_f1"] < 0.6:
        print("STOP RULE: test group macro-F1 < 0.6, discuss with R before A6 (SOUND_PLAN.md A5)")

    models = ML_DIR / "models"
    models.mkdir(exist_ok=True)
    with open(models / f"model_{phase}.pkl", "wb") as f:
        pickle.dump({"model": final, "features": feats, "metrics": metrics,
                     "test_rows": np.where(te)[0].tolist(), "features_csv": [rel(fp) for fp in paths]}, f)
    with open(models / f"metrics_{phase}.json", "w", encoding="utf-8", newline="\n") as f:
        json.dump(metrics, f, indent=1)
    confusion_png(yt_g, out_group, GROUP_IDS, f"Group, test fold (n={len(yt_g)})", FIG_DIR / "confusion_group_test.png")
    ids = sorted(set(yt_l) | set(label.tolist()))
    confusion_png(yt_l, label, ids, f"Label, test fold (n={len(yt_l)})", FIG_DIR / "confusion_label_test.png")
    write_model_card(metrics, df, feats, cfg)
    print(f"wrote ml/models/model_{phase}.pkl, metrics_{phase}.json, docs/sound/model_card.md, figures")
    return 0


def write_model_card(m: dict, df, feats: list, cfg: dict) -> None:
    t = m["test"]
    lines = [f"# Sound classifier model card (Phase {m['phase']})", ""]
    if m["data_note"]:
        lines += [f"> **{m['data_note']}**", ""]
    lines += [
        f"Generated by `ml/train.py` on {m['date']} from `{m['features_csv']}` (grid: {m['grid']}). Do not edit by hand.",
        "",
        "## What it does",
        "",
        "Runs on the Pico after every capture: 25 streaming spectral features (`firmware/sensing/sound_features.c`, "
        "v1), then **stage 1** picks the sound group and, for an animal group, **stage 2** picks the animal. "
        f"A stage-2 label is kept only when its confidence is at least **{m['stage2_min_conf']}%** "
        "(`SOUND_STAGE2_MIN_CONF`); otherwise the node reports just the group. The backend only displays the result.",
        "",
        "## Data and split",
        "",
        f"{m['n_takes']} takes; split by `split_key` (ESC-50 fold): folds 1-3 train, 4 validation, 5 test. Every take "
        "of one source clip stays in one split. Final models are refitted on folds 1-4; test is used once.",
        "",
        "| label | group | train | val | test |",
        "|---|---|---:|---:|---:|",
    ]
    key = df["split_key"].astype(int)
    for lid in sorted(df["label_id"].unique()):
        d = key[df["label_id"] == lid]
        lines.append(f"| {sc.name(int(lid))} | {sc.group_name(int(lid))} | {int(d.isin(cfg['train_keys']).sum())} | "
                     f"{int(d.isin(cfg['val_keys']).sum())} | {int(d.isin(cfg['test_keys']).sum())} |")
    lines += ["", "## Candidates (macro-F1 on validation; chosen = smallest within "
              f"{cfg['select_within']} of the best)", "", "| stage | model | val macro-F1 | nodes | flash B | chosen |",
              "|---|---|---:|---:|---:|:-:|"]
    for r in m["candidates"]:
        lines.append(f"| {r['stage']} | {r['model']} | {r['val_macro_f1']:.3f} | {r['nodes']} | {r['flash_bytes']} | "
                     f"{'✓' if r['chosen'] else ''} |")
    lines += [
        "", "## Test results (quantised model = what the node computes)", "",
        "| metric | value |", "|---|---:|",
        f"| group accuracy | {t['group_accuracy']:.3f} |",
        f"| group macro-F1 | {t['group_macro_f1']:.3f} |",
        f"| label accuracy | {t['label_accuracy']:.3f} |",
        f"| label macro-F1 | {t['label_macro_f1']:.3f} |",
        f"| coverage (animal-group captures that got an animal label) | {t['coverage']:.3f} |",
        f"| precision of the animal labels emitted | {t['finer_label_precision']:.3f} |",
        f"| animal vs not-animal accuracy | {t['animal_vs_not_accuracy']:.3f} |",
        f"| same, legacy mic.c thresholds (before) | {t['legacy_animal_vs_not_accuracy']} |",
        f"| stage 1 quantised vs sklearn agreement | {t['stage1_quantised_vs_sklearn_agreement']:.3f} |",
        f"| model flash (all stages) | {m['model_flash_bytes']} B |",
        "", "Confusion matrices: `docs/sound/figures/confusion_group_test.png`, `confusion_label_test.png`.",
        "", "## Limitations", "",
        "- ESC-50 is not Singapore wildlife: these are farm and pet animals and generic birds/insects (Phase B fixes this).",
        "- Played through a speaker in a room is not the field: distance, wind and overlapping calls are not covered.",
        "- The MAX9814's AGC rescales everything, so loudness features (`loud_*`, `log_rms`) carry little information.",
        "- ESC-50 is licensed CC BY-NC 3.0 (Piczak 2015); fine for coursework, not for a commercial product.",
    ]
    if "BACKUP" in (m["data_note"] or "").upper():
        lines[-4:-4] = [
            "- **This is the backup model.** The clips were converted to ADC-like numbers in software "
            "(`ml/esc50_direct.py`), not recorded through the Pico mic. "
            "Expect lower accuracy on the real node until it is retrained on mic recordings.",
            "- **Known limitation (DECISIONS D3): AMBIENT is synthetic noise** (white, filtered fan, 50 Hz hum, pink-ish "
            "drift), because ESC-50 has no ambient class. It is trivially separable, so the AMBIENT row and the group "
            "metrics above are flattered; real quiet rooms will be confused with WEATHER/INSECT more often.",
        ]
    path = REPO_ROOT / "docs" / "sound" / "model_card.md"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    sys.exit(main())
