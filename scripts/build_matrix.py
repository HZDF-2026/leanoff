"""Cross-compile the Go port of leanoff into the full platform matrix.

Go cross-compiles from any host with CGO disabled, so one machine can
produce every binary an air-gapped or locked-down box might need — which is
leanoff's whole point. Output lands in dist/go/.

Usage:  python scripts/build_matrix.py [--skip-tests]

Matrix: windows/linux/darwin x amd64/arm64/arm/386 (where meaningful).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MATRIX = [
    ("windows", "amd64"),
    ("windows", "arm64"),
    ("windows", "386"),
    ("linux", "amd64"),
    ("linux", "arm64"),
    ("linux", "386"),
    ("linux", "arm"),
    ("darwin", "amd64"),
    ("darwin", "arm64"),
]


def go_cmd() -> str:
    go = os.environ.get("GO", "")
    if go:
        return go
    found = shutil.which("go")
    if found:
        return found
    portable = Path(r"h:\work\.toolchains\go\bin\go.exe")
    if portable.exists():
        return str(portable)
    return "go"


def build(goos: str, goarch: str) -> Path:
    ext = ".exe" if goos == "windows" else ""
    out = ROOT / "dist" / "go" / f"leanoff-{goos}-{goarch}{ext}"
    out.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, GOOS=goos, GOARCH=goarch, CGO_ENABLED="0")
    r = subprocess.run([go_cmd(), "build", "-trimpath", "-ldflags=-s -w",
                        "-o", str(out), "./cmd/leanoff"],
                       cwd=ROOT, env=env)
    if r.returncode != 0:
        raise SystemExit(f"build failed for {goos}/{goarch}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-tests", action="store_true",
                    help="skip `go test ./...` before building")
    args = ap.parse_args()

    if not args.skip_tests:
        r = subprocess.run([go_cmd(), "test", "./..."], cwd=ROOT)
        if r.returncode != 0:
            raise SystemExit("tests failed; refusing to build matrix")

    built = []
    for goos, goarch in MATRIX:
        out = build(goos, goarch)
        built.append((out, out.stat().st_size))
        print(f"built {out.relative_to(ROOT)} ({out.stat().st_size:,} bytes)")
    print(f"\n{len(built)} binaries in dist/go/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
