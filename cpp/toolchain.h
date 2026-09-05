// toolchain.h — lean binary discovery and LEAN_PATH assembly: the port of
// internal/leanoff/toolchain.go (find_toolchain / lean_path_components of
// leanoff.py).
#ifndef LEANOFF_TOOLCHAIN_H
#define LEANOFF_TOOLCHAIN_H

#include <string>
#include <vector>
#include <stdexcept>

namespace leanoff {

struct Toolchain {
    std::string exe;  // resolved lean executable
    std::string lib;  // <toolchain>/lib/lean
};

// Resolves the lean executable. spec is a path to lean or its bin directory;
// empty means "lean on PATH". Throws std::runtime_error when nothing is found.
Toolchain findToolchain(const std::string& spec);

// LEAN_PATH directories: extras, leanoff's own oleans, project oleans,
// package oleans (manifest order).
std::vector<std::string> leanPathComponents(const std::string& root,
                                            const std::string& manifestPath,
                                            const std::vector<std::string>& extra);

// ~ expansion like Path.expanduser.
std::string expandUser(const std::string& p);

// Absolute path with symlinks resolved, like Path.resolve().
std::string pyResolve(const std::string& p);

bool isDir(const std::string& p);
bool fileExists(const std::string& p);

}  // namespace leanoff

#endif  // LEANOFF_TOOLCHAIN_H
