"""
suite_runner.py - runs the project's own test suites from the dashboard's Tests page.

Only the fixed suites listed in SUITES can run. The page cannot pass a command, a path or an
argument: it names a suite id and this module runs that suite's fixed steps. One run at a time.

    pipeline   in-process: plays three simulated transfers through the real parser, then checks
               what the backend stored (radio choice, CRC, NACK and resend, relays, SIMULATED tag)
    backend    pytest backend/tests
    host       CMake build of host/ and ctest: ALFP, mesh filtering, uplink, audio features and the
               real lora_mesh.c / wifi_mesh.c over a fake radio with 20% loss
    style      tools/check_style.sh: BARR-C:2018 layout, source rules, warnings-as-errors compile
    all        tools/run_all_tests.sh

The routes in app.py accept requests from this machine only (see app.require_local).
"""
from __future__ import annotations

import asyncio
import contextlib
import io
import itertools
import os
import shutil
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

ROOT = Path(__file__).resolve().parents[1]
MAX_LINES = 4000
HOST_BUILD_DIR = Path(tempfile.gettempdir()) / "smartrelay-host-tests"


@dataclass
class Step:
    label: str                                    # what the page shows for this step
    argv: Optional[List[str]] = None              # a fixed command, run from the repo root
    check: Optional[Callable[["Run"], bool]] = None  # an in-process check (pipeline suite)


@dataclass
class Suite:
    id: str
    name: str
    proves: str
    needs: List[str]
    steps: Callable[[], List[Step]]
    duration: str


@dataclass
class Run:
    id: int
    suite: str
    started_at: float
    status: str = "running"                       # running, passed, failed, cancelled, error
    finished_at: Optional[float] = None
    exit_code: Optional[int] = None
    step: str = ""
    lines: List[str] = field(default_factory=list)
    checks: List[Dict[str, Any]] = field(default_factory=list)
    summary: str = ""

    def out(self, line: str) -> None:
        self.lines.append(line.rstrip("\n"))
        if len(self.lines) > MAX_LINES:
            del self.lines[: len(self.lines) - MAX_LINES]

    def view(self, after: int = 0) -> Dict[str, Any]:
        total = len(self.lines)
        return {
            "id": self.id, "suite": self.suite, "status": self.status, "step": self.step,
            "started_at": self.started_at, "finished_at": self.finished_at, "exit_code": self.exit_code,
            "summary": self.summary, "checks": self.checks, "line_count": total,
            "lines": self.lines[after:] if after < total else [],
        }


def _py() -> List[str]:
    return [sys.executable]


def _host_steps() -> List[Step]:
    b = str(HOST_BUILD_DIR)
    return [
        Step("cmake -S host -B <temp>/smartrelay-host-tests", ["cmake", "-S", "host", "-B", b]),
        Step("cmake --build <temp>/smartrelay-host-tests -j", ["cmake", "--build", b, "-j"]),
        Step("ctest --test-dir <temp>/smartrelay-host-tests --output-on-failure",
             ["ctest", "--test-dir", b, "--output-on-failure"]),
    ]


SUITES: Dict[str, Suite] = {s.id: s for s in [
    Suite("pipeline", "Live pipeline check (simulated)",
          "Three transfers go through the same parser as the real bridge and are checked in the database: a "
          "file under 1024 B uses LoRa, a photo uses WiFi relayed by nodes 3 and 4, a lost fragment is "
          "NACKed and resent, and every file passes CRC-32 on the laptop. The captures appear in the "
          "dashboard tagged SIMULATED.",
          [], lambda: [Step("play and check three transfers", check=lambda r: pipeline_check(r))], "about 10 s"),
    Suite("backend", "Backend and dashboard API",
          "pytest over backend/tests: parsing the bridge's serial text, joining telemetry with photos, "
          "transfer events, filters, summary, export and the per-file table, each on a throwaway database.",
          ["pytest"], lambda: [Step("python -m pytest -q backend/tests", _py() + ["-m", "pytest", "-q", "backend/tests"])],
          "about 5 s"),
    Suite("host", "Firmware logic on the laptop (C)",
          "The firmware's own C code built for the laptop: packet formats and CRCs, ALFP transfers under loss "
          "and corruption, duplicate and hop filtering, the USB uplink, audio features, and the real "
          "lora_mesh.c and wifi_mesh.c relaying across processes with 20% loss.",
          ["cmake", "cc"], _host_steps, "1 to 2 min the first time"),
    Suite("style", "BARR-C:2018 coding standard",
          "tools/check_style.sh: clang-format layout, the source-level BARR-C rules (tools/check_barrc.py) "
          "and a warnings-as-errors compile.",
          ["clang-format", "cc"], lambda: [Step("bash tools/check_style.sh", ["bash", "tools/check_style.sh"])],
          "about 30 s"),
    Suite("all", "Everything (tools/run_all_tests.sh)",
          "Style check, host tests, mesh integration, backend tests and ML tests, with one summary table. "
          "A stage whose tool is missing is reported SKIPPED, never as passed.",
          ["cc"], lambda: [Step("bash tools/run_all_tests.sh", ["bash", "tools/run_all_tests.sh"])], "2 to 4 min"),
]}


