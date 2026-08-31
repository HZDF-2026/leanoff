package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func mkdir(dir string) error {
	return os.MkdirAll(dir, 0o755)
}

func writeStubLean(bin string) error {
	name := "lean"
	if os.PathSeparator == '\\' {
		name = "lean.exe"
	}
	return os.WriteFile(filepath.Join(bin, name), nil, 0o644)
}

func TestRunHelp(t *testing.T) {
	var out, errBuf bytes.Buffer
	for _, arg := range []string{"-h", "--help", "help"} {
		out.Reset()
		if code := run([]string{arg}, &out, &errBuf); code != 0 {
			t.Fatalf("run(%q) code = %d, want 0", arg, code)
		}
		if !strings.Contains(out.String(), "usage: leanoff {verify,build} [options]") {
			t.Fatalf("run(%q) help text missing usage", arg)
		}
	}
}

func TestRunNoCommand(t *testing.T) {
	var out, errBuf bytes.Buffer
	code := run(nil, &out, &errBuf)
	if code != 2 {
		t.Fatalf("code = %d, want 2", code)
	}
	if !strings.Contains(errBuf.String(), "required: cmd") {
		t.Fatalf("stderr = %q", errBuf.String())
	}
}

func TestRunUnknownCommand(t *testing.T) {
	var out, errBuf bytes.Buffer
	code := run([]string{"frobnicate"}, &out, &errBuf)
	if code != 2 {
		t.Fatalf("code = %d, want 2", code)
	}
	if !strings.Contains(errBuf.String(), `unknown command "frobnicate"`) {
		t.Fatalf("stderr = %q", errBuf.String())
	}
}

func TestRunUnknownFlag(t *testing.T) {
	var out, errBuf bytes.Buffer
	code := run([]string{"verify", "--wat"}, &out, &errBuf)
	if code != 2 {
		t.Fatalf("code = %d, want 2", code)
	}
	if !strings.Contains(errBuf.String(), "unrecognized arguments: --wat") {
		t.Fatalf("stderr = %q", errBuf.String())
	}
	// --flag=value form must be validated too
	errBuf.Reset()
	code = run([]string{"verify", "--wat=1"}, &out, &errBuf)
	if code != 2 {
		t.Fatalf("code = %d, want 2", code)
	}
	if !strings.Contains(errBuf.String(), "unrecognized arguments: --wat=1") {
		t.Fatalf("stderr = %q", errBuf.String())
	}
}

func TestRunInvalidFormat(t *testing.T) {
	var out, errBuf bytes.Buffer
	code := run([]string{"verify", "--format", "yaml"}, &out, &errBuf)
	if code != 2 {
		t.Fatalf("code = %d, want 2", code)
	}
	if !strings.Contains(errBuf.String(), "invalid --format choice: 'yaml'") {
		t.Fatalf("stderr = %q", errBuf.String())
	}
}

func TestRunInvalidJobs(t *testing.T) {
	var out, errBuf bytes.Buffer
	if code := run([]string{"verify", "--jobs", "zero"}, &out, &errBuf); code != 2 {
		t.Fatalf("non-numeric jobs: code = %d, want 2", code)
	}
	errBuf.Reset()
	if code := run([]string{"verify", "--jobs", "0"}, &out, &errBuf); code != 2 {
		t.Fatalf("zero jobs: code = %d, want 2", code)
	}
}

func TestRunFlagMissingValue(t *testing.T) {
	var out, errBuf bytes.Buffer
	code := run([]string{"verify", "--root"}, &out, &errBuf)
	if code != 2 {
		t.Fatalf("code = %d, want 2", code)
	}
	if !strings.Contains(errBuf.String(), "argument root: expected one argument") {
		t.Fatalf("stderr = %q", errBuf.String())
	}
}

func TestRunVerifyNoModules(t *testing.T) {
	// A lean binary is not needed: verify exits with the "no modules"
	// message before any elaboration when the root has no .lean files —
	// but it resolves the toolchain first, so point --lean at a stub.
	tmp := t.TempDir()
	bin := tmp + "/bin"
	if err := mkdir(bin); err != nil {
		t.Fatal(err)
	}
	if err := writeStubLean(bin); err != nil {
		t.Fatal(err)
	}
	var out, errBuf bytes.Buffer
	code := run([]string{"verify", "--root", tmp, "--lean", bin, "--format", "json"}, &out, &errBuf)
	if code != 0 {
		t.Fatalf("code = %d, want 0 (stderr: %q)", code, errBuf.String())
	}
	if !strings.HasPrefix(out.String(), "no .lean modules found under ") {
		t.Fatalf("stdout = %q", out.String())
	}
}

func TestRunVerifyNoToolchain(t *testing.T) {
	var out, errBuf bytes.Buffer
	empty := t.TempDir() + "/emptybin"
	if err := mkdir(empty); err != nil {
		t.Fatal(err)
	}
	code := run([]string{"verify", "--root", t.TempDir(), "--lean", empty}, &out, &errBuf)
	if code != 1 {
		t.Fatalf("code = %d, want 1", code)
	}
	if !strings.Contains(errBuf.String(), "leanoff: lean executable not found:") {
		t.Fatalf("stderr = %q", errBuf.String())
	}
}
