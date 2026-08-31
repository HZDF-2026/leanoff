package leanoff

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"
)

// Options mirrors the Python CLI flags.
type Options struct {
	Root       string
	Lean       string
	LeanPath   []string
	Filter     string
	Jobs       int
	AllowSorry bool
	Format     string
	Out        string // build only
}

// Engine runs verify/build. Out receives the report; RunLean is injectable
// so tests can exercise the scheduling layer without a real toolchain.
type Engine struct {
	Out     io.Writer
	RunLean RunLeanFunc
}

func (e *Engine) lean() RunLeanFunc {
	if e.RunLean != nil {
		return e.RunLean
	}
	return RunLean
}

// runLeanSafe mirrors the Python _collect wrapper: a killed (panicking)
// lean becomes a FAIL result, never a crash.
func (e *Engine) runLeanSafe(exe, root string, mod Module, leanPath, olean string, cwdRoot bool) (r Result) {
	defer func() {
		if p := recover(); p != nil {
			r = Result{Name: mod.Name, Errors: 1,
				FirstError: fmt.Sprintf("leanoff: lean died: %v", p)}
		}
	}()
	return e.lean()(exe, root, mod, leanPath, olean, cwdRoot)
}

// Verify elaborates every module against existing oleans and reports.
func (e *Engine) Verify(o Options) (int, error) {
	root := pyResolve(o.Root)
	exe, toolchainLib, err := FindToolchain(o.Lean)
	if err != nil {
		return 1, err
	}
	comps := LeanPathComponents(root, filepath.Join(root, "lake-manifest.json"), o.LeanPath)
	leanPath := strings.Join(append(comps, toolchainLib), string(os.PathListSeparator))
	mods, err := DiscoverModules(root, o.Filter)
	if err != nil {
		return 1, err
	}
	if len(mods) == 0 {
		msg := "no .lean modules found under " + root
		if o.Filter != "" {
			msg += " matching '" + o.Filter + "'"
		}
		fmt.Fprintln(e.Out, msg)
		return 0, nil
	}
	known := make(map[string]bool, len(mods))
	for _, m := range mods {
		known[m.Name] = true
	}
	for i := range mods {
		ParseImports(&mods[i], known)
	}
	t0 := time.Now()
	var mu sync.Mutex
	var results []Result
	var wg sync.WaitGroup
	sem := make(chan struct{}, clampJobs(o.Jobs))
	for _, m := range mods {
		wg.Add(1)
		sem <- struct{}{}
		go func(mod Module) {
			defer wg.Done()
			defer func() { <-sem }()
			r := e.runLeanSafe(exe, root, mod, leanPath, "", true)
			r.OK = Classify(r, o.AllowSorry)
			mu.Lock()
			results = append(results, r)
			mu.Unlock()
		}(m)
	}
	wg.Wait()
	return e.report(results, time.Since(t0).Seconds(), o.Format), nil
}

// Build compiles modules to oleans in dependency order, level by level.
// A failed level stops the build: modules below it would miss their oleans
// and only cascade spurious errors. Work queued behind a failure within a
// level is skipped, exactly like cancelling the pending futures.
func (e *Engine) Build(o Options) (int, error) {
	root := pyResolve(o.Root)
	exe, toolchainLib, err := FindToolchain(o.Lean)
	if err != nil {
		return 1, err
	}
	outdir := pyResolve(filepath.Join(root, ".leanoff", "olean"))
	if o.Out != "" {
		outdir = pyResolve(expandUser(o.Out))
	}
	comps := LeanPathComponents(root, filepath.Join(root, "lake-manifest.json"), o.LeanPath)
	leanPath := strings.Join(append(append([]string{outdir}, comps...), toolchainLib),
		string(os.PathListSeparator))
	mods, err := DiscoverModules(root, o.Filter)
	if err != nil {
		return 1, err
	}
	if len(mods) == 0 {
		fmt.Fprintf(e.Out, "no .lean modules found under %s\n", root)
		return 0, nil
	}
	known := make(map[string]bool, len(mods))
	for _, m := range mods {
		known[m.Name] = true
	}
	for i := range mods {
		ParseImports(&mods[i], known)
	}
	levels, err := TopoLevels(mods)
	if err != nil {
		return 1, err
	}
	t0 := time.Now()
	var mu sync.Mutex
	var results []Result
	for _, level := range levels {
		var cancelled atomic.Bool
		failed := false
		var wg sync.WaitGroup
		sem := make(chan struct{}, clampJobs(o.Jobs))
		for _, m := range level {
			wg.Add(1)
			sem <- struct{}{}
			go func(mod Module) {
				defer wg.Done()
				defer func() { <-sem }()
				if cancelled.Load() {
					return
				}
				rel := strings.ReplaceAll(mod.Name, ".", string(filepath.Separator))
				olean := filepath.Join(outdir, rel+".olean")
				_ = os.MkdirAll(filepath.Dir(olean), 0o755)
				r := e.runLeanSafe(exe, root, mod, leanPath, olean, true)
				r.OK = Classify(r, o.AllowSorry)
				mu.Lock()
				results = append(results, r)
				if !r.OK {
					failed = true
					cancelled.Store(true)
				}
				mu.Unlock()
			}(m)
		}
		wg.Wait()
		mu.Lock()
		stop := failed
		mu.Unlock()
		if stop {
			break
		}
	}
	return e.report(results, time.Since(t0).Seconds(), o.Format), nil
}

