// Command leanoff runs offline verification and minimal builds for Lean 4
// projects: elaborate source files and report errors, warnings, sorry —
// with plain `lean`, a LEAN_PATH assembled from what is already on disk.
// No git, no network, no lake.
//
// Exit codes are machine-readable: 0 ok, 1 failures, 2 usage error.
package main

import (
	"fmt"
	"io"
	"os"
	"runtime"
	"strconv"
	"strings"

	"github.com/HZDF-2026/leanoff/internal/leanoff"
)

const usage = `usage: leanoff {verify,build} [options]

commands:
  verify    elaborate modules against existing oleans, report
  build     compile project modules to oleans in dependency order

options:
  --root DIR        project root (default: cwd)
  --lean PATH       path to lean executable or its bin directory
  --lean-path DIR   extra LEAN_PATH directory (repeatable, e.g. a local Mathlib checkout)
  --filter REGEX    only modules whose name matches this regex
  --jobs N          parallel lean processes (default: min(8, cpus))
  --allow-sorry     do not fail on sorry
  --format FORMAT   text (default) or json
  --out DIR         olean output directory, build only (default: <root>/.leanoff/olean)
  -h, --help        show this help`

func main() {
	os.Exit(run(os.Args[1:], os.Stdout, os.Stderr))
}

func run(args []string, stdout, stderr io.Writer) int {
	if len(args) == 0 {
		return usageErr(stderr, "the following arguments are required: cmd")
	}
	cmd := args[0]
	if cmd == "-h" || cmd == "--help" || cmd == "help" {
		fmt.Fprintln(stdout, usage)
		return 0
	}
	if cmd != "verify" && cmd != "build" {
		return usageErr(stderr, fmt.Sprintf("unknown command %q", cmd))
	}
	o, code := parseOptions(args[1:], stdout, stderr)
	if code >= 0 {
		return code
	}
	eng := &leanoff.Engine{Out: stdout}
	var err error
	if cmd == "verify" {
		code, err = eng.Verify(o)
	} else {
		code, err = eng.Build(o)
	}
	if err != nil {
		fmt.Fprintln(stderr, err)
		return 1
	}
	return code
}

func parseOptions(args []string, stdout, stderr io.Writer) (leanoff.Options, int) {
	o := leanoff.Options{Root: ".", Format: "text", Jobs: defaultJobs()}
	i := 0
	for i < len(args) {
		a := args[i]
		i++
		if a == "--allow-sorry" {
			o.AllowSorry = true
			continue
		}
		if a == "-h" || a == "--help" {
			fmt.Fprintln(stdout, usage)
			return o, 0
		}
		name, val, hasVal := a, "", false
		if strings.HasPrefix(a, "--") {
			if eq := strings.Index(a, "="); eq >= 0 {
				name, val, hasVal = a[:eq], a[eq+1:], true
			}
		}
		if !hasVal {
			switch name {
			case "--root", "--lean", "--lean-path", "--filter", "--format", "--jobs", "--out":
				if i >= len(args) {
					return o, usageErr(stderr, "argument "+strings.TrimPrefix(name, "--")+": expected one argument")
				}
				val, hasVal, i = args[i], true, i+1
			default:
				return o, usageErr(stderr, "unrecognized arguments: "+a)
			}
		}
		switch name {
		case "--root":
			o.Root = val
		case "--lean":
			o.Lean = val
		case "--lean-path":
			o.LeanPath = append(o.LeanPath, val)
		case "--filter":
			o.Filter = val
		case "--out":
			o.Out = val
		case "--format":
			if val != "text" && val != "json" {
				return o, usageErr(stderr, "invalid --format choice: '"+val+"' (choose from 'text', 'json')")
			}
			o.Format = val
		case "--jobs":
			n, err := strconv.Atoi(val)
			if err != nil || n < 1 {
				return o, usageErr(stderr, "invalid --jobs value: "+val)
			}
			o.Jobs = n
		default:
			return o, usageErr(stderr, "unrecognized arguments: "+a)
		}
	}
	return o, -1
}

func usageErr(stderr io.Writer, msg string) int {
	fmt.Fprintf(stderr, "usage: leanoff {verify,build} [options]\nleanoff: error: %s\n", msg)
	return 2
}

func defaultJobs() int {
	n := runtime.NumCPU()
	if n > 8 {
		return 8
	}
	if n < 1 {
		return 4
	}
	return n
}
