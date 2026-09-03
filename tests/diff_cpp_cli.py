"""Ad-hoc CLI differential check: C++ vs Python over the argparse surface.

Runs both implementations with the same argv for every captured argparse
scenario (help, abbreviation, ambiguity, choices, int validation, error
precedence) and compares stdout, stderr, and exit codes byte for byte (modulo
\r\n normalization).

Usage:  python tests/diff_cpp_cli.py
Skips (exit 0) when the C++ binary has not been built.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP_BIN = ROOT / "dist" / "cpp" / ("leanoff.exe" if os.name == "nt" else "leanoff")

# argv variants that reach the engine would need a toolchain; those run against
# a missing lean and must still match (exit 1, same message on stderr).
CASES = [
    # help surfaces
    [], ["-h"], ["--help"], ["--h"],
    ["verify", "-h"], ["verify", "--help"], ["verify", "--h"],
    ["build", "-h"], ["build", "--help"], ["build", "--h"],
    ["-h", "verify"], ["verify", "-h", "--format", "xml"],
    ["verify", "--format", "xml", "-h"],
    # missing / invalid subcommand
    ["frobnicate"], ["-x"], ["-x", "verify"], ["-1"], ["-"], [""],
    ["frob", "--h"], ["--h", "frob"], ["--", "verify"], ["--", "verify", "build"],
    ["--", "--frob", "verify"], ["--frob"], ["--frob", "verify"],
    ["--frob", "verify", "--baz"], ["--root", "x", "verify"], ["--root", "verify"],
    # sub-level usage errors
    ["verify", "--frob"],
    ["verify", "extra"],
    ["verify", "extra", "more"],
    ["verify", "--", "x"],
    ["verify", "--", "--frob"],
    ["verify", "--format", "xml"],
    ["verify", "--format="],
    ["verify", "--jobs", "x"],
    ["verify", "--jobs=x"],
    ["verify", "--jobs="],
    ["verify", "--jobs", "-1.5"],
    ["verify", "--l", "."],
    ["verify", "--f"],
    ["verify", "--lea=x"],
    ["verify", "--l=x"],
    ["verify", "--root"],
    ["verify", "--jobs"],
    ["verify", "--jobs", "--format", "json"],
    ["verify", "--jobs", "-x"],
    ["verify", "--allow-sorry=x"],
    ["verify", "--allow-sorry="],
    ["verify", "--out", "x"],
    ["build", "--out"],
    ["verify", "--form"],
    ["verify", "--form=x"],
    ["verify", "-x"],
    ["verify", "--format", "json", "--"],
    ["verify", "--help=x"],
    # engine path without lean on PATH (message + exit 1)
    ["verify"],
    ["verify", "--form=json"],
    ["verify", "--al"],
    ["verify", "--r", "x"],
]


def norm(s: str) -> str:
    return s.replace("\r\n", "\n")


def run(argv: list) -> tuple:
    py = subprocess.run([sys.executable, str(ROOT / "leanoff.py")] + argv,
                        capture_output=True, text=True)
    cpp = subprocess.run([str(CPP_BIN)] + argv, capture_output=True, text=True)
    return py, cpp


def main() -> int:
    if not CPP_BIN.exists():
        print(f"skip: {CPP_BIN} not built")
        return 0
    failures = 0
    for argv in CASES:
        py, cpp = run(argv)
        label = "leanoff " + " ".join(repr(a) for a in argv) if argv else "leanoff"
        ok = (py.returncode == cpp.returncode
              and norm(py.stdout) == norm(cpp.stdout)
              and norm(py.stderr) == norm(cpp.stderr))
        if not ok:
            failures += 1
            print(f"FAIL {label}")
            print(f"  exit {py.returncode} vs {cpp.returncode}")
            if norm(py.stdout) != norm(cpp.stdout):
                print("  py stdout:", repr(py.stdout))
                print("  cpp stdout:", repr(cpp.stdout))
            if norm(py.stderr) != norm(cpp.stderr):
                print("  py stderr:", repr(py.stderr))
                print("  cpp stderr:", repr(cpp.stderr))
        else:
            print(f"ok   {label}")
    if failures:
        print(f"\n{failures} mismatch(es)")
        return 1
    print(f"\nall {len(CASES)} CLI cases match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