def _has(tool: str) -> bool:
    if tool == "pytest":
        import importlib.util
        return importlib.util.find_spec("pytest") is not None
    if tool == "cc":
        return any(shutil.which(c) for c in ("cc", "gcc", "clang"))
    return shutil.which(tool) is not None


class SuiteRunner:
    def __init__(self, store, feed_line: Callable[[str, bool], None]) -> None:
        self.store = store
        self.feed_line = feed_line
        self.runs: Dict[int, Run] = {}
        self.last: Dict[str, int] = {}
        self.current: Optional[Run] = None
        self._ids = itertools.count(1)
        self._proc: Optional[asyncio.subprocess.Process] = None
        self._cancel = False

    def suites(self) -> List[Dict[str, Any]]:
        out = []
        for s in SUITES.values():
            missing = [t for t in s.needs if not _has(t)]
            last = self.runs.get(self.last.get(s.id, -1))
            out.append({
                "id": s.id, "name": s.name, "proves": s.proves, "duration": s.duration,
                "commands": [st.label for st in s.steps()],
                "available": not missing, "missing": missing,
                "last": None if last is None else {k: v for k, v in last.view(len(last.lines)).items() if k != "lines"},
            })
        return out

    def start(self, suite_id: str) -> Run:
        if suite_id not in SUITES:
            raise KeyError(suite_id)
        if self.current is not None and self.current.status == "running":
            raise RuntimeError("a test run is already in progress")
        run = Run(id=next(self._ids), suite=suite_id, started_at=time.time())
        self.runs[run.id] = run
        self.last[suite_id] = run.id
        for old in sorted(self.runs)[:-20]:
            del self.runs[old]
        self.current = run
        self._cancel = False
        asyncio.get_running_loop().create_task(self._execute(run))
        return run

    def cancel(self) -> bool:
        if self.current is None or self.current.status != "running":
            return False
        self._cancel = True
        if self._proc is not None and self._proc.returncode is None:
            self._proc.terminate()
        return True

    async def _execute(self, run: Run) -> None:
        suite = SUITES[run.suite]
        missing = [t for t in suite.needs if not _has(t)]
        ok = True
        try:
            if missing:
                run.out(f"Cannot run: {', '.join(missing)} not installed on this laptop.")
                run.status, run.summary = "error", "missing " + ", ".join(missing)
                return
            for step in suite.steps():
                if self._cancel:
                    break
                run.step = step.label
                run.out(f"$ {step.label}")
                if step.check is not None:
                    ok = await asyncio.to_thread(step.check, run)
                else:
                    ok = await self._command(run, step.argv)
                if not ok:
                    break
            if self._cancel:
                run.status, run.summary = "cancelled", "cancelled"
            else:
                run.status = "passed" if ok else "failed"
                run.summary = run.summary or _summary(run.lines, ok)
        except Exception as exc:  # report it on the page rather than leave the run "running"
            run.out(f"runner error: {exc!r}")
            run.status, run.summary = "error", str(exc)
        finally:
            run.finished_at = time.time()
            run.out(f"--- {run.status.upper()} in {run.finished_at - run.started_at:.1f} s ---")
            self._proc = None

    async def _command(self, run: Run, argv: List[str]) -> bool:
        env = {**os.environ, "PYTHONUNBUFFERED": "1", "NO_COLOR": "1"}
        # the suites must never touch the live database or grab the bridge's serial port
        env.pop("SMARTRELAY_DB", None)
        env["SMARTRELAY_BRIDGE_PORT"] = ""
        env["PATH"] = str(Path(sys.executable).parent) + os.pathsep + env.get("PATH", "")
        self._proc = await asyncio.create_subprocess_exec(
            *argv, cwd=str(ROOT), env=env, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
        assert self._proc.stdout is not None
        async for raw in self._proc.stdout:
            run.out(raw.decode("utf-8", "replace"))
        run.exit_code = await self._proc.wait()
        return run.exit_code == 0


def _summary(lines: List[str], ok: bool) -> str:
    """The most useful closing line a tool printed: pytest's count, ctest's percentage, or the
    run_all_tests.sh verdict."""
    keys = ("passed", "failed", "tests passed", "No stage failed", "stage(s) FAILED", "OK")
    for line in reversed(lines[-60:]):
        s = line.strip().strip("=").strip()
        if s and any(k in s for k in keys) and not s.startswith("$"):
            return s[:160]
    return "passed" if ok else "failed"


# ---------------------------------------------------------------------------
# Pipeline check: simulator scenarios through the real parser, then assertions on the database
# ---------------------------------------------------------------------------

def _load_simulator():
    sim_dir = str(ROOT / "simulator")
    if sim_dir not in sys.path:
        sys.path.insert(0, sim_dir)
    import demo_feed  # noqa: WPS433 (imported here so the backend starts without it)
    return demo_feed


def pipeline_check(run: Run, feed_line: Optional[Callable[[str, bool], None]] = None) -> bool:
    runner = _ACTIVE
    feed = feed_line or runner.feed_line
    store = runner.store
    sim = _load_simulator()

    def send(_backend: str, lines: List[str], quiet: bool = False) -> None:
        for line in lines:
            feed(line, True)

    original_send = sim.send
    sim.send = send
    start_id = _max_transfer_id(store)
    try:
        for name in ("lora", "normal", "loss"):
            run.out(f"scenario '{name}' (node 5): playing @EVT lines and the @ALFP file stream")
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed):  # the simulator's one-line summary of the transfer
                sim.run_scenario(name, "in-process", 5)
            for line in printed.getvalue().splitlines():
                run.out("  " + line)
    finally:
        sim.send = original_send

    rows = _new_demo_transfers(store, start_id)
    run.out(f"{len(rows)} new simulated transfer(s) stored")
    by_radio = {"lora": [r for r in rows if r["radio"] == "lora"], "wifi": [r for r in rows if r["radio"] == "wifi"]}
    lora = by_radio["lora"][0] if by_radio["lora"] else None
    wifi = by_radio["wifi"]
    clean = next((r for r in wifi if not r["nacks"]), None)
    lossy = next((r for r in wifi if r["nacks"]), None)
    resent = store.fragments_for(lossy["id"]) if lossy else []
    vias = {f["via"] for f in store.fragments_for(clean["id"])} if clean else set()

    checks = [
        ("Three transfers stored", len(rows) == 3, f"{len(rows)} stored"),
        ("File under 1024 B went over LoRa (size rule)", bool(lora and lora["size_bytes"] < 1024),
         f"{lora['size_bytes']} B via LoRa" if lora else "no LoRa transfer"),
        ("Photo of 1024 B or more went over WiFi", bool(clean and clean["size_bytes"] >= 1024),
         f"{clean['size_bytes']} B via WiFi" if clean else "no WiFi transfer"),
        ("Photo relayed over 2 hops by nodes 3 and 4", bool(clean and clean["hops_max"] == 2 and vias <= {3, 4}
                                                            and vias),
         f"hops {clean['hops_max']}, via {sorted(vias)}" if clean else "-"),
        ("Lost fragment NACKed and resent (selective repeat)",
         bool(lossy and lossy["nacks"] >= 1 and any((f["round"] or 0) >= 1 for f in resent)),
         f"{lossy['nacks']} NACK, {sum(1 for f in resent if (f['round'] or 0) >= 1)} fragment(s) resent"
         if lossy else "no lossy transfer"),
        ("Every file verified: CRC-32 recomputed on the laptop", bool(rows) and all(r["crc_laptop_ok"] == 1
                                                                                     for r in rows),
         ", ".join(f"#{r['id']} {'ok' if r['crc_laptop_ok'] == 1 else 'FAIL'}" for r in rows)),
        ("Every transfer finished as verified", bool(rows) and all(r["status"] == "verified" for r in rows),
         ", ".join(r["status"] or "?" for r in rows)),
        ("Each became a capture tagged SIMULATED", bool(rows) and all(r["capture_row_id"] and r["is_demo"] == 1
                                                                       for r in rows),
         ", ".join(f"capture {r['capture_row_id']}" for r in rows)),
    ]
    ok = True
    for name, passed, detail in checks:
        run.checks.append({"name": name, "passed": passed, "detail": detail})
        run.out(f"{'PASS' if passed else 'FAIL'}  {name}  ({detail})")
        ok = ok and passed
    run.summary = f"{sum(1 for c in run.checks if c['passed'])} of {len(run.checks)} checks passed"
    return ok


def _max_transfer_id(store) -> int:
    newest = store.list_transfers(limit=1, mode="both")
    return int(newest[0]["id"]) if newest else 0


def _new_demo_transfers(store, after_id: int) -> List[Dict[str, Any]]:
    rows = store.list_transfers(limit=50, mode="sim")
    return sorted((r for r in rows if r["id"] > after_id), key=lambda r: r["id"])


_ACTIVE: Optional[SuiteRunner] = None


def make_runner(store, feed_line: Callable[[str, bool], None]) -> SuiteRunner:
    global _ACTIVE
    _ACTIVE = SuiteRunner(store, feed_line)
    return _ACTIVE
