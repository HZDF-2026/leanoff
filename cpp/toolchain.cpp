// toolchain.cpp — see toolchain.h. The lake-manifest.json reading keeps the
// Python reference's tolerance: a malformed manifest (or a manifest whose
// entries are not objects) is simply ignored.
#include "toolchain.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace leanoff {
namespace {

namespace fs = std::filesystem;

const char kPathSep =
#ifdef _WIN32
    '\\';
#else
    '/';
#endif

std::string pathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + kPathSep + b;
}

std::string pathJoin4(const std::string& a, const std::string& b, const std::string& c,
                      const std::string& d) {
    return pathJoin(pathJoin(pathJoin(a, b), c), d);
}

// ---------------------------------------------------------------------------
// Minimal JSON reader (lake-manifest.json only): objects keep key order,
// numbers are kept as raw text, malformed input throws.
// ---------------------------------------------------------------------------

struct JVal {
    enum Kind { Nul, Bool, Num, Str, Arr, Obj } kind = Nul;
    bool b = false;
    std::string s;  // Str text or Num raw text
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* get(const std::string& key) const {
        if (kind != Obj) return nullptr;
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    bool isStr() const { return kind == Str; }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : t_(text) {}

    JVal parse() {
        JVal v = parseValue();
        ws();
        if (pos_ != t_.size()) throw std::runtime_error("json: trailing data");
        return v;
    }

private:
    const std::string& t_;
    size_t pos_ = 0;

    void ws() {
        while (pos_ < t_.size()) {
            char c = t_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos_++;
            else break;
        }
    }

    char peek() {
        if (pos_ >= t_.size()) throw std::runtime_error("json: unexpected end");
        return t_[pos_];
    }

    void expect(char c) {
        if (pos_ >= t_.size() || t_[pos_] != c) throw std::runtime_error("json: syntax");
        pos_++;
    }

    bool lit(const char* s) {
        size_t n = std::strlen(s);
        if (t_.compare(pos_, n, s) == 0) {
            pos_ += n;
            return true;
        }
        return false;
    }

    void appendUtf8(std::string& out, uint32_t r) {
        if (r < 0x80) {
            out.push_back(static_cast<char>(r));
        } else if (r < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (r >> 6)));
            out.push_back(static_cast<char>(0x80 | (r & 0x3F)));
        } else if (r < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (r >> 12)));
            out.push_back(static_cast<char>(0x80 | ((r >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (r & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (r >> 18)));
            out.push_back(static_cast<char>(0x80 | ((r >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((r >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (r & 0x3F)));
        }
    }

    uint32_t hex4() {
        if (pos_ + 4 > t_.size()) throw std::runtime_error("json: bad \\u");
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            char c = t_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else throw std::runtime_error("json: bad \\u");
        }
        return v;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= t_.size()) throw std::runtime_error("json: unterminated string");
            char c = t_[pos_++];
            if (c == '"') break;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= t_.size()) throw std::runtime_error("json: bad escape");
            char e = t_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t r = hex4();
                    if (r >= 0xD800 && r <= 0xDBFF && pos_ + 1 < t_.size() &&
                        t_[pos_] == '\\' && t_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        uint32_t lo = hex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            r = 0x10000 + ((r - 0xD800) << 10) + (lo - 0xDC00);
                        }
                    }
                    appendUtf8(out, r);
                    break;
                }
                default: throw std::runtime_error("json: bad escape");
            }
        }
        return out;
    }

    JVal parseValue() {
        ws();
        char c = peek();
        JVal v;
        if (c == '{') {
            pos_++;
            v.kind = JVal::Obj;
            ws();
            if (peek() == '}') {
                pos_++;
                return v;
            }
            while (true) {
                ws();
                std::string key = parseString();
                ws();
                expect(':');
                v.obj.emplace_back(std::move(key), parseValue());
                ws();
                char d = peek();
                pos_++;
                if (d == '}') break;
                if (d != ',') throw std::runtime_error("json: object syntax");
            }
            return v;
        }
        if (c == '[') {
            pos_++;
            v.kind = JVal::Arr;
            ws();
            if (peek() == ']') {
                pos_++;
                return v;
            }
            while (true) {
                v.arr.push_back(parseValue());
                ws();
                char d = peek();
                pos_++;
                if (d == ']') break;
                if (d != ',') throw std::runtime_error("json: array syntax");
            }
            return v;
        }
        if (c == '"') {
            v.kind = JVal::Str;
            v.s = parseString();
            return v;
        }
        if (lit("null")) return v;
        if (lit("true")) {
            v.kind = JVal::Bool;
            v.b = true;
            return v;
        }
        if (lit("false")) {
            v.kind = JVal::Bool;
            v.b = false;
            return v;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            v.kind = JVal::Num;
            size_t start = pos_;
            if (c == '-') pos_++;
            while (pos_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[pos_]))) pos_++;
            if (pos_ < t_.size() && t_[pos_] == '.') {
                pos_++;
                while (pos_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[pos_]))) pos_++;
            }
            if (pos_ < t_.size() && (t_[pos_] == 'e' || t_[pos_] == 'E')) {
                pos_++;
                if (pos_ < t_.size() && (t_[pos_] == '+' || t_[pos_] == '-')) pos_++;
                while (pos_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[pos_]))) pos_++;
            }
            v.s = t_.substr(start, pos_ - start);
            return v;
        }
        throw std::runtime_error("json: unexpected value");
    }
};

bool isAbsPath(const std::string& p) {
#ifdef _WIN32
    if (p.size() >= 2 && p[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(p[0])))
        return true;
    return p.size() >= 2 && p[0] == '\\' && p[1] == '\\';
#else
    return !p.empty() && p[0] == '/';
#endif
}

}  // namespace

