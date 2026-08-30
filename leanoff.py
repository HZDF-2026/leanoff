#!/usr/bin/env python3
"""leanoff — offline verification and minimal builds for Lean 4 projects.

lake is a build system, and build systems want to resolve dependencies:
network, git, manifests, hashes. When you are air-gapped, on a locked-down
Windows box, in a CI cache-restore job, or lake simply refuses to re-verify a
"plausible package" whose URL moved, you still want one thing:

    elaborate my source files and tell me: errors, warnings, sorry.

leanoff does exactly that, with plain `lean` and a LEAN_PATH assembled from
what is already on disk. No git, no network, no lake.

Commands:
    leanoff verify   elaborate modules against existing oleans, report
    leanoff build    compile project modules to oleans in dependency order

Zero dependencies. Python 3.9+ stdlib only.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path

IMPORT_RE = re.compile(r"^\s*import\s+([A-Za-z_][A-Za-z0-9_.]*)")
ERROR_RE = re.compile(r": error")
WARNING_RE = re.compile(r": warning")
SORRY_RE = re.compile(r"uses 'sorry'")

SKIP_DIRS = {".lake", ".git", ".vscode", "node_modules", "dist", "out", "build", ".leanoff"}


@dataclass
class Module:
    name: str
    path: Path
    deps: list = field(default_factory=list)  # project-internal deps only


@dataclass
class Result:
    name: str
    ok: bool
    errors: int
    warnings: int
    sorries: int
    seconds: float
    first_error: str = ""


def find_toolchain(spec: str | None) -> tuple:
    """Resolve (lean_exe, toolchain_lib_dir). spec is a path to lean or its bin dir."""
    if spec is None:
        import shutil

        lean = shutil.which("lean")
        if lean is None:
            sys.exit("leanoff: no `lean` on PATH and no --lean given")
        exe = Path(lean).resolve()
    else:
        p = Path(spec).expanduser()
        if p.is_dir():
            exe = p / ("lean.exe" if os.name == "nt" else "lean")
        else:
            exe = p
        if not exe.exists():
            sys.exit(f"leanoff: lean executable not found: {exe}")
        exe = exe.resolve()
    lib = exe.parent.parent / "lib" / "lean"
    return exe, lib if lib.is_dir() else exe.parent.parent / "lib" / "lean"


def lean_path_components(root: Path, manifest: Path | None, extra: list) -> list:
    """LEAN_PATH directories: extras, leanoff's own oleans, project oleans,
    package oleans (manifest order)."""
    comps = [Path(e).expanduser().resolve() for e in extra]
    own = root / ".leanoff" / "olean"
    if own.is_dir():
        comps.append(own.resolve())
    proj = root / ".lake" / "build" / "lib" / "lean"
    if proj.is_dir():
        comps.append(proj.resolve())
    if manifest and manifest.is_file():
        try:
            data = json.loads(manifest.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            data = {}
        packages_dir = root / data.get("packagesDir", ".lake/packages")
        for entry in data.get("packages", []):
            if not isinstance(entry, dict):
                continue
            d = entry.get("dir")
            if entry.get("type") == "path" and d:
                pkg_root = Path(d).expanduser()
                if not pkg_root.is_absolute():
                    pkg_root = (root / pkg_root).resolve()
            else:
                pkg_root = packages_dir / str(entry.get("name", ""))
            odir = pkg_root / ".lake" / "build" / "lib" / "lean"
            if odir.is_dir():
                comps.append(odir.resolve())
    return comps


def discover_modules(root: Path, pattern: str | None) -> list:
    """All .lean files under root as modules, optionally filtered by regex."""
    modules = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if not fn.endswith(".lean"):
                continue
            f = Path(dirpath) / fn
            rel = f.relative_to(root)
            name = str(rel.with_suffix("")).replace(os.sep, ".")
            if pattern and not re.search(pattern, name):
                continue
            modules[name] = Module(name=name, path=f)
    return list(modules.values())


def parse_imports(mod: Module, known: set) -> None:
    deps = []
    try:
        text = mod.path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return
    for line in text.splitlines():
        m = IMPORT_RE.match(line)
        if not m:
            continue
        dep = m.group(1)
        if dep in known and dep != mod.name:
            deps.append(dep)
    mod.deps = deps


def topo_levels(mods: list) -> list:
    """Modules grouped into dependency levels; each level is parallelizable."""
    remaining = {m.name: set(m.deps) for m in mods}
    done, levels = set(), []
    while remaining:
        ready = sorted(n for n, ds in remaining.items() if ds <= done)
        if not ready:
            sys.exit(
                "leanoff: import cycle among: "
                + ", ".join(sorted(remaining))
            )
        levels.append(ready)
        done |= set(ready)
        for n in ready:
            del remaining[n]
    by_name = {m.name: m for m in mods}
    return [[by_name[n] for n in lvl] for lvl in levels]


def run_lean(lean_exe, root, mod, lean_path, out_olean, cwd_root) -> Result:
    cmd = [str(lean_exe)]
    if out_olean is not None:
        cmd.append(f"--o={out_olean}")
    cmd.append(str(mod.path.relative_to(root) if cwd_root else mod.path))
    env = dict(os.environ)
    env["LEAN_PATH"] = lean_path
    t0 = time.monotonic()
    proc = subprocess.run(
        cmd,
        cwd=str(root if cwd_root else mod.path.parent),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=3600,
    )
    seconds = time.monotonic() - t0
    out = proc.stdout.decode("utf-8", errors="replace")
    errors = sum(1 for l in out.splitlines() if ERROR_RE.search(l))
    warnings = sum(1 for l in out.splitlines() if WARNING_RE.search(l))
    sorries = sum(1 for l in out.splitlines() if SORRY_RE.search(l))
    first = next((l for l in out.splitlines() if ERROR_RE.search(l)), "")
    return Result(mod.name, proc.returncode == 0, errors, warnings, sorries, seconds, first)


def classify(r: Result, allow_sorry: bool) -> bool:
    if r.errors > 0 or not r.ok:
        return False
    if r.sorries > 0 and not allow_sorry:
        return False
    return True


def report(results: list, wall: float, args) -> int:
    results.sort(key=lambda r: r.name)
    failed = [r for r in results if not r.ok]
    if args.format == "json":
        print(json.dumps({
            "modules": [r.__dict__ for r in results],
            "failed": len(failed),
            "wall_seconds": round(wall, 1),
        }, indent=2))
        return 1 if failed else 0
    w = max((len(r.name) for r in results), default=8)
    print(f"{'module':<{w}}  {'status':<6} {'err':>3} {'warn':>4} {'sorry':>5} {'time':>7}")
    for r in results:
        status = "PASS" if r.ok else "FAIL"
        print(f"{r.name:<{w}}  {status:<6} {r.errors:>3} {r.warnings:>4} {r.sorries:>5} {r.seconds:>6.1f}s")
        if not r.ok and r.first_error:
            print(f"    {r.first_error.strip()[:160]}")
    tot_e = sum(r.errors for r in results)
    tot_w = sum(r.warnings for r in results)
    tot_s = sum(r.sorries for r in results)
    print(f"\n{len(results)} modules: {len(failed)} failed, {tot_e} errors, {tot_w} warnings, {tot_s} sorry  ({wall:.1f}s wall)")
    return 1 if failed else 0


def cmd_verify(args) -> int:
    root = Path(args.root).expanduser().resolve()
    lean_exe, toolchain_lib = find_toolchain(args.lean)
    comps = lean_path_components(root, root / "lake-manifest.json", args.lean_path)
    lean_path = os.pathsep.join(str(c) for c in comps + [toolchain_lib])
    mods = discover_modules(root, args.filter)
    if not mods:
        print(f"no .lean modules found under {root}"
              + (f" matching {args.filter!r}" if args.filter else ""))
        return 0
    known = {m.name for m in mods}
    for m in mods:
        parse_imports(m, known)
    t0 = time.monotonic()
    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_lean, lean_exe, root, m, lean_path, None, True): m for m in mods}
        for f in as_completed(futs):
            r = f.result()
            r.ok = classify(r, args.allow_sorry)
            results.append(r)
    return report(results, time.monotonic() - t0, args)


def cmd_build(args) -> int:
    root = Path(args.root).expanduser().resolve()
    lean_exe, toolchain_lib = find_toolchain(args.lean)
    outdir = Path(args.out).expanduser().resolve() if args.out else root / ".leanoff" / "olean"
    comps = lean_path_components(root, root / "lake-manifest.json", args.lean_path)
    lean_path = os.pathsep.join(str(d) for d in [outdir] + comps + [toolchain_lib])
    mods = discover_modules(root, args.filter)
    if not mods:
        print(f"no .lean modules found under {root}")
        return 0
    known = {m.name for m in mods}
    for m in mods:
        parse_imports(m, known)
    t0 = time.monotonic()
    results = []
    for level in topo_levels(mods):
        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {}
            for m in level:
                olean = outdir / Path(*m.name.split("."))
                olean = olean.with_suffix(".olean")
                olean.parent.mkdir(parents=True, exist_ok=True)
                futs[ex.submit(run_lean, lean_exe, root, m, lean_path, str(olean), True)] = m
            for f in as_completed(futs):
                r = f.result()
                r.ok = classify(r, args.allow_sorry)
                results.append(r)
                if not r.ok:
                    for m in level:
                        ex.shutdown(wait=False, cancel_futures=True)
                        break
    return report(results, time.monotonic() - t0, args)


def main() -> None:
    ap = argparse.ArgumentParser(prog="leanoff", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--root", default=".", help="project root (default: cwd)")
        p.add_argument("--lean", help="path to lean executable or its bin directory")
        p.add_argument("--lean-path", action="append", default=[], help="extra LEAN_PATH directory (repeatable, e.g. a local Mathlib checkout)")
        p.add_argument("--filter", help="only modules whose name matches this regex")
        p.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
        p.add_argument("--allow-sorry", action="store_true", help="do not fail on sorry")
        p.add_argument("--format", choices=["text", "json"], default="text")

    vp = sub.add_parser("verify", help="elaborate modules against existing oleans")
    common(vp)
    vp.set_defaults(func=cmd_verify)

    bp = sub.add_parser("build", help="compile project modules to oleans in dependency order")
    common(bp)
    bp.add_argument("--out", help="olean output directory (default: <root>/.leanoff/olean)")
    bp.set_defaults(func=cmd_build)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
