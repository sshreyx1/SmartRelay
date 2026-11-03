"""
capture_id.py - Python mirror of common/capture_id.h: the human-readable capture name.

    display_id(2, 42) -> "N2-000042"     (logs, backend display_id, dashboard badge, CSV)
    file_stem(2, 42)  -> "N2_000042"     (SD file names)
    parse("N2_000042_full.scl") -> (2, 42)
"""
import re
from typing import Optional, Tuple

_RE = re.compile(r"^[Nn](\d{1,3})[-_](\d{6,10})")


def display_id(node: int, capture_id: int) -> str:
    return f"N{int(node)}-{int(capture_id):06d}"


def file_stem(node: int, capture_id: int) -> str:
    return f"N{int(node)}_{int(capture_id):06d}"


def parse(s: str) -> Optional[Tuple[int, int]]:
    """(node, capture_id) from "N2-000042" / "N2_000042..." or None."""
    m = _RE.match(s or "")
    if not m:
        return None
    node, cid = int(m.group(1)), int(m.group(2))
    if node > 255 or cid > 0xFFFFFFFF:
        return None
    return node, cid
