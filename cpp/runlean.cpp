// runlean.cpp — see runlean.h.
#include "runlean.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "pycompat.h"
#include "toolchain.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#ifdef __APPLE__
#include <crt_externs.h>
static char** envpEnviron() { return *_NSGetEnviron(); }
#else
extern "C" char** environ;
static char** envpEnviron() { return environ; }
#endif
#endif

namespace leanoff {

namespace {

std::string dirnameOf(const std::string& p) {
    size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    if (pos == 0) return p.substr(0, 1);
    return p.substr(0, pos);
}

// filepath.Rel(root, path) for the module argument; falls back to the full
// path like the Go port does on error (never happens for discovered modules).
std::string relToRoot(const std::string& root, const std::string& path) {
    if (path.rfind(root, 0) == 0 && path.size() > root.size() &&
        (path[root.size()] == '/' || path[root.size()] == '\\')) {
        return path.substr(root.size() + 1);
    }
    if (path == root) return ".";
    return path;
}

struct ProcOutcome {
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    std::string output;
};

#ifdef _WIN32

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                 nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// list2cmdline-style quoting: quote when spaces/tabs/quotes are present,
// doubling embedded quotes.
std::string quoteArg(const std::string& a) {
    if (a.empty()) return "\"\"";
    bool need = false;
    for (char c : a) {
        if (c == ' ' || c == '\t' || c == '"') {
            need = true;
            break;
        }
    }
    if (!need) return a;
    std::string out = "\"";
    for (char c : a) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

ProcOutcome runProcess(const std::string& exe, const std::vector<std::string>& args,
                      const std::string& cwd, const std::string& leanPath,
                      unsigned timeoutSec) {
    ProcOutcome o;
    // Command line: [quoted exe, quoted args...]; lpApplicationName stays NULL
    // so batch-file stubs (lean.bat) execute like they do under Python/Go.
    std::string cmdUtf8 = quoteArg(exe);
    for (const auto& a : args) cmdUtf8 += " " + quoteArg(a);
    std::wstring wcmd = utf8ToWide(cmdUtf8);
    std::wstring wcwd = utf8ToWide(cwd);

    // Environment: current block with LEAN_PATH replaced/appended.
    std::wstring envBlock;
    {
        LPWCH env = GetEnvironmentStringsW();
        std::wstring rebuilt;
        bool hasLeanPath = false;
        if (env) {
            for (LPWCH p = env; *p; p += wcslen(p) + 1) {
                std::wstring var(p);
                if (_wcsnicmp(p, L"LEAN_PATH=", 10) == 0) {
                    if (!hasLeanPath) {  // first definition wins, like a dict
                        rebuilt += L"LEAN_PATH=" + utf8ToWide(leanPath) + L'\0';
                        hasLeanPath = true;
                    }
                    continue;
                }
                rebuilt += var + L'\0';
            }
            FreeEnvironmentStringsW(env);
        }
        if (!hasLeanPath) rebuilt += L"LEAN_PATH=" + utf8ToWide(leanPath) + L'\0';
        rebuilt += L'\0';
        envBlock = std::move(rebuilt);
    }

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rPipe = nullptr, wPipe = nullptr;
    if (!CreatePipe(&rPipe, &wPipe, &sa, 0)) return o;
    SetHandleInformation(rPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wPipe;
    si.hStdError = wPipe;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(wcmd.begin(), wcmd.end());
    cmdBuf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                              CREATE_UNICODE_ENVIRONMENT, (LPVOID)envBlock.data(),
                              wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(wPipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rPipe);
        return o;
    }
    o.started = true;

    // Reader thread: drains the merged stdout/stderr pipe.
    std::string out;
    std::thread reader([rPipe, &out]() {
        char buf[8192];
        DWORD n = 0;
        for (;;) {
            if (!ReadFile(rPipe, buf, sizeof buf, &n, nullptr) || n == 0) break;
            out.append(buf, n);
        }
    });

    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutSec * 1000ULL);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        o.timedOut = true;
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    o.exitCode = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    reader.join();
    CloseHandle(rPipe);
    o.output = std::move(out);
    return o;
}

#else  // POSIX

ProcOutcome runProcess(const std::string& exe, const std::vector<std::string>& args,
                      const std::string& cwd, const std::string& leanPath,
                      unsigned timeoutSec) {
    ProcOutcome o;
    int outPipe[2];
    if (pipe(outPipe) != 0) return o;

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    // envp: current environment with LEAN_PATH replaced.
    std::vector<std::string> envStore;
    for (char** e = envpEnviron(); e && *e; e++) {
        if (strncmp(*e, "LEAN_PATH=", 10) == 0) continue;
        envStore.emplace_back(*e);
    }
    envStore.emplace_back("LEAN_PATH=" + leanPath);
    std::vector<char*> envp;
    for (auto& e : envStore) envp.push_back(&e[0]);
    envp.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        // child
        close(outPipe[0]);
        dup2(outPipe[1], 1);
        dup2(outPipe[1], 2);
        close(outPipe[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, 0);
            if (devnull > 2) close(devnull);
        }
        if (chdir(cwd.c_str()) != 0) _exit(127);
        execve(exe.c_str(), argv.data(), envp.data());
        _exit(127);  // exec failed
    }
    if (pid < 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        return o;
    }
    close(outPipe[1]);
    o.started = true;

    std::string out;
    std::thread reader([fd = outPipe[0], &out]() {
        char buf[8192];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
        close(fd);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            o.timedOut = true;
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (WIFEXITED(status)) {
        o.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        o.exitCode = -WTERMSIG(status);  // Python's returncode convention
    }
    reader.join();
    o.output = std::move(out);
    return o;
}

#endif  // _WIN32

}  // namespace

LeanCounts parseLeanOutput(const std::string& out) {
    LeanCounts c;
    for (const auto& line : pySplitLines(out)) {
        if (line.find(": error") != std::string::npos) {
            c.errors++;
            if (c.first.empty()) c.first = line;
        }
        if (line.find(": warning") != std::string::npos) c.warnings++;
        if (line.find("uses 'sorry'") != std::string::npos) c.sorries++;
    }
    return c;
}

bool classify(const Result& r, bool allowSorry) {
    if (r.errors > 0 || !r.ok) return false;
    if (r.sorries > 0 && !allowSorry) return false;
    return true;
}

Result runLean(const std::string& exe, const std::string& root, const Module& mod,
               const std::string& leanPath, const std::string& outOlean,
               bool cwdRoot) {
    std::vector<std::string> args;
    if (!outOlean.empty()) args.push_back("--o=" + outOlean);
    std::string modPath = cwdRoot ? relToRoot(root, mod.path) : mod.path;
    args.push_back(modPath);
    std::string cwd = cwdRoot ? root : dirnameOf(mod.path);

    auto t0 = std::chrono::steady_clock::now();
    ProcOutcome p = runProcess(exe, args, cwd, leanPath, 3600);
    if (!p.started) {
        return Result{mod.name, false, 1, 0, 0, 0.0,
                      "leanoff: lean died: OSError: cannot execute " + exe};
    }
    if (p.timedOut) {
        return Result{mod.name, false, 1, 0, 0, 0.0,
                      "leanoff: lean died: TimeoutExpired: command timed out after 3600 seconds"};
    }
    double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LeanCounts c = parseLeanOutput(p.output);
    return Result{mod.name, p.exitCode == 0, c.errors, c.warnings, c.sorries, seconds,
                  c.first};
}

}  // namespace leanoff
