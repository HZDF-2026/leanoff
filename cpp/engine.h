// engine.h — verify/build orchestration: the port of internal/leanoff/
// commands.go (cmd_verify / cmd_build / report of leanoff.py).
#ifndef LEANOFF_ENGINE_H
#define LEANOFF_ENGINE_H

#include <ostream>
#include <string>
#include <vector>

#include "runlean.h"

namespace leanoff {

// Options mirrors the Python CLI flags.
struct Options {
    std::string root = ".";
    std::string lean;
    std::vector<std::string> leanPath;
    std::string filter;
    int jobs = 0;
    bool allowSorry = false;
    std::string format = "text";
    std::string out;  // build only
};

// Engine runs verify/build. runLean is injectable so tests can exercise
// the scheduling layer without a real toolchain.
struct Engine {
    std::ostream* out = nullptr;
    RunLeanFunc runLeanFn = &runLean;

    // Both throw std::runtime_error for setup failures (no lean, bad
    // --filter, import cycle); the caller prints the message and exits 1.
    // Returns the report's exit code: 1 if any module failed, else 0.
    int verify(const Options& o);
    int build(const Options& o);
};

// Prints the text or JSON report and returns the exit code (1 if any module
// failed). Exposed for the reference-table tests.
int report(std::vector<Result> results, double wall, const std::string& format,
           std::ostream& w);

// Renders the report exactly as Python's json.dumps(..., indent=2).
std::string reportJSON(const std::vector<Result>& results, int failed, double wall);

}  // namespace leanoff

#endif  // LEANOFF_ENGINE_H
