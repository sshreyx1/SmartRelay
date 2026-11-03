"""
mlcommon.py - small helpers shared by the ml/ scripts: repo paths, the label table import,
the label map, and CSV reading/writing with the encoding rules from SOUND_PLAN.md rule 12.
"""
import csv
import sys
from pathlib import Path

ML_DIR = Path(__file__).resolve().parent
REPO_ROOT = ML_DIR.parent
DATA_DIR = ML_DIR / "data"

sys.path.insert(0, str(REPO_ROOT / "common"))
import sound_classes as sc  # noqa: E402,F401  (re-exported for the scripts)


def rel(path: Path) -> str:
    """Path relative to ml/, with forward slashes, so CSVs work on macOS and Windows."""
    p = Path(path).resolve()
    try:
        return p.relative_to(ML_DIR).as_posix()
    except ValueError:
        return p.as_posix()


def resolve(path_str: str) -> Path:
    """Inverse of rel(): a CSV path back to a real path on this machine."""
    p = Path(path_str)
    return p if p.is_absolute() else ML_DIR / p


def load_label_map(path: Path = ML_DIR / "label_map.yaml") -> dict:
    """ESC-50 category -> SoundClass."""
    import yaml
    with open(path, encoding="utf-8") as f:
        cats = yaml.safe_load(f)["categories"]
    return {cat: sc.by_name(label) for cat, label in cats.items()}


def read_csv(path: Path) -> list:
    with open(path, encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def write_csv(path: Path, rows: list, fields: list) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)


def append_csv(path: Path, row: dict, fields: list) -> None:
    new = not path.exists()
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        if new:
            w.writeheader()
        w.writerow(row)
