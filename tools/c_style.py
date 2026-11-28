"""Finish a generated C file the way docs/CODING_STANDARD.md requires: clang-format layout
(.clang-format at the repository root) and the BARR-C:2018 3.3.c end-of-file comment."""
import shutil
import subprocess
from pathlib import Path

END_MARK = "/*** end of file ***/"


def _clang_format():
    exe = shutil.which("clang-format")
    if exe:
        return exe
    try:
        import clang_format  # the `pip install clang-format` package
    except ImportError:
        return None
    found = sorted(Path(clang_format.__file__).parent.glob("data/bin/clang-format*"))
    return str(found[0]) if found else None


def finish_c_file(path: Path) -> None:
    exe = _clang_format()
    if exe:
        subprocess.run([exe, "-style=file", "-i", str(path)], check=True)
    else:
        print(f"note: clang-format not found, run it on {path} before committing")
    text = path.read_text(encoding="utf-8").rstrip()
    if text.endswith(END_MARK):
        text = text[: -len(END_MARK)].rstrip()
    path.write_text(f"{text}\n\n{END_MARK}\n\n", encoding="utf-8", newline="\n")
