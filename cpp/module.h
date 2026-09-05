// module.h — project module discovery, import parsing, and dependency
// levelization: the port of internal/leanoff/module.go (which ports the
// same functions of leanoff.py).
#ifndef LEANOFF_MODULE_H
#define LEANOFF_MODULE_H

#include <set>
#include <string>
#include <vector>
#include <stdexcept>

namespace leanoff {

struct Module {
    std::string name;
    std::string path;
    std::vector<std::string> deps;  // project-internal deps only
};

// All .lean files under root as modules, optionally filtered by a Python
// regex on the module name. Sorted by name so output never depends on
// directory iteration order. Throws std::runtime_error on a bad --filter.
std::vector<Module> discoverModules(const std::string& root, const std::string& filter);

// "Dir/Sub/Mod.lean" -> "Dir.Sub.Mod" (rel.with_suffix("").replace(os.sep, ".")).
std::string moduleNameFromRel(const std::string& rel);

// Fills mod.deps with the module's project-internal imports.
void parseImports(Module& mod, const std::set<std::string>& known);

// Modules grouped into dependency levels; each level is parallelizable.
// Throws std::runtime_error on an import cycle.
std::vector<std::vector<Module>> topoLevels(const std::vector<Module>& mods);

}  // namespace leanoff

#endif  // LEANOFF_MODULE_H
