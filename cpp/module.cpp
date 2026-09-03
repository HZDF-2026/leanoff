// module.cpp — see module.h. Mirrors internal/leanoff/module.go and the
// discover_modules / parse_imports / topo_levels functions of leanoff.py.
#include "module.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>

#include "pycompat.h"
#include "pyre.h"

namespace leanoff {
namespace {

namespace fs = std::filesystem;

const char* const kSkipDirs[] = {
    ".lake", ".git", ".vscode", "node_modules", "dist", "out", "build", ".leanoff",
};

bool isSkipDir(const std::string& name) {
    for (const char* d : kSkipDirs) {
        if (name == d) return true;
    }
    return false;
}

std::string pathU8(const fs::path& p) {
    return p.u8string();
}

}  // namespace

std::string moduleNameFromRel(const std::string& rel) {
    // stripPySuffix: Python's with_suffix("") — drop the last dot-suffix only
    // when it lies after the last separator (a leading dot is not a suffix).
    int64_t lastDot = -1;
    int64_t lastSep = -1;
    for (size_t i = 0; i < rel.size(); i++) {
        if (rel[i] == '.') lastDot = static_cast<int64_t>(i);
        if (rel[i] == '/' || rel[i] == '\\') lastSep = static_cast<int64_t>(i);
    }
    std::string stripped = rel;
    if (lastDot > lastSep + 1) stripped = rel.substr(0, static_cast<size_t>(lastDot));
    for (char& c : stripped) {
        if (c == '/' || c == '\\') c = '.';
    }
    return stripped;
}

std::vector<Module> discoverModules(const std::string& root, const std::string& filter) {
    std::unique_ptr<Regexp> re;
    if (!filter.empty()) {
        try {
            re = Regexp::compile(filter);
        } catch (const std::exception& e) {
            throw std::runtime_error("leanoff: invalid --filter regex: " + std::string(e.what()));
        }
    }
    std::map<std::string, std::string> mods;  // name -> path, sorted by name
    const fs::path rootP = fs::u8path(root);

    // os.walk / WalkDir: children in SKIP_DIRS are pruned anywhere in the
    // tree (the root itself is never skipped); unreadable directories are
    // skipped silently. relDir tracks the path relative to root so module
    // names never need fs::relative's canonicalization.
    std::function<void(const fs::path&, const fs::path&)> walk =
        [&](const fs::path& dir, const fs::path& relDir) {
            std::error_code ec;
            fs::directory_iterator it(dir, ec);
            if (ec) return;  // unreadable directory: skip, keep walking siblings
            std::vector<fs::path> subdirs;
            for (const auto& entry : it) {
                std::error_code ec2;
                // Symlinks to directories are not descended (os.walk with
                // followlinks=False, WalkDir's non-following d.IsDir()).
                bool isDir = !entry.is_symlink(ec2) && entry.is_directory(ec2);
                std::string base = pathU8(entry.path().filename());
                if (isDir) {
                    if (isSkipDir(base)) continue;
                    subdirs.push_back(entry.path());
                    continue;
                }
                if (base.size() <= 5 ||
                    base.compare(base.size() - 5, 5, ".lean") != 0)
                    continue;
                fs::path rel = relDir.empty() ? entry.path().filename()
                                              : relDir / entry.path().filename();
                std::string name = moduleNameFromRel(pathU8(rel));
                if (re && !re->find(name).has_value()) continue;
                mods[name] = pathU8(entry.path());
            }
            for (const auto& d : subdirs) {
                fs::path subRel = relDir.empty() ? d.filename() : relDir / d.filename();
                walk(d, subRel);
            }
        };
    walk(rootP, fs::path());

    std::vector<Module> out;
    out.reserve(mods.size());
    for (const auto& kv : mods) {
        out.push_back(Module{kv.first, kv.second, {}});
    }
    return out;
}

void parseImports(Module& mod, const std::set<std::string>& known) {
    std::ifstream in(fs::u8path(mod.path), std::ios::binary);
    if (!in) return;  // unreadable file: no deps, like OSError in Python
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    static const std::unique_ptr<Regexp> importRe = [] {
        return Regexp::compile("^\\s*import\\s+([A-Za-z_][A-Za-z0-9_.]*)");
    }();
    std::vector<std::string> deps;
    for (const auto& line : pySplitLines(data)) {
        auto m = importRe->matchString(line);
        if (!m) continue;
        std::string dep = m->group(1);
        if (known.count(dep) && dep != mod.name) deps.push_back(dep);
    }
    mod.deps = std::move(deps);
}

std::vector<std::vector<Module>> topoLevels(const std::vector<Module>& mods) {
    std::map<std::string, std::set<std::string>> remaining;
    std::map<std::string, Module> byName;
    for (const auto& m : mods) {
        remaining[m.name] = std::set<std::string>(m.deps.begin(), m.deps.end());
        byName[m.name] = m;
    }
    std::set<std::string> done;
    std::vector<std::vector<Module>> levels;
    while (!remaining.empty()) {
        std::vector<std::string> ready;
        for (const auto& kv : remaining) {
            bool ok = true;
            for (const auto& d : kv.second) {
                if (!done.count(d)) {
                    ok = false;
                    break;
                }
            }
            if (ok) ready.push_back(kv.first);
        }
        if (ready.empty()) {
            std::vector<std::string> names;
            for (const auto& kv : remaining) names.push_back(kv.first);
            std::sort(names.begin(), names.end());
            std::string joined;
            for (size_t i = 0; i < names.size(); i++) {
                if (i > 0) joined += ", ";
                joined += names[i];
            }
            throw std::runtime_error("leanoff: import cycle among: " + joined);
        }
        std::sort(ready.begin(), ready.end());
        std::vector<Module> level;
        for (const auto& n : ready) {
            level.push_back(byName[n]);
            done.insert(n);
            remaining.erase(n);
        }
        levels.push_back(std::move(level));
    }
    return levels;
}

}  // namespace leanoff
