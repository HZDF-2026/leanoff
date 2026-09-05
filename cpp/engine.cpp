// engine.cpp — see engine.h. The scheduling mirrors the Python reference:
// verify fans every module out to a bounded pool; build walks the dependency
// levels and a failed level stops the build (modules below it would only
// cascade spurious errors), with queued work in the failed level skipped.
#include "engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "module.h"
#include "pycompat.h"
#include "runlean.h"
#include "toolchain.h"
#include <cstdio>

namespace leanoff {

namespace {

namespace fs = std::filesystem;

const char kPathSep =
#ifdef _WIN32
    '\\';
#else
    '/';
#endif

// os.pathsep: the LEAN_PATH list separator.
const char kListSep =
#ifdef _WIN32
    ';';
#else
    ':';
#endif

int clampJobs(int n) {
    if (n < 1) return 1;
    return n;
}

std::string joinPathList(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) out += kListSep;
        out += parts[i];
    }
    return out;
}

// f"{s:<{w}}": left-aligned (name, status).
std::string padLeft(const std::string& s, size_t w) {
    size_t n = utf8RuneCount(s);
    if (n >= w) return s;
    return s + std::string(w - n, ' ');
}

// f"{s:>{w}}": right-aligned (err, warn, sorry, time).
std::string padRight(const std::string& s, size_t w) {
    size_t n = utf8RuneCount(s);
    if (n >= w) return s;
    return std::string(w - n, ' ') + s;
}

// A bounded worker pool over mods. Results appear in completion order (the
// report sorts by name). cancelOnFail mirrors the build path: once a module
// fails, queued modules are skipped (Python cancels the pending futures).
std::vector<Result> runPool(const std::vector<Module>& mods, int jobs, RunLeanFunc fn,
                            const std::string& exe, const std::string& root,
                            const std::string& leanPath, const std::string& outdir,
                            bool cancelOnFail, bool allowSorry) {
    std::vector<Result> results;
    std::mutex mu;
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    size_t nworkers = std::min<size_t>(static_cast<size_t>(clampJobs(jobs)), mods.size());
    std::vector<std::thread> workers;
    workers.reserve(nworkers);
    for (size_t w = 0; w < nworkers; w++) {
        workers.emplace_back([&]() {
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= mods.size()) return;
                if (cancelOnFail && failed.load()) continue;  // cancelled: drain
                const Module& mod = mods[i];
                std::string olean;
                if (!outdir.empty()) {
                    std::string rel = mod.name;
                    for (char& c : rel) {
                        if (c == '.') c = kPathSep;
                    }
                    olean = outdir + kPathSep + rel + ".olean";
                    std::error_code ec;
                    fs::path parent = fs::u8path(olean).parent_path();
                    if (!parent.empty()) fs::create_directories(parent, ec);
                }
                Result r;
                try {
                    r = fn(exe, root, mod, leanPath, olean, true);
                } catch (const std::exception& e) {
                    r = Result{mod.name, false, 1, 0, 0, 0.0,
                               std::string("leanoff: lean died: ") + e.what()};
                }
                r.ok = classify(r, allowSorry);
                {
                    std::lock_guard<std::mutex> lock(mu);
                    results.push_back(r);
                    if (!r.ok) failed.store(true);
                }
            }
        });
    }
    for (auto& t : workers) t.join();
    return results;
}

}  // namespace

int Engine::verify(const Options& o) {
    std::string root = pyResolve(o.root);
    Toolchain tc = findToolchain(o.lean);
    std::vector<std::string> comps =
        leanPathComponents(root, root + kPathSep + "lake-manifest.json", o.leanPath);
    std::vector<std::string> all = comps;
    all.push_back(tc.lib);
    std::string leanPath = joinPathList(all);
    std::vector<Module> mods = discoverModules(root, o.filter);
    if (mods.empty()) {
        std::string msg = "no .lean modules found under " + root;
        if (!o.filter.empty()) msg += " matching '" + o.filter + "'";
        if (out) *out << msg << "\n";
        return 0;
    }
    std::set<std::string> known;
    for (const auto& m : mods) known.insert(m.name);
    for (auto& m : mods) parseImports(m, known);

    auto t0 = std::chrono::steady_clock::now();
    std::vector<Result> results =
        runPool(mods, o.jobs, runLeanFn, tc.exe, root, leanPath, "", false, o.allowSorry);
    double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return report(results, wall, o.format, out ? *out : std::cout);
}

