package leanoff

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDiscoverModules(t *testing.T) {
	tmp := t.TempDir()
	files := map[string]string{
		"A.lean":              "",
		"Sub/Nested/Mod.lean": "",
		"Sub/Other.lean":      "",
		"X.txt":               "",
	}
	for name := range files {
		p := filepath.Join(tmp, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	mods, err := DiscoverModules(tmp, "")
	if err != nil {
		t.Fatal(err)
	}
	var names []string
	for _, m := range mods {
		names = append(names, m.Name)
	}
	if strings.Join(names, ",") != "A,Sub.Nested.Mod,Sub.Other" {
		t.Fatalf("names = %v", names)
	}
}

func TestDiscoverSkipsBuildDirs(t *testing.T) {
	tmp := t.TempDir()
	skipped := []string{".lake/Pkg.lean", ".git/G.lean", ".vscode/V.lean",
		"node_modules/N.lean", "dist/D.lean", "out/O.lean", "build/B.lean", ".leanoff/L.lean"}
	for _, name := range skipped {
		p := filepath.Join(tmp, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(tmp, "Keep.lean"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	mods, err := DiscoverModules(tmp, "")
	if err != nil {
		t.Fatal(err)
	}
	if len(mods) != 1 || mods[0].Name != "Keep" {
		t.Fatalf("mods = %v, want [Keep]", mods)
	}
}

func TestDiscoverFilter(t *testing.T) {
	tmp := t.TempDir()
	for _, name := range []string{"A.lean", "Sub/B.lean", "Sub/C.lean"} {
		p := filepath.Join(tmp, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	mods, err := DiscoverModules(tmp, `^Sub\.`)
	if err != nil {
		t.Fatal(err)
	}
	if len(mods) != 2 || mods[0].Name != "Sub.B" || mods[1].Name != "Sub.C" {
		t.Fatalf("mods = %v, want [Sub.B Sub.C]", mods)
	}
}

func TestDiscoverInvalidRegex(t *testing.T) {
	_, err := DiscoverModules(t.TempDir(), "(")
	if err == nil || !strings.Contains(err.Error(), "invalid --filter regex") {
		t.Fatalf("err = %v, want regex compile error", err)
	}
}

func TestModuleNameFromRel(t *testing.T) {
	sep := string(filepath.Separator)
	cases := map[string]string{
		"A.lean":                 "A",
		"Sub" + sep + "Mod.lean": "Sub.Mod",
		"A.lean.lean":            "A.lean",
		".lean":                  ".lean", // leading dot is not a suffix
	}
	for in, want := range cases {
		if got := moduleNameFromRel(in); got != want {
			t.Errorf("moduleNameFromRel(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestParseImports(t *testing.T) {
	tmp := t.TempDir()
	src := "Sub/A.lean"
	p := filepath.Join(tmp, filepath.FromSlash(src))
	body := "import B\n" + // known
		"  import C\n" + // known, indented
		"import Mathlib.Tactic\n" + // unknown: not a project module
		"import Sub.A\n" + // self: excluded
		"-- import D\n" + // comment: not anchored at start after \s*
		"import B\n" + // duplicate: kept, like the Python reference
		"importx E\n" // different keyword
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	mod := Module{Name: "Sub.A", Path: p}
	known := map[string]bool{"Sub.A": true, "B": true, "C": true, "D": true}
	ParseImports(&mod, known)
	if strings.Join(mod.Deps, ",") != "B,C,B" {
		t.Fatalf("deps = %v, want [B C B]", mod.Deps)
	}
}

func TestParseImportsMissingFile(t *testing.T) {
	mod := Module{Name: "A", Path: filepath.Join(t.TempDir(), "gone.lean")}
	ParseImports(&mod, map[string]bool{"A": true})
	if len(mod.Deps) != 0 {
		t.Fatalf("deps = %v, want empty", mod.Deps)
	}
}

func TestParseImportsCRLF(t *testing.T) {
	tmp := t.TempDir()
	p := filepath.Join(tmp, "A.lean")
	if err := os.WriteFile(p, []byte("import B\r\nimport C\r"), 0o644); err != nil {
		t.Fatal(err)
	}
	mod := Module{Name: "A", Path: p}
	ParseImports(&mod, map[string]bool{"A": true, "B": true, "C": true})
	if strings.Join(mod.Deps, ",") != "B,C" {
		t.Fatalf("deps = %v, want [B C]", mod.Deps)
	}
}
