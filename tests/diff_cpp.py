"""Differential test: the C++ port must behave exactly like the Python
reference on the same project, with the same stub lean.

Runs both implementations over examples/two_modules (verify + build, text +
json) and compares outputs modulo timing fields. The stub lean fails every
module, which also exercises the fail-fast build path. Also runs the argparse
surface check (tests/diff_cpp_cli.py).

Usage:  python tests/diff_cpp.py
Skips (exit 0) when the C++ binary has not been built.
"""

from __future__ import annotations

import json
import os
import re
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP_BIN = ROOT / "dist" / "cpp" / ("leanoff.exe" if os.name == "nt" else "leanoff")

STUB_WIN = """@echo off
echo %~1:1:0: error: stub elaboration failure
echo %~1:2:0: warning: stub warning
echo %~1:3:0: info: declaration uses 'sorry'
exit /b 1
"""

STUB_UNIX = """#!/bin/sh
echo "$1:1:0: error: stub elaboration failure"
echo "$1:2:0: warning: stub warning"
echo "$1:3:0: info: declaration uses 'sorry'"
exit 1
"""


def make_stub(tmp: Path) -> Path:
    if os.name == "nt":
        stub = tmp / "lean.bat"
        stub.write_text(STUB_WIN, encoding="utf-8")
    else:
        stub = tmp / "lean"
        stub.write_text(STUB_UNIX, encoding="utf-8")
        stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    return stub


def strip_timings_json(rep: str) -> dict:
    data = json.loads(rep)
    for m in data.get("modules", []):
        m.pop("seconds", None)
    data.pop("wall_seconds", None)
    return data


def strip_timings_text(s: str) -> str:
    s = re.sub(r"\s+\d+\.\d+s", " Xs", s)
    s = re.sub(r"\(\d+\.\d+s wall\)", "(Xs wall)", s)
    return s


def main() -> int:
    if not CPP_BIN.exists():
        print(f"skip: {CPP_BIN} not built")
        return 0
    with tempfile.TemporaryDirectory() as td:
        stub = make_stub(Path(td))
        failures = 0
        for fmt in ("json", "text"):
            for cmd in ("verify", "build"):
                args = [cmd, "--root", str(ROOT / "examples" / "two_modules"),
                        "--lean", str(stub), "--format", fmt, "--jobs", "2"]
                py = subprocess.run([sys.executable, str(ROOT / "leanoff.py")] + args,
                                    capture_output=True, text=True)
                cpp = subprocess.run([str(CPP_BIN)] + args,
                                     capture_output=True, text=True)
                label = f"{cmd} {fmt}"
                if py.returncode != cpp.returncode:
                    print(f"FAIL {label}: exit {py.returncode} vs {cpp.returncode}")
                    print("py stderr:", py.stderr)
                    print("cpp stderr:", cpp.stderr)
                    failures += 1
                    continue
                if fmt == "json":
                    match = strip_timings_json(py.stdout) == strip_timings_json(cpp.stdout)
                else:
                    match = strip_timings_text(py.stdout) == strip_timings_text(cpp.stdout)
                print(f"{'ok  ' if match else 'FAIL'} {label}")
                if not match:
                    print("py stdout:", py.stdout)
                    print("cpp stdout:", cpp.stdout)
                    failures += 1
        if failures:
            print(f"\n{failures} mismatch(es)")
            return 1
    # argparse surface: help texts, abbreviations, error precedence
    return subprocess.run([sys.executable, str(ROOT / "tests" / "diff_cpp_cli.py")]).returncode


if __name__ == "__main__":
    sys.exit(main())