std::string expandUser(const std::string& p) {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home) return p;
    if (p == "~") return home;
    if (p.rfind("~/", 0) == 0 || p.rfind("~\\", 0) == 0) {
        return pathJoin(home, p.substr(2));
    }
    return p;
}

std::string pyResolve(const std::string& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(fs::u8path(p), ec);
    if (ec) return p;
    fs::path res = fs::weakly_canonical(abs, ec);
    if (ec) return abs.u8string();
    return res.u8string();
}

bool isDir(const std::string& p) {
    std::error_code ec;
    return fs::is_directory(fs::u8path(p), ec) && !ec;
}

bool fileExists(const std::string& p) {
    std::error_code ec;
    return fs::exists(fs::u8path(p), ec) && !ec;
}

namespace {

// shutil.which / exec.LookPath for the lean binary.
std::string lookPath(const std::string& name) {
#ifdef _WIN32
    const char* pathExt = std::getenv("PATHEXT");
    std::vector<std::string> exts;
    if (pathExt && *pathExt) {
        std::string s(pathExt);
        size_t start = 0;
        while (true) {
            size_t semi = s.find(';', start);
            std::string e = semi == std::string::npos ? s.substr(start) : s.substr(start, semi - start);
            if (!e.empty()) exts.push_back(e);
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
    } else {
        exts = {".COM", ".EXE", ".BAT", ".CMD"};
    }
    // Windows searches the current directory first.
    std::vector<std::string> dirs;
    dirs.push_back(".");
    const char* pathEnv = std::getenv("PATH");
    if (pathEnv && *pathEnv) {
        std::string s(pathEnv);
        size_t start = 0;
        while (true) {
            size_t semi = s.find(';', start);
            std::string d = semi == std::string::npos ? s.substr(start) : s.substr(start, semi - start);
            dirs.push_back(d);
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
    }
    for (const auto& d : dirs) {
        for (const auto& e : exts) {
            std::string full = d.empty() ? name + e : d + "\\" + name + e;
            std::error_code ec;
            if (fs::is_regular_file(fs::u8path(full), ec) && !ec) return full;
        }
    }
    return "";
#else
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return "";
    std::string s(pathEnv);
    size_t start = 0;
    while (true) {
        size_t colon = s.find(':', start);
        std::string d = colon == std::string::npos ? s.substr(start) : s.substr(start, colon - start);
        std::string full = d.empty() ? name : d + "/" + name;
        if (::access(full.c_str(), X_OK) == 0) return full;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return "";
#endif
}

}  // namespace

Toolchain findToolchain(const std::string& spec) {
    std::string exe;
    if (spec.empty()) {
        std::string lp = lookPath("lean");
        if (lp.empty()) {
            throw std::runtime_error("leanoff: no `lean` on PATH and no --lean given");
        }
        exe = pyResolve(lp);
    } else {
        std::string p = expandUser(spec);
        if (isDir(p)) {
            exe = pathJoin(p,
#ifdef _WIN32
                           "lean.exe"
#else
                           "lean"
#endif
            );
        } else {
            exe = p;
        }
        if (!fileExists(exe)) {
            throw std::runtime_error("leanoff: lean executable not found: " + exe);
        }
        exe = pyResolve(exe);
    }
    // <toolchain>/lib/lean: two levels up from the bin directory.
    fs::path exeP = fs::u8path(exe);
    fs::path lib = exeP.parent_path().parent_path() / "lib" / "lean";
    return Toolchain{exe, lib.u8string()};
}

std::vector<std::string> leanPathComponents(const std::string& root,
                                            const std::string& manifestPath,
                                            const std::vector<std::string>& extra) {
    std::vector<std::string> comps;
    for (const auto& e : extra) {
        comps.push_back(pyResolve(expandUser(e)));
    }
    std::string own = pathJoin(pathJoin(root, ".leanoff"), "olean");
    if (isDir(own)) comps.push_back(pyResolve(own));
    std::string proj = pathJoin4(root, ".lake", "build", "lib");
    proj = pathJoin(proj, "lean");
    if (isDir(proj)) comps.push_back(pyResolve(proj));
    if (manifestPath.empty()) return comps;

    std::ifstream in(fs::u8path(manifestPath), std::ios::binary);
    if (!in) return comps;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    JVal m;
    try {
        m = JsonParser(data).parse();
    } catch (const std::exception&) {
        return comps;  // malformed manifest: ignored, like json.loads failure
    }
    std::string packagesDir = ".lake/packages";
    if (const JVal* pd = m.get("packagesDir")) {
        if (pd->isStr()) packagesDir = pd->s;
    }
    const JVal* pkgs = m.get("packages");
    if (!pkgs || pkgs->kind != JVal::Arr) return comps;
    for (const auto& entry : pkgs->arr) {
        if (entry.kind != JVal::Obj) continue;
        std::string type, dir, name;
        if (const JVal* v = entry.get("type")) { if (v->isStr()) type = v->s; }
        if (const JVal* v = entry.get("dir")) { if (v->isStr()) dir = v->s; }
        if (const JVal* v = entry.get("name")) {
            if (v->isStr()) name = v->s;
            else if (v->kind == JVal::Num) name = v->s;  // str() of a JSON number
        }
        std::string pkgRoot;
        if (type == "path" && !dir.empty()) {
            pkgRoot = expandUser(dir);
            if (!isAbsPath(pkgRoot)) pkgRoot = pyResolve(pathJoin(root, pkgRoot));
        } else {
            pkgRoot = pathJoin(pathJoin(root, packagesDir), name);
        }
        std::string odir = pathJoin4(pkgRoot, ".lake", "build", "lib");
        odir = pathJoin(odir, "lean");
        if (isDir(odir)) comps.push_back(pyResolve(odir));
    }
    return comps;
}

}  // namespace leanoff
