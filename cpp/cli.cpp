// cli.cpp — argparse-compatible command line for leanoff, frozen against the
// Python reference (help texts, usage lines, and every error message were
// captured from leanoff.py on CPython 3.12).
//
// Reproduced behaviors (all verified against the reference):
//   - -h/--help and unique prefixes ("--h") print the frozen help, exit 0
//   - options resolve by exact name or unique prefix ("--form" -> --format);
//     a prefix matching several options errors "ambiguous option: ..."
//   - --opt=value and "--opt value" forms; a value that itself looks like an
//     option is refused ("expected one argument") unless it is a lone "-" or
//     a negative number ("-1", "-1.5")
//   - choices (--format) and int (--jobs) validation with argparse's messages
//   - errors fire left-to-right: a help request after a bad option still
//     errors, one before it wins
//   - unrecognized tokens (unknown options, extra positionals, "--" and
//     everything after it) are reported by the TOP parser after a clean
//     subcommand parse: top usage line, "leanoff: error:" prefix
//   - missing/invalid subcommand: "the following arguments are required:
//     cmd" wins over unrecognized extras; "invalid choice" fires as soon as
//     the positional is consumed
#include "cli.h"

#include <cctype>
#include <climits>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "pycompat.h"

namespace leanoff {
namespace {

// ---------------------------------------------------------------------------
// Frozen reference texts (usage lines end with "\n"; help texts are the full
// "-h" output).

const char* const kTopUsage = "usage: leanoff [-h] {verify,build} ...\n";

const char* const kTopHelp =
    "usage: leanoff [-h] {verify,build} ...\n"
    "\n"
    "leanoff — offline verification and minimal builds for Lean 4 projects.\n"
    "\n"
    "lake is a build system, and build systems want to resolve dependencies:\n"
    "network, git, manifests, hashes. When you are air-gapped, on a locked-down\n"
    "Windows box, in a CI cache-restore job, or lake simply refuses to re-verify a\n"
    "\"plausible package\" whose URL moved, you still want one thing:\n"
    "\n"
    "    elaborate my source files and tell me: errors, warnings, sorry.\n"
    "\n"
    "leanoff does exactly that, with plain `lean` and a LEAN_PATH assembled from\n"
    "what is already on disk. No git, no network, no lake.\n"
    "\n"
    "Commands:\n"
    "    leanoff verify   elaborate modules against existing oleans, report\n"
    "    leanoff build    compile project modules to oleans in dependency order\n"
    "\n"
    "Zero dependencies. Python 3.9+ stdlib only.\n"
    "\n"
    "positional arguments:\n"
    "  {verify,build}\n"
    "    verify        elaborate modules against existing oleans\n"
    "    build         compile project modules to oleans in dependency order\n"
    "\n"
    "options:\n"
    "  -h, --help      show this help message and exit\n";

const char* const kVerifyUsage =
    "usage: leanoff verify [-h] [--root ROOT] [--lean LEAN] [--lean-path LEAN_PATH]\n"
    "                      [--filter FILTER] [--jobs JOBS] [--allow-sorry]\n"
    "                      [--format {text,json}]\n";

const char* const kVerifyHelp =
    "usage: leanoff verify [-h] [--root ROOT] [--lean LEAN] [--lean-path LEAN_PATH]\n"
    "                      [--filter FILTER] [--jobs JOBS] [--allow-sorry]\n"
    "                      [--format {text,json}]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --root ROOT           project root (default: cwd)\n"
    "  --lean LEAN           path to lean executable or its bin directory\n"
    "  --lean-path LEAN_PATH\n"
    "                        extra LEAN_PATH directory (repeatable, e.g. a local\n"
    "                        Mathlib checkout)\n"
    "  --filter FILTER       only modules whose name matches this regex\n"
    "  --jobs JOBS\n"
    "  --allow-sorry         do not fail on sorry\n"
    "  --format {text,json}\n";

const char* const kBuildUsage =
    "usage: leanoff build [-h] [--root ROOT] [--lean LEAN] [--lean-path LEAN_PATH]\n"
    "                     [--filter FILTER] [--jobs JOBS] [--allow-sorry]\n"
    "                     [--format {text,json}] [--out OUT]\n";

const char* const kBuildHelp =
    "usage: leanoff build [-h] [--root ROOT] [--lean LEAN] [--lean-path LEAN_PATH]\n"
    "                     [--filter FILTER] [--jobs JOBS] [--allow-sorry]\n"
    "                     [--format {text,json}] [--out OUT]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --root ROOT           project root (default: cwd)\n"
    "  --lean LEAN           path to lean executable or its bin directory\n"
    "  --lean-path LEAN_PATH\n"
    "                        extra LEAN_PATH directory (repeatable, e.g. a local\n"
    "                        Mathlib checkout)\n"
    "  --filter FILTER       only modules whose name matches this regex\n"
    "  --jobs JOBS\n"
    "  --allow-sorry         do not fail on sorry\n"
    "  --format {text,json}\n"
    "  --out OUT             olean output directory (default:\n"
    "                        <root>/.leanoff/olean)\n";

// ---------------------------------------------------------------------------
// argparse's _negative_number_matcher: ^-\d+$ or ^-\d*\.\d+$.

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool negativeNumber(const std::string& s) {
    if (s.size() < 2 || s[0] != '-') return false;
    std::string t = s.substr(1);
    size_t dot = t.find('.');
    if (dot == std::string::npos) return allDigits(t);
    return allDigits(t.substr(0, dot)) && allDigits(t.substr(dot + 1));
}

// argparse lets an option take the next token as its value unless the token
// looks like an option: longer than one dash, starting with '-', and not a
// negative number. A lone "-" is a value.
bool usableAsValue(const std::string& s) {
    return !(s.size() > 1 && s[0] == '-' && !negativeNumber(s));
}

// Starts with '-' and could be an option (not a lone dash, not a number):
// such tokens are unrecognized optionals in argparse unless they match.
bool looksLikeOption(const std::string& s) {
    return s.size() > 1 && s[0] == '-' && !negativeNumber(s);
}

int topErr(std::ostream& err, const std::string& msg) {
    err << kTopUsage << "leanoff: error: " << msg << "\n";
    return 2;
}

int subErr(std::ostream& err, bool build, const std::string& msg) {
    err << (build ? kBuildUsage : kVerifyUsage) << "leanoff "
        << (build ? "build" : "verify") << ": error: " << msg << "\n";
    return 2;
}

// min(8, os.cpu_count() or 4): the Python default for --jobs.
int defaultJobs() {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    return static_cast<int>(std::min<unsigned>(n, 8));
}

// ---------------------------------------------------------------------------
// Subcommand option table. Order = argparse registration order, which is the
// order ambiguity messages list the candidates in.

enum class Kind { Help, Str, Append, Int, Flag, Choice };

struct SubOpt {
    const char* name;
    Kind kind;
};

const SubOpt kVerifyOpts[] = {
    {"help", Kind::Help},         {"root", Kind::Str},
    {"lean", Kind::Str},          {"lean-path", Kind::Append},
    {"filter", Kind::Str},        {"jobs", Kind::Int},
    {"allow-sorry", Kind::Flag},  {"format", Kind::Choice},
};

const SubOpt kBuildOpts[] = {
    {"help", Kind::Help},        {"root", Kind::Str},
    {"lean", Kind::Str},         {"lean-path", Kind::Append},
    {"filter", Kind::Str},       {"jobs", Kind::Int},
    {"allow-sorry", Kind::Flag}, {"format", Kind::Choice},
    {"out", Kind::Str},
};

constexpr size_t kVerifyOptCount = sizeof(kVerifyOpts) / sizeof(kVerifyOpts[0]);
constexpr size_t kBuildOptCount = sizeof(kBuildOpts) / sizeof(kBuildOpts[0]);

// argparse _parse_optional for a "--name[=value]" token: exact registration
// wins, then the unique-prefix rule, then ambiguity. found==false means
// unrecognized (the token goes to extras).
const SubOpt* matchOption(const std::string& name, bool build, bool& ambiguous,
                          std::string& ambiguousList) {
    const SubOpt* reg = build ? kBuildOpts : kVerifyOpts;
    size_t n = build ? kBuildOptCount : kVerifyOptCount;
    for (size_t i = 0; i < n; i++) {
        if (name == std::string("--") + reg[i].name) return &reg[i];
    }
    const SubOpt* hit = nullptr;
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        std::string full = std::string("--") + reg[i].name;
        if (full.rfind(name, 0) == 0) {
            if (count > 0) ambiguousList += ", ";
            ambiguousList += full;
            hit = &reg[i];
            count++;
        }
    }
    ambiguous = count > 1;
    return count == 1 ? hit : nullptr;
}

// Parsed subcommand state: cliMain's Options plus the dispatch flag.
struct ParsedSub {
    bool isBuild = false;
    Options opts;
};

// Parses the subcommand's arguments. Returns: 0 help printed, 2 usage error
// (already reported), 1 parse finished (sub extras collected).
int parseSub(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
             ParsedSub& p, std::vector<std::string>& extras) {
    const bool build = p.isBuild;
    size_t i = 0;
    while (i < args.size()) {
        std::string a = args[i++];
        if (a == "-h" || a == "--help") {
            out << (build ? kBuildHelp : kVerifyHelp);
            return 0;
        }
        if (a == "--") {
            // subcommands have no positionals: "--" and everything after it
            // are extras (argparse keeps the literal "--" in the list).
            extras.push_back("--");
            while (i < args.size()) extras.push_back(args[i++]);
            break;
        }
        if (!looksLikeOption(a)) {
            extras.push_back(a);
            continue;
        }
        std::string name = a, val;
        bool hasVal = false;
        size_t eq = a.find('=');
        if (eq != std::string::npos) {
            name = a.substr(0, eq);
            val = a.substr(eq + 1);
            hasVal = true;
        }
        bool ambiguous = false;
        std::string ambiguousList;
        const SubOpt* hit = matchOption(name, build, ambiguous, ambiguousList);
        if (ambiguous) {
            // argparse reports the whole token, "=value" included
            return subErr(err, build, "ambiguous option: " + a + " could match " + ambiguousList);
        }
        if (hit == nullptr) {
            extras.push_back(a);
            continue;
        }
        const std::string opt = hit->name;
        if (hit->kind == Kind::Help) {
            if (hasVal) {
                return subErr(err, build,
                              "argument -h/--help: ignored explicit argument '" + val + "'");
            }
            out << (build ? kBuildHelp : kVerifyHelp);
            return 0;
        }
        if (hit->kind == Kind::Flag) {
            if (hasVal) {
                return subErr(err, build,
                              "argument --allow-sorry: ignored explicit argument '" + val + "'");
            }
            p.opts.allowSorry = true;
            continue;
        }
        if (!hasVal) {
            if (i >= args.size() || !usableAsValue(args[i])) {
                return subErr(err, build,
                              "argument --" + opt + ": expected one argument");
            }
            val = args[i++];
        }
        if (hit->kind == Kind::Choice) {
            if (val != "text" && val != "json") {
                return subErr(err, build, "argument --format: invalid choice: '" + val +
                                              "' (choose from 'text', 'json')");
            }
            p.opts.format = val;
            continue;
        }
        if (hit->kind == Kind::Int) {
            long long n = 0;
            if (!pyIntParse(val, n) || n < INT_MIN || n > INT_MAX) {
                return subErr(err, build, "argument --jobs: invalid int value: '" + val + "'");
            }
            p.opts.jobs = static_cast<int>(n);
            continue;
        }
        if (opt == "root") {
            p.opts.root = val;
        } else if (opt == "lean") {
            p.opts.lean = val;
        } else if (opt == "lean-path") {
            p.opts.leanPath.push_back(val);
        } else if (opt == "filter") {
            p.opts.filter = val;
        } else {  // out
            p.opts.out = val;
        }
    }
    return 1;
}

}  // namespace

