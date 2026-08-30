# verify_proofs.ps1 - standalone verification of the leanoff formal proofs
# Direct-compile recipe (plain `lean`, no lake/git needed).
# Usage: powershell -ExecutionPolicy Bypass -File proofs\verify_proofs.ps1
# Overridable locations:
#   $env:LEAN_BIN - lean 4.32.2 toolchain bin dir (default H:\lean4\lean-4.32.2-windows\bin)
#   $env:MATHLIB  - Mathlib v4.32.2 checkout       (default H:\mathlib432)
$ErrorActionPreference = 'Continue'

$leanBin = if ($env:LEAN_BIN) { $env:LEAN_BIN } else { 'H:\lean4\lean-4.32.2-windows\bin' }
$mathlib = if ($env:MATHLIB) { $env:MATHLIB } else { 'H:\mathlib432' }

$env:Path = "$leanBin;$env:Path"

$libs = @("$mathlib\.lake\build\lib\lean")
Get-ChildItem "$mathlib\.lake\packages" -Directory -ErrorAction SilentlyContinue | ForEach-Object {
    $p = "$($_.FullName)\.lake\build\lib\lean"
    if (Test-Path $p) { $libs += $p }
}
$env:LEAN_PATH = ($libs -join ';') + ";$leanBin\..\lib\lean"

Set-Location $PSScriptRoot
$files = @('TopoLevels.lean') | Where-Object { Test-Path ".\$_" }
$totalErr = 0
foreach ($f in $files) {
    $out = lean ".\$f" 2>&1
    $errs = @($out | Where-Object { $_ -match ': error' })
    $warns = @($out | Where-Object { $_ -match ': warning' })
    $sorries = @($out | Where-Object { $_ -match "uses 'sorry'" })
    '{0}: exit={1} errors={2} warnings={3} sorry={4}' -f $f, $LASTEXITCODE, $errs.Count, $warns.Count, $sorries.Count
    if ($errs) { $errs | Select-Object -First 15 | ForEach-Object { "  $_" } }
    if ($warns) { $warns | Select-Object -First 6 | ForEach-Object { "  W: $_" } }
    $totalErr += $errs.Count
}
exit $totalErr
