#!/usr/bin/env python3
"""
smp_smoke_check.py - CURVEBALL round 3, Task C2: turn a captured smp_smoke serial log into a
one-page PASS/FAIL summary.

    python3 tools/smp_smoke_check.py path/to/serial.log
    (or pipe it: tools/serial_log.py --port COM5 --test smp_smoke | python3 tools/smp_smoke_check.py -)

Looks for the "PASS <check>: ..." / "FAIL <check>: ..." lines smp_smoke.c prints (one per check)
and the final "SMOKE done: ..." line. Exits non-zero if any check failed or the SMOKE line never
appeared (the run did not finish, e.g. it crashed or hung).
"""
import re
import sys

LINE_RE = re.compile(r"^(PASS|FAIL)\s+(\S+):\s*(.*)$")


def check_log(lines):
    results = []  # (name, passed, detail)
    seen_names = set()
    smoke_done = False
    for raw in lines:
        line = raw.rstrip("\r\n")
        m = LINE_RE.match(line)
        if m:
            status, name, detail = m.groups()
            results.append((name, status == "PASS", detail))
            seen_names.add(name)
        elif line.startswith("SMOKE "):
            smoke_done = True
    return results, smoke_done


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    src = sys.stdin if sys.argv[1] == "-" else open(sys.argv[1], "r", errors="replace")
    with src:
        lines = src.readlines()

    results, smoke_done = check_log(lines)

    if not results:
        print("No PASS/FAIL lines found at all -- is this really an smp_smoke log?")
        return 2

    print(f"{'CHECK':<28} {'RESULT':<6} DETAIL")
    print("-" * 80)
    n_fail = 0
    for name, passed, detail in results:
        print(f"{name:<28} {'PASS' if passed else 'FAIL':<6} {detail}")
        if not passed:
            n_fail += 1

    print("-" * 80)
    print(f"{len(results)} checks, {n_fail} failed, run {'completed' if smoke_done else 'DID NOT COMPLETE'}")

    if n_fail or not smoke_done:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