int cliMain(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    // ---- top-level parse: only the cmd positional and -h/--help exist ----
    // Tokens from the subcommand on belong to the subparser (argparse hands
    // it the remaining strings), so the top level stops scanning there.
    // Positional candidates: anything not option-like, including negative
    // numbers, a lone "-", and the literal "--" (argparse keeps "--" as a
    // candidate: it fills the cmd slot and errors "invalid choice: '--'").
    std::vector<std::string> extras;
    std::string cmd;
    size_t cmdAt = std::string::npos;
    for (size_t k = 0; k < args.size(); k++) {
        const std::string& a = args[k];
        if (!looksLikeOption(a) || a == "--") {
            cmd = a;
            cmdAt = k;
            break;
        }
        if (a == "-h" || a == "--help") {
            out << kTopHelp;
            return 0;
        }
        std::string name = a, val;
        bool hasVal = false;
        size_t eq = a.find('=');
        if (eq != std::string::npos) {
            name = a.substr(0, eq);
            val = a.substr(eq + 1);
            hasVal = true;
        }
        if (std::string("--help").rfind(name, 0) == 0) {
            // unique prefix of the only long option (exact handled above)
            if (hasVal) {
                return topErr(err, "argument -h/--help: ignored explicit argument '" + val + "'");
            }
            out << kTopHelp;
            return 0;
        }
        extras.push_back(a);  // unrecognized top-level option
    }

    if (cmdAt == std::string::npos) {
        // missing required positional wins over unrecognized extras
        return topErr(err, "the following arguments are required: cmd");
    }
    if (cmd != "verify" && cmd != "build") {
        return topErr(err, "argument cmd: invalid choice: '" + cmd +
                               "' (choose from 'verify', 'build')");
    }

    // ---- subcommand parse ----
    ParsedSub p;
    p.isBuild = cmd == "build";
    p.opts.root = ".";
    p.opts.format = "text";
    p.opts.jobs = defaultJobs();
    std::vector<std::string> subExtras;
    int rc = parseSub(std::vector<std::string>(args.begin() + static_cast<long>(cmdAt) + 1,
                                                args.end()),
                      out, err, p, subExtras);
    if (rc != 1) return rc;  // help (0) or usage error (2)
    for (const auto& e : subExtras) extras.push_back(e);
    if (!extras.empty()) {
        std::string joined = extras[0];
        for (size_t k = 1; k < extras.size(); k++) joined += " " + extras[k];
        return topErr(err, "unrecognized arguments: " + joined);
    }

    // ---- dispatch ----
    Engine eng;
    eng.out = &out;
    try {
        return p.isBuild ? eng.build(p.opts) : eng.verify(p.opts);
    } catch (const std::exception& e) {
        err << e.what() << "\n";
        return 1;
    }
}

}  // namespace leanoff