int Engine::build(const Options& o) {
    std::string root = pyResolve(o.root);
    Toolchain tc = findToolchain(o.lean);
    std::string outdir = pyResolve(root + kPathSep + ".leanoff" + kPathSep + "olean");
    if (!o.out.empty()) outdir = pyResolve(expandUser(o.out));
    std::vector<std::string> comps =
        leanPathComponents(root, root + kPathSep + "lake-manifest.json", o.leanPath);
    std::vector<std::string> all;
    all.push_back(outdir);
    for (const auto& c : comps) all.push_back(c);
    all.push_back(tc.lib);
    std::string leanPath = joinPathList(all);
    std::vector<Module> mods = discoverModules(root, o.filter);
    if (mods.empty()) {
        if (out) *out << "no .lean modules found under " << root << "\n";
        return 0;
    }
    std::set<std::string> known;
    for (const auto& m : mods) known.insert(m.name);
    for (auto& m : mods) parseImports(m, known);
    std::vector<std::vector<Module>> levels = topoLevels(mods);

    auto t0 = std::chrono::steady_clock::now();
    std::vector<Result> results;
    for (const auto& level : levels) {
        std::vector<Result> rs = runPool(level, o.jobs, runLeanFn, tc.exe, root,
                                          leanPath, outdir, true, o.allowSorry);
        results.insert(results.end(), rs.begin(), rs.end());
        bool failed = false;
        for (const auto& r : rs) {
            if (!r.ok) {
                failed = true;
                break;
            }
        }
        if (failed) break;  // stop the build on a failed level
    }
    double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return report(results, wall, o.format, out ? *out : std::cout);
}

int report(std::vector<Result> results, double wall, const std::string& format,
           std::ostream& w) {
    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b) { return a.name < b.name; });
    int failed = 0;
    for (const auto& r : results) {
        if (!r.ok) failed++;
    }
    if (format == "json") {
        w << reportJSON(results, failed, wall) << "\n";
        return failed > 0 ? 1 : 0;
    }
    // max(len(r.name) for r in results, default=8): the default only applies
    // to an empty result list, so short names make narrow columns.
    size_t width = results.empty() ? 8 : 0;
    for (const auto& r : results) {
        size_t n = utf8RuneCount(r.name);
        if (n > width) width = n;
    }
    char buf[64];
    w << padLeft("module", width) << "  " << padLeft("status", 6) << " "
      << padRight("err", 3) << " " << padRight("warn", 4) << " "
      << padRight("sorry", 5) << " " << padRight("time", 7) << "\n";
    int totE = 0, totW = 0, totS = 0;
    for (const auto& r : results) {
        std::string status = r.ok ? "PASS" : "FAIL";
        std::snprintf(buf, sizeof buf, "%6.1f", r.seconds);
        w << padLeft(r.name, width) << "  " << padLeft(status, 6) << " "
          << padRight(std::to_string(r.errors), 3) << " "
          << padRight(std::to_string(r.warnings), 4) << " "
          << padRight(std::to_string(r.sorries), 5) << " " << buf << "s\n";
        if (!r.ok && !r.firstError.empty()) {
            w << "    " << truncateRunes(pyStrip(r.firstError), 160) << "\n";
        }
        totE += r.errors;
        totW += r.warnings;
        totS += r.sorries;
    }
    std::snprintf(buf, sizeof buf, "%.1f", wall);
    w << "\n" << results.size() << " modules: " << failed << " failed, " << totE
      << " errors, " << totW << " warnings, " << totS << " sorry  (" << buf
      << "s wall)\n";
    return failed > 0 ? 1 : 0;
}

// Renders the report exactly as Python's json.dumps(..., indent=2):
// ASCII-escaped strings, insertion-ordered fields, two-space indent.
std::string reportJSON(const std::vector<Result>& results, int failed, double wall) {
    std::string b = "{\n  \"modules\": [";
    for (size_t i = 0; i < results.size(); i++) {
        if (i > 0) b += ",";
        const Result& r = results[i];
        b += "\n    {\n";
        b += "      \"name\": " + pyStr(r.name) + ",\n";
        b += std::string("      \"ok\": ") + (r.ok ? "true" : "false") + ",\n";
        b += "      \"errors\": " + std::to_string(r.errors) + ",\n";
        b += "      \"warnings\": " + std::to_string(r.warnings) + ",\n";
        b += "      \"sorries\": " + std::to_string(r.sorries) + ",\n";
        b += "      \"seconds\": " + pyFloat(r.seconds) + ",\n";
        b += "      \"first_error\": " + pyStr(r.firstError) + "\n";
        b += "    }";
    }
    if (!results.empty()) b += "\n  ],\n";
    else b += "],\n";
    b += "  \"failed\": " + std::to_string(failed) + ",\n";
    b += "  \"wall_seconds\": " + pyFloat(round1(wall)) + "\n";
    b += "}";
    return b;
}

}  // namespace leanoff
