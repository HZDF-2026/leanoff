package leanoff

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

// lookPath is exec.LookPath as a variable so tests can stub PATH lookup.
var lookPath = exec.LookPath

// FindToolchain resolves (lean executable, toolchain lib dir). spec is a
// path to the lean binary or its bin directory; empty means "lean on PATH".
func FindToolchain(spec string) (string, string, error) {
	var exe string
	if spec == "" {
		lp, err := lookPath("lean")
		if err != nil {
			return "", "", errors.New("leanoff: no `lean` on PATH and no --lean given")
		}
		exe = pyResolve(lp)
	} else {
		p := expandUser(spec)
		if isDir(p) {
			name := "lean"
			if runtime.GOOS == "windows" {
				name = "lean.exe"
			}
			exe = filepath.Join(p, name)
		} else {
			exe = p
		}
		if !fileExists(exe) {
			return "", "", fmt.Errorf("leanoff: lean executable not found: %s", exe)
		}
		exe = pyResolve(exe)
	}
	lib := filepath.Join(filepath.Dir(filepath.Dir(exe)), "lib", "lean")
	return exe, lib, nil
}

// LeanPathComponents assembles the LEAN_PATH directory list: extras,
// leanoff's own oleans, project oleans, then package oleans in manifest
// order.
func LeanPathComponents(root string, manifestPath string, extra []string) []string {
	var comps []string
	for _, e := range extra {
		comps = append(comps, pyResolve(expandUser(e)))
	}
	own := filepath.Join(root, ".leanoff", "olean")
	if isDir(own) {
		comps = append(comps, pyResolve(own))
	}
	proj := filepath.Join(root, ".lake", "build", "lib", "lean")
	if isDir(proj) {
		comps = append(comps, pyResolve(proj))
	}
	if manifestPath != "" {
		if data, err := os.ReadFile(manifestPath); err == nil {
			var m struct {
				PackagesDir string `json:"packagesDir"`
				Packages    []struct {
					Type string `json:"type"`
					Dir  string `json:"dir"`
					Name string `json:"name"`
				} `json:"packages"`
			}
			if json.Unmarshal(data, &m) == nil {
				pd := m.PackagesDir
				if pd == "" {
					pd = ".lake/packages"
				}
				packagesDir := filepath.Join(root, pd)
				for _, entry := range m.Packages {
					var pkgRoot string
					if entry.Type == "path" && entry.Dir != "" {
						pkgRoot = expandUser(entry.Dir)
						if !filepath.IsAbs(pkgRoot) {
							pkgRoot = pyResolve(filepath.Join(root, pkgRoot))
						}
					} else {
						pkgRoot = filepath.Join(packagesDir, entry.Name)
					}
					odir := filepath.Join(pkgRoot, ".lake", "build", "lib", "lean")
					if isDir(odir) {
						comps = append(comps, pyResolve(odir))
					}
				}
			}
		}
	}
	return comps
}

func isDir(p string) bool {
	fi, err := os.Stat(p)
	return err == nil && fi.IsDir()
}

func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

// expandUser expands a leading "~" like Python's Path.expanduser.
func expandUser(p string) string {
	if p == "~" {
		if home, err := os.UserHomeDir(); err == nil {
			return home
		}
		return p
	}
	if strings.HasPrefix(p, "~/") || strings.HasPrefix(p, "~\\") {
		if home, err := os.UserHomeDir(); err == nil {
			return filepath.Join(home, p[2:])
		}
	}
	return p
}

// pyResolve makes a path absolute and resolves symlinks, like Path.resolve().
func pyResolve(p string) string {
	abs, err := filepath.Abs(p)
	if err != nil {
		return p
	}
	if resolved, err := filepath.EvalSymlinks(abs); err == nil {
		return resolved
	}
	return abs
}
