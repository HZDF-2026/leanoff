package leanoff

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// Result is the outcome of elaborating one module. Field names and JSON
// shape mirror the Python reference's dataclass exactly.
type Result struct {
	Name       string  `json:"name"`
	OK         bool    `json:"ok"`
	Errors     int     `json:"errors"`
	Warnings   int     `json:"warnings"`
	Sorries    int     `json:"sorries"`
	Seconds    float64 `json:"seconds"`
	FirstError string  `json:"first_error"`
}

// RunLeanFunc elaborates one module; injectable for tests.
type RunLeanFunc func(exe, root string, mod Module, leanPath, outOlean string, cwdRoot bool) Result

// RunLean elaborates mod with the real lean binary: combined stdout/stderr,
// a 3600s timeout, and LEAN_PATH set. A lean that dies (timeout, spawn
// failure) becomes a FAIL result, never a panic.
func RunLean(exe, root string, mod Module, leanPath, outOlean string, cwdRoot bool) Result {
	args := []string{}
	if outOlean != "" {
		args = append(args, "--o="+outOlean)
	}
	modPath := mod.Path
	if cwdRoot {
		if rel, err := filepath.Rel(root, mod.Path); err == nil {
			modPath = rel
		}
	}
	args = append(args, modPath)
	cwd := root
	if !cwdRoot {
		cwd = filepath.Dir(mod.Path)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3600*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, exe, args...)
	cmd.Dir = cwd
	cmd.Env = append(os.Environ(), "LEAN_PATH="+leanPath)
	t0 := time.Now()
	out, err := cmd.CombinedOutput()
	if err != nil && ctx.Err() == context.DeadlineExceeded {
		return Result{Name: mod.Name, Errors: 1,
			FirstError: fmt.Sprintf("leanoff: lean died: TimeoutExpired: %v", err)}
	}
	ok := true
	if err != nil {
		if _, exitErr := err.(*exec.ExitError); exitErr {
			ok = false // lean ran and exited non-zero: parse output normally
		} else {
			return Result{Name: mod.Name, Errors: 1,
				FirstError: fmt.Sprintf("leanoff: lean died: OSError: %v", err)}
		}
	}
	seconds := time.Since(t0).Seconds()
	errs, warns, sorries, first := parseLeanOutput(string(out))
	return Result{Name: mod.Name, OK: ok, Errors: errs, Warnings: warns,
		Sorries: sorries, Seconds: seconds, FirstError: first}
}

// parseLeanOutput counts error, warning, and sorry lines in lean's output
// and captures the first error line, mirroring the Python regexes
// ": error", ": warning", and "uses 'sorry'".
func parseLeanOutput(out string) (errs, warns, sorries int, first string) {
	for _, line := range pySplitLines(out) {
		if strings.Contains(line, ": error") {
			errs++
			if first == "" {
				first = line
			}
		}
		if strings.Contains(line, ": warning") {
			warns++
		}
		if strings.Contains(line, "uses 'sorry'") {
			sorries++
		}
	}
	return
}

// Classify decides pass/fail: any error, a non-zero lean exit, or (unless
// allowed) a sorry means failure.
func Classify(r Result, allowSorry bool) bool {
	if r.Errors > 0 || !r.OK {
		return false
	}
	if r.Sorries > 0 && !allowSorry {
		return false
	}
	return true
}