// report prints the text or JSON report and returns the exit code:
// 1 if any module failed, else 0.
func (e *Engine) report(results []Result, wall float64, format string) int {
	sort.Slice(results, func(i, j int) bool { return results[i].Name < results[j].Name })
	failed := 0
	for _, r := range results {
		if !r.OK {
			failed++
		}
	}
	if format == "json" {
		fmt.Fprintln(e.Out, reportJSON(results, failed, wall))
		if failed > 0 {
			return 1
		}
		return 0
	}
	width := 8
	if len(results) > 0 {
		width = 0
		for _, r := range results {
			if n := utf8.RuneCountInString(r.Name); n > width {
				width = n
			}
		}
	}
	w := e.Out
	fmt.Fprintf(w, "%-*s  %-6s %3s %4s %5s %7s\n", width, "module", "status", "err", "warn", "sorry", "time")
	totE, totW, totS := 0, 0, 0
	for _, r := range results {
		status := "FAIL"
		if r.OK {
			status = "PASS"
		}
		fmt.Fprintf(w, "%-*s  %-6s %3d %4d %5d %6.1fs\n",
			width, r.Name, status, r.Errors, r.Warnings, r.Sorries, r.Seconds)
		if !r.OK && r.FirstError != "" {
			fmt.Fprintf(w, "    %s\n", truncateRunes(strings.TrimSpace(r.FirstError), 160))
		}
		totE += r.Errors
		totW += r.Warnings
		totS += r.Sorries
	}
	fmt.Fprintf(w, "\n%d modules: %d failed, %d errors, %d warnings, %d sorry  (%.1fs wall)\n",
		len(results), failed, totE, totW, totS, wall)
	if failed > 0 {
		return 1
	}
	return 0
}

// reportJSON renders the report exactly as Python's
// json.dumps(..., indent=2): ASCII-escaped strings, insertion-ordered
// fields, two-space indent.
func reportJSON(results []Result, failed int, wall float64) string {
	var b strings.Builder
	b.WriteString("{\n  \"modules\": [")
	for i, r := range results {
		if i > 0 {
			b.WriteString(",")
		}
		b.WriteString("\n    {\n")
		fmt.Fprintf(&b, "      \"name\": %s,\n", pyStr(r.Name))
		fmt.Fprintf(&b, "      \"ok\": %v,\n", r.OK)
		fmt.Fprintf(&b, "      \"errors\": %d,\n", r.Errors)
		fmt.Fprintf(&b, "      \"warnings\": %d,\n", r.Warnings)
		fmt.Fprintf(&b, "      \"sorries\": %d,\n", r.Sorries)
		fmt.Fprintf(&b, "      \"seconds\": %s,\n", pyFloat(r.Seconds))
		fmt.Fprintf(&b, "      \"first_error\": %s\n", pyStr(r.FirstError))
		b.WriteString("    }")
	}
	if len(results) > 0 {
		b.WriteString("\n  ],\n")
	} else {
		b.WriteString("],\n")
	}
	fmt.Fprintf(&b, "  \"failed\": %d,\n", failed)
	fmt.Fprintf(&b, "  \"wall_seconds\": %s\n", pyFloat(round1(wall)))
	b.WriteString("}")
	return b.String()
}

func clampJobs(n int) int {
	if n < 1 {
		return 1
	}
	return n
}
