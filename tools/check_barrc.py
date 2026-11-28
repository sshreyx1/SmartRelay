#!/usr/bin/env python3
"""
check_barrc.py - checks the BARR-C:2018 rules that can be decided from the source text alone, over
every project C file (docs/CODING_STANDARD.md lists the scope). Layout rules (braces, spacing,
alignment) are checked by clang-format in tools/check_style.sh; this script covers the rest of the
mechanical ones. Exit code 0 = no violation.

Run:      python tools/check_barrc.py
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VENDORED = ("third_party/mtk3smp_rp2040/", "third_party/lora_mesh/src/")
# names fixed by an external API (docs/CODING_STANDARD.md, deviation D3)
FIXED_NAME_FILES = {"firmware/FreeRTOSConfig.h"}
END_MARK = "/*** end of file ***/"
TOKEN = re.compile(
    r"""//[^\n]* | /\*.*?\*/ | "(?:[^"\\\n]|[\\].)*" | '(?:[^'\\\n]|[\\].)*'""", re.S | re.X)


def project_files():
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "*.c", "*.h"], capture_output=True, text=True,
                         check=True).stdout.split()
    return [f for f in out if not f.startswith(VENDORED)]


def code_only(text: str) -> str:
    """The source with comments and string / character literals blanked, line structure kept."""
    return TOKEN.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), text)


def check(path: str):
    text = (ROOT / path).read_text(encoding="utf-8").replace("\r\n", "\n")
    for number, line in enumerate(text.split("\n"), 1):
        if len(line) > 80:
            yield "1.2.a", number, "line is longer than 80 characters"
        if "\t" in line:
            yield "3.5.a", number, "tab character"
    if not text.endswith(END_MARK + "\n\n"):
        yield "3.3.c", 0, "missing end-of-file comment followed by a blank line"
    name = path.rsplit("/", 1)[-1]
    if re.search(r"[^a-z0-9_.]", name) and path not in FIXED_NAME_FILES:
        yield "4.1.a", 0, "file name is not lower case"
    if name.endswith(".h") and not re.search(r"^#ifndef (\w+)\n#define \1\b", code_only(text), re.M):
        yield "4.2.b", 0, "no include guard"
    for number, line in enumerate(code_only(text).split("\n"), 1):
        if re.search(r"\b(short|long)\b", line) and not line.lstrip().startswith("#"):
            yield "5.2.b", number, "short / long keyword"
        if re.search(r"\b(float|double)\b", line) and path != "common/float_types.h":
            yield "5.4.b.i", number, "use float32_t / float64_t"
        if re.search(r"\b(auto|register)\s+\w", line):
            yield "1.7.a/b", number, "auto / register keyword"
        if re.search(r"\bwhile\s*\(\s*(1|1u|true)\s*\)", line) and not re.search(r"}\s*while", line):
            yield "8.4.c", number, "write infinite loops as for (;;)"
        if re.search(r"\b(setjmp|longjmp|abort)\s*\(", line):
            yield "8.5.b", number, "abort / setjmp / longjmp"


def main() -> int:
    files = project_files()
    found = [(path, number, rule, what) for path in files for rule, number, what in check(path)]
    for path, number, rule, what in found:
        print(f"{path}:{number}: BARR-C {rule}: {what}")
    print(f"check_barrc.py: {len(files)} files, {len(found)} violation(s)")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
