# leanoff

**Offline verification and minimal builds for Lean 4 projects. No git, no network, no lake.**

`lake` is a build system, and build systems want to resolve dependencies: network access,
`git`, manifests, content hashes. When any of those is missing, `lake build` refuses to run —
but you still want the one thing that matters:

> elaborate my source files and tell me: **errors, warnings, sorry.**

`leanoff` does exactly that, using plain `lean` and a `LEAN_PATH` assembled from what is
already on disk. It was born on a machine where `lake` failed with *"git is not installed"*
and a dependency-refresh loop, and a 19-module Mathlib-heavy project still had to be verified.

## When you need this

- `lake build` demands `git` or network access and you have neither (air-gapped machines,
  locked-down Windows boxes, China-network conditions, planes)
- CI restored an olean cache and you want to verify source against it without re-resolving
- `lake` rejects a "plausible package" whose URL moved, and you just want to know whether
  your code still elaborates
- You keep a local Mathlib checkout (path dependency) and want fast whole-project checks
  without lake's bookkeeping

## Quickstart

```bash
# verify every module in the project (elaborate against existing oleans)
python leanoff.py verify --root . --lean path/to/lean-toolchain/bin

# compile project modules to oleans in dependency order, lake-free
python leanoff.py build --root .

# standalone folder that is not a lake project? point at your Mathlib:
python leanoff.py verify --root . --lean-path /path/to/mathlib/.lake/build/lib/lean
```

`verify` elaborates each module against the oleans already on disk; `build` additionally
emits project oleans in topological order (default `<root>/.leanoff/olean`, which `verify`
then picks up automatically).

## What it discovers on its own

| Component | Source |
|---|---|
| Project oleans | `<root>/.lake/build/lib/lean` |
| Own build output | `<root>/.leanoff/olean` |
| Dependencies | `lake-manifest.json` — path deps via `dir`, git deps via `packagesDir` |
| Toolchain libs | `<lean>/../lib/lean` |
| Anything else | `--lean-path` flags (repeatable) |

Lake projects with local Mathlib checkouts (path dependencies) need **zero configuration**.
Module order comes from parsing `import` lines; independent modules elaborate in parallel.

## Options

```
--root DIR        project root (default: cwd)
--lean PATH       lean executable or its bin directory
--lean-path DIR   extra LEAN_PATH entry (repeatable)
--filter REGEX    only modules whose name matches
--jobs N          parallel elaboration (default: min(8, cpus))
--allow-sorry     do not fail on `sorry`
--format text|json
```

Exit code is non-zero if any module fails — CI-friendly. `--format json` emits a machine
readable summary.

`build` fails fast: when a module fails, remaining queued modules of its level are
cancelled and dependent levels are not attempted — they would only elaborate against
oleans that were never produced and cascade spurious errors. `verify` always reports
every module; it exists to give the full picture.

Wall time is ~99.9% lean elaboration — Python-side overhead on a 35-module project is
under 0.3 s. Parallelism is memory-bandwidth-bound: on a 16-core/64 GB laptop, `--jobs 16`
buys ~11% over the default 8 (measured warm-cache), and the first run after boot is
slower until the OS caches the dependency oleans.

## Honest limitations

- `verify` elaborates against *existing* oleans. If a dependency's interface changed since
  those oleans were produced, verification can pass against a stale world. Run `build`
  first when in doubt — it regenerates project oleans from current source.
- No fine-grained incremental caching, no trace hashing, no doc generation. It is a
  verification harness, not a build system. When lake works, use lake.
- The toolchain is not installed for you: pass `--lean` or have `lean` on PATH.

## Verified on

- A 19-module Mathlib-heavy project with a local Mathlib path dependency and 7 git
  packages: zero-config discovery, whole-project parallel verification.
- Standalone single-file module against an external Mathlib (`--lean-path`).
- A synthetic core-only two-module project exercising the `build` olean chain.
- 7 orchestration unit tests ([`tests/`](tests/)): level scheduling, fail-fast
  cancellation, killed-lean handling — with a stubbed `run_lean`, no toolchain needed
  (`python -m unittest tests.test_leanoff`).

## Formal verification

The scheduling core of `leanoff build` — the `topo_levels` algorithm that groups modules
into dependency levels for parallel compilation — is machine-checked in Lean 4
([proofs/TopoLevels.lean](proofs/TopoLevels.lean), against Mathlib):

| Theorem | Guarantees |
|---|---|
| `levels_cover` | every module is placed in some level (nothing silently skipped) |
| `levels_level_subset` | levels only contain project modules |
| `levels_disjoint` | no module is scheduled twice |
| `levels_deps_earlier` | every dependency sits in strictly earlier levels — level-by-level compilation is sound |
| `levels_same_level_indep` | no intra-level dependencies — parallel compilation of a level is safe |
| `levels_none_cycle` | the "import cycle among …" exit is never spurious: `none` certifies a real cycle |
| `levels_exists_of_acyclic` | acyclic imports always levelize: `build` never gets stuck on valid graphs |

Reproduce with any Lean 4.32.2 toolchain and a local Mathlib checkout:

```bash
powershell -ExecutionPolicy Bypass -File proofs/verify_proofs.ps1
# overridable: $env:LEAN_BIN, $env:MATHLIB
```

No `sorry`, no `admit`, no new axioms beyond `Classical.choice`.

## Roadmap

- [ ] `lean-toolchain` / elan toolchain auto-discovery
- [ ] `--changed-only` (mtime-based re-verification)
- [ ] CI mode with GitHub Actions annotation output

## License

MIT — see [LICENSE](LICENSE).
