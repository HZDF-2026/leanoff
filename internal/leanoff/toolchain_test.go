package leanoff

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestFindToolchainFromBinDir(t *testing.T) {
	tmp := t.TempDir()
	bin := filepath.Join(tmp, "toolchain", "bin")
	if err := os.MkdirAll(bin, 0o755); err != nil {
		t.Fatal(err)
	}
	exeName := "lean"
	if isWindows() {
		exeName = "lean.exe"
	}
	exe := filepath.Join(bin, exeName)
	if err := os.WriteFile(exe, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(tmp, "toolchain", "lib", "lean"), 0o755); err != nil {
		t.Fatal(err)
	}
	gotExe, lib, err := FindToolchain(bin)
	if err != nil {
		t.Fatal(err)
	}
	// FindToolchain resolves symlinks (and Windows 8.3 names) like Python's
	// Path.resolve, so the expectations must go through the same resolution.
	wantExe := pyResolve(exe)
	if filepath.Clean(gotExe) != filepath.Clean(wantExe) {
		t.Fatalf("exe = %q, want %q", gotExe, wantExe)
	}
	wantLib := filepath.Join(filepath.Dir(filepath.Dir(wantExe)), "lib", "lean")
	if filepath.Clean(lib) != filepath.Clean(wantLib) {
		t.Fatalf("lib = %q, want %q", lib, wantLib)
	}
}

func TestFindToolchainMissingExe(t *testing.T) {
	tmp := t.TempDir()
	bin := filepath.Join(tmp, "bin")
	if err := os.MkdirAll(bin, 0o755); err != nil {
		t.Fatal(err)
	}
	_, _, err := FindToolchain(bin)
	if err == nil {
		t.Fatal("expected error for missing lean executable")
	}
	if !strings.Contains(err.Error(), "leanoff: lean executable not found:") {
		t.Fatalf("err = %v", err)
	}
}

func TestFindToolchainNoLeanOnPath(t *testing.T) {
	orig := lookPath
	lookPath = func(string) (string, error) {
		return "", os.ErrNotExist
	}
	t.Cleanup(func() { lookPath = orig })
	_, _, err := FindToolchain("")
	if err == nil || err.Error() != "leanoff: no `lean` on PATH and no --lean given" {
		t.Fatalf("err = %v, want no-lean message", err)
	}
}

func TestLeanPathComponents(t *testing.T) {
	tmp := t.TempDir()
	for _, d := range []string{
		filepath.Join(tmp, ".leanoff", "olean"),
		filepath.Join(tmp, ".lake", "build", "lib", "lean"),
		filepath.Join(tmp, "pkgs", "Mathlib", ".lake", "build", "lib", "lean"),
		filepath.Join(tmp, "local", "pathpkg", ".lake", "build", "lib", "lean"),
	} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	manifest := `{"packagesDir": "pkgs", "packages": [
		{"type": "path", "dir": "local/pathpkg"},
		{"name": "Mathlib"},
		{"name": "Missing", "dir": "gone"}
	]}`
	if err := os.WriteFile(filepath.Join(tmp, "lake-manifest.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
	comps := LeanPathComponents(tmp, filepath.Join(tmp, "lake-manifest.json"), nil)
	// LeanPathComponents resolves symlinks like Python's Path.resolve, so the
	// expectations must be built from the resolved temp dir.
	rt := pyResolve(tmp)
	want := []string{
		filepath.Join(rt, ".leanoff", "olean"),
		filepath.Join(rt, ".lake", "build", "lib", "lean"),
		filepath.Join(rt, "local", "pathpkg", ".lake", "build", "lib", "lean"),
		filepath.Join(rt, "pkgs", "Mathlib", ".lake", "build", "lib", "lean"),
	}
	if len(comps) != len(want) {
		t.Fatalf("comps = %v, want %v", comps, want)
	}
	for i := range want {
		if comps[i] != want[i] {
			t.Errorf("comps[%d] = %q, want %q", i, comps[i], want[i])
		}
	}
}

func TestLeanPathComponentsExtrasFirst(t *testing.T) {
	tmp := t.TempDir()
	extra := filepath.Join(tmp, "extra")
	if err := os.MkdirAll(extra, 0o755); err != nil {
		t.Fatal(err)
	}
	comps := LeanPathComponents(tmp, "", []string{extra})
	want := pyResolve(extra)
	if len(comps) != 1 || comps[0] != want {
		t.Fatalf("comps = %v, want [%s]", comps, want)
	}
}

func TestLeanPathComponentsBadManifestIgnored(t *testing.T) {
	tmp := t.TempDir()
	if err := os.WriteFile(filepath.Join(tmp, "lake-manifest.json"), []byte("{not json"), 0o644); err != nil {
		t.Fatal(err)
	}
	comps := LeanPathComponents(tmp, filepath.Join(tmp, "lake-manifest.json"), nil)
	if len(comps) != 0 {
		t.Fatalf("comps = %v, want empty", comps)
	}
}

func TestExpandUser(t *testing.T) {
	home, err := os.UserHomeDir()
	if err != nil {
		t.Skip("no home directory")
	}
	if got := expandUser("~"); got != home {
		t.Errorf("expandUser(~) = %q, want %q", got, home)
	}
	if got := expandUser("~/x"); got != filepath.Join(home, "x") {
		t.Errorf("expandUser(~/x) = %q", got)
	}
	if got := expandUser("plain"); got != "plain" {
		t.Errorf("expandUser(plain) = %q", got)
	}
}

func isWindows() bool {
	return os.PathSeparator == '\\'
}
