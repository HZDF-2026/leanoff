# Formal proofs

`TopoLevels.lean` machine-checks the `topo_levels` scheduling algorithm of
`leanoff build` in Lean 4 (Mathlib v4.32.2):

- levelization completeness, disjointness, and containment (`levels_cover`,
  `levels_level_subset`, `levels_disjoint`)
- build soundness: dependencies are compiled strictly earlier
  (`levels_deps_earlier`); intra-level parallelism is dependency-free
  (`levels_same_level_indep`)
- the `none` exit certifies a genuine import cycle (`levels_none_cycle`),
  and acyclic graphs always levelize (`levels_exists_of_acyclic`)

Run:

```bash
powershell -ExecutionPolicy Bypass -File verify_proofs.ps1
```

The script compiles the proofs directly with `lean` (no lake, no git), using
`$env:LEAN_BIN` (lean 4.32.2 bin dir) and `$env:MATHLIB` (Mathlib v4.32.2
checkout). It reports errors/warnings/sorry counts and exits non-zero on any
error.
