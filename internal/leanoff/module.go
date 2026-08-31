package leanoff

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

// Module is one .lean file: its import name, its path, and (after
// ParseImports) its project-internal dependencies.
type Module struct {
	Name string
	Path string
	Deps []string
}

var importRe = regexp.MustCompile(`^\s*import\s+([A-Za-z_][A-Za-z0-9_.]*)`)

var skipDirs = map[string]bool{
	".lake": true, ".git": true, ".vscode": true, "node_modules": true,
	"dist": true, "out": true, "build": true, ".leanoff": true,
}

// DiscoverModules lists every .lean file under root as a module, optionally
// filtered by a regex on the module name. Results are sorted by name so the
// output does not depend on directory iteration order.
func DiscoverModules(root string, pattern string) ([]Module, error) {
	var re *regexp.Regexp
	if pattern != "" {
		var err error
		if re, err = regexp.Compile(pattern); err != nil {
			return nil, fmt.Errorf("leanoff: invalid --filter regex: %v", err)
		}
	}
	mods := map[string]string{}
	_ = filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil // os.walk silently skips unreadable directories
		}
		if d.IsDir() {
			if path != root && skipDirs[d.Name()] {
				return filepath.SkipDir
			}
			return nil
		}
		if !strings.HasSuffix(d.Name(), ".lean") {
			return nil
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return nil
		}
		name := moduleNameFromRel(rel)
		if re != nil && !re.MatchString(name) {
			return nil
		}
		mods[name] = path
		return nil
	})
	names := make([]string, 0, len(mods))
	for n := range mods {
		names = append(names, n)
	}
	sort.Strings(names)
	out := make([]Module, 0, len(names))
	for _, n := range names {
		out = append(out, Module{Name: n, Path: mods[n]})
	}
	return out, nil
}

// moduleNameFromRel turns "Dir/Sub/Mod.lean" into "Dir.Sub.Mod", mirroring
// the Python reference: rel.with_suffix("").replace(os.sep, "."). A leading
// dot in the final component is not a suffix, so a file literally named
// ".lean" keeps its name.
func moduleNameFromRel(rel string) string {
	return strings.ReplaceAll(stripPySuffix(rel), string(filepath.Separator), ".")
}

func stripPySuffix(p string) string {
	lastDot := strings.LastIndex(p, ".")
	lastSep := strings.LastIndexAny(p, `/\`)
	if lastDot > lastSep+1 {
		return p[:lastDot]
	}
	return p
}

// ParseImports fills mod.Deps with the module's project-internal imports:
// every `import X` line whose target is a discovered module and not the
// module itself.
func ParseImports(mod *Module, known map[string]bool) {
	data, err := os.ReadFile(mod.Path)
	if err != nil {
		return
	}
	var deps []string
	for _, line := range pySplitLines(string(data)) {
		m := importRe.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		if known[m[1]] && m[1] != mod.Name {
			deps = append(deps, m[1])
		}
	}
	mod.Deps = deps
}

// TopoLevels groups modules into dependency levels; each level is
// parallelizable. Returns an error on import cycles instead of exiting.
func TopoLevels(mods []Module) ([][]Module, error) {
	remaining := make(map[string]map[string]bool, len(mods))
	byName := make(map[string]Module, len(mods))
	for _, m := range mods {
		s := make(map[string]bool, len(m.Deps))
		for _, d := range m.Deps {
			s[d] = true
		}
		remaining[m.Name] = s
		byName[m.Name] = m
	}
	done := map[string]bool{}
	var levels [][]Module
	for len(remaining) > 0 {
		var ready []string
		for n, ds := range remaining {
			ok := true
			for d := range ds {
				if !done[d] {
					ok = false
					break
				}
			}
			if ok {
				ready = append(ready, n)
			}
		}
		if len(ready) == 0 {
			names := make([]string, 0, len(remaining))
			for n := range remaining {
				names = append(names, n)
			}
			sort.Strings(names)
			return nil, fmt.Errorf("leanoff: import cycle among: %s", strings.Join(names, ", "))
		}
		sort.Strings(ready)
		level := make([]Module, 0, len(ready))
		for _, n := range ready {
			level = append(level, byName[n])
			done[n] = true
			delete(remaining, n)
		}
		levels = append(levels, level)
	}
	return levels, nil
}
