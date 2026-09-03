// runlean.h — elaborating one module with the real lean binary: the port of
// internal/leanoff/run.go (run_lean of leanoff.py).
#ifndef LEANOFF_RUNLEAN_H
#define LEANOFF_RUNLEAN_H

#include <string>

#include "module.h"

namespace leanoff {

// The outcome of elaborating one module. Field names and JSON shape mirror
// the Python reference's dataclass exactly.
struct Result {
    std::string name;
    bool ok = true;
    int errors = 0;
    int warnings = 0;
    int sorries = 0;
    double seconds = 0.0;
    std::string firstError;
};

using RunLeanFunc = Result (*)(const std::string& exe, const std::string& root,
                               const Module& mod, const std::string& leanPath,
                               const std::string& outOlean, bool cwdRoot);

// Elaborates mod with lean: combined stdout/stderr, LEAN_PATH set, a 3600s
// timeout. A lean that dies (timeout, spawn failure) becomes a FAIL result,
// never a crash.
Result runLean(const std::string& exe, const std::string& root, const Module& mod,
               const std::string& leanPath, const std::string& outOlean,
               bool cwdRoot);

// Counts error/warning/sorry lines in lean's output and captures the first
// error line (": error", ": warning", "uses 'sorry'").
struct LeanCounts {
    int errors = 0;
    int warnings = 0;
    int sorries = 0;
    std::string first;
};
LeanCounts parseLeanOutput(const std::string& out);

// Classify decides pass/fail: any error, a non-zero lean exit, or (unless
// allowed) a sorry means failure.
bool classify(const Result& r, bool allowSorry);

}  // namespace leanoff

#endif  // LEANOFF_RUNLEAN_H
