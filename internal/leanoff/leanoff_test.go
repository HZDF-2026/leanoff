package leanoff

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// Regression tests mirroring tests/test_leanoff.py: a stubbed RunLean (no
// real Lean toolchain) exercises the orchestration layer — level
// scheduling, fail-fast on a failed level, skipping queued work, and
// turning a killed lean into a FAIL result instead of a panic.

func setupProject(t *testing.T, files map[string]string) string {
	t.Helper()
	tmp := t.TempDir()
	bin := filepath.Join(tmp, "bin")
	if err := os.MkdirAll(bin, 0o755); err != nil {
		t.Fatal(err)
	}
	// create both spellings so the test is portable: find_toolchain looks
	// for lean.exe on Windows and lean elsewhere.
	for _, name := range []string{"lean", "lean.exe"} {
		if err := os.WriteFile(filepath.Join(bin, name), nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	for name, text := range files {
		p := filepath.Join(tmp, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(text), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return tmp
}

func opts(root string, jobs int) Options {
	return Options{Root: root, Lean: filepath.Join(root, "bin"), Jobs: jobs, Format: "json"}
}

func TestBuildStopsAfterFailedLevel(t *testing.T) {
	tmp := setupProject(t, map[string]string{
		"X.lean": "",
		"Y.lean": "",
		"Z.lean": "import X\n",
	})
	var mu sync.Mutex
	var calls []string
	eng := &Engine{
		Out: &bytes.Buffer{},
		RunLean: func(exe, root string, mod Module, leanPath, outOlean string, cwdRoot bool) Result {
			mu.Lock()
			calls = append(calls, mod.Name)
			mu.Unlock()
			if mod.Name == "X" {
				return Result{Name: "X", Errors: 1, FirstError: "X.lean:1:0: error: nope"}
			}
			time.Sleep(150 * time.Millisecond)
			return Result{Name: mod.Name, OK: true}
		},
	}
	code, err := eng.Build(opts(tmp, 1))
	if err != nil {
		t.Fatal(err)
	}
	// X failed in level 0: queued Y is skipped, Z (level 1) never runs,
	// and the whole thing exits non-zero without a panic.
	if code != 1 {
		t.Fatalf("exit code = %d, want 1", code)
	}
	if !contains(calls, "X") {
		t.Fatalf("X was not elaborated: %v", calls)
	}
	if contains(calls, "Z") {
		t.Fatalf("Z ran after a failed level: %v", calls)
	}
}

func TestBuildHealthyRunsEverything(t *testing.T) {
	tmp := setupProject(t, map[string]string{
		"A.lean": "",
		"B.lean": "import A\n",
		"C.lean": "import B\n",
	})
	var mu sync.Mutex
	var calls []string
	eng := &Engine{
		Out: &bytes.Buffer{},
		RunLean: func(exe, root string, mod Module, leanPath, outOlean string, cwdRoot bool) Result {
			mu.Lock()
			calls = append(calls, mod.Name)
			mu.Unlock()
			return Result{Name: mod.Name, OK: true, Seconds: 0.01}
		},
	}
	code, err := eng.Build(opts(tmp, 4))
	if err != nil {
		t.Fatal(err)
	}
	if code != 0 {
		t.Fatalf("exit code = %d, want 0", code)
	}
	want := []string{"A", "B", "C"}
	if len(calls) != 3 {
		t.Fatalf("calls = %v, want all of %v", calls, want)
	}
	sorted := append([]string(nil), calls...)
	for i := range sorted {
		if i > 0 && sorted[i] < sorted[i-1] {
			t.Fatalf("calls not sortable: %v", calls)
		}
	}
	if !contains(calls, "A") || !contains(calls, "B") || !contains(calls, "C") {
		t.Fatalf("missing modules: %v", calls)
	}
}

func TestBuildKilledLeanBecomesFailResult(t *testing.T) {
	tmp := setupProject(t, map[string]string{"A.lean": ""})
	eng := &Engine{
		Out: &bytes.Buffer{},
		RunLean: func(exe, root string, mod Module, leanPath, outOlean string, cwdRoot bool) Result {
			panic("process killed by OOM")
		},
	}
	code, err := eng.Build(opts(tmp, 1))
	if err != nil {
		t.Fatal(err)
	}
	if code != 1 {
		t.Fatalf("exit code = %d, want 1", code)
	}
}

func TestVerifyReportsAllModulesEvenWithFailures(t *testing.T) {
	tmp := setupProject(t, map[string]string{
		"A.lean": "",
		"B.lean": "",
	})
	eng := &Engine{
		Out: &bytes.Buffer{},
		RunLean: func(exe, root string, mod Module, leanPath, outOlean string, cwdRoot bool) Result {
			ok := mod.Name != "A"
			r := Result{Name: mod.Name, OK: ok, Seconds: 0.01}
			if !ok {
				r.Errors = 1
			}
			return r
		},
	}
	code, err := eng.Verify(opts(tmp, 2))
	if err != nil {
		t.Fatal(err)
	}
	if code != 1 {
		t.Fatalf("exit code = %d, want 1", code)
	}
}

func TestVerifyAllowSorryFlipsVerdict(t *testing.T) {
	tmp := setupProject(t, map[string]string{"A.lean": ""})
	eng := &Engine{
		Out: &bytes.Buffer{},
		RunLean: func(exe, root string, mod Module, leanPath, outOlean string, cwdRoot bool) Result {
			return Result{Name: mod.Name, OK: true, Sorries: 2}
		},
	}
	code, err := eng.Verify(opts(tmp, 1))
	if err != nil {
		t.Fatal(err)
	}
	if code != 1 {
		t.Fatalf("exit code = %d, want 1 (sorry fails by default)", code)
	}
	o := opts(tmp, 1)
	o.AllowSorry = true
	code, err = eng.Verify(o)
	if err != nil {
		t.Fatal(err)
	}
	if code != 0 {
		t.Fatalf("exit code = %d, want 0 (allow-sorry)", code)
	}
}

func TestVerifyNoModulesMessage(t *testing.T) {
	tmp := setupProject(t, nil)
	var buf bytes.Buffer
	eng := &Engine{Out: &buf, RunLean: func(exe, root string, mod Module, lp, oo string, cr bool) Result {
		t.Fatal("RunLean must not be called")
		return Result{}
	}}
	o := opts(tmp, 1)
	o.Filter = "Nope.*"
	code, err := eng.Verify(o)
	if err != nil {
		t.Fatal(err)
	}
	if code != 0 {
		t.Fatalf("exit code = %d, want 0", code)
	}
	want := "no .lean modules found under " + pyResolve(tmp) + " matching 'Nope.*'\n"
	if buf.String() != want {
		t.Fatalf("output = %q, want %q", buf.String(), want)
	}
}

func TestTopoLevelOrder(t *testing.T) {
	mods := []Module{
		{Name: "C", Path: "C.lean", Deps: []string{"B"}},
		{Name: "A", Path: "A.lean"},
		{Name: "B", Path: "B.lean", Deps: []string{"A"}},
	}
	levels, err := TopoLevels(mods)
	if err != nil {
		t.Fatal(err)
	}
	var order []string
	for _, lvl := range levels {
		for _, m := range lvl {
			order = append(order, m.Name)
		}
	}
	if strings.Join(order, ",") != "A,B,C" {
		t.Fatalf("order = %v, want A,B,C", order)
	}
	if len(levels) != 3 {
		t.Fatalf("levels = %d, want 3", len(levels))
	}
}

func TestTopoParallelModulesShareALevel(t *testing.T) {
	mods := []Module{
		{Name: "A", Path: "A.lean"},
		{Name: "B", Path: "B.lean"},
		{Name: "C", Path: "C.lean", Deps: []string{"A", "B"}},
	}
	levels, err := TopoLevels(mods)
	if err != nil {
		t.Fatal(err)
	}
	if len(levels) != 2 {
		t.Fatalf("levels = %d, want 2", len(levels))
	}
	if len(levels[0]) != 2 || levels[0][0].Name != "A" || levels[0][1].Name != "B" {
		t.Fatalf("level 0 = %v, want [A B]", levels[0])
	}
	if len(levels[1]) != 1 || levels[1][0].Name != "C" {
		t.Fatalf("level 1 = %v, want [C]", levels[1])
	}
}

func TestTopoCycleReturnsError(t *testing.T) {
	mods := []Module{
		{Name: "A", Path: "A.lean", Deps: []string{"B"}},
		{Name: "B", Path: "B.lean", Deps: []string{"A"}},
	}
	_, err := TopoLevels(mods)
	if err == nil {
		t.Fatal("expected import-cycle error")
	}
	if !strings.Contains(err.Error(), "import cycle among: A, B") {
		t.Fatalf("error = %v, want cycle message", err)
	}
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}
