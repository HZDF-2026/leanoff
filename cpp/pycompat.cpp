// pycompat.cpp — see pycompat.h. These routines keep the C++ port
// byte-compatible with the Python reference's reports.
#include "pycompat.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace leanoff {

std::u32string utf8ToU32(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t r = 0xFFFD;
        size_t len = 1;
        if (c < 0x80) {
            r = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < n &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) {
            r = (static_cast<uint32_t>(c & 0x1F) << 6) |
                (static_cast<unsigned char>(s[i + 1]) & 0x3F);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80) {
            r = (static_cast<uint32_t>(c & 0x0F) << 12) |
                (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                (static_cast<unsigned char>(s[i + 2]) & 0x3F);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 3]) & 0xC0) == 0x80) {
            r = (static_cast<uint32_t>(c & 0x07) << 18) |
                (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                (static_cast<unsigned char>(s[i + 3]) & 0x3F);
            len = 4;
        }
        out.push_back(static_cast<char32_t>(r));
        i += len;
    }
    return out;
}

std::string u32SliceToUtf8(const std::u32string& s, size_t from, size_t to) {
    std::string out;
    out.reserve(to - from);
    for (size_t i = from; i < to; i++) {
        uint32_t r = static_cast<uint32_t>(s[i]);
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
    return out;
}

std::string u32ToUtf8(const std::u32string& s) {
    return u32SliceToUtf8(s, 0, s.size());
}

size_t utf8RuneCount(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) n++;
    }
    return n;
}

namespace {

bool isPyLineSep(char32_t r) {
    switch (r) {
        case U'\n': case U'\r': case U'\v': case U'\f':
        case U'\x1c': case U'\x1d': case U'\x1e':
        case U'\x85': case U'\u2028': case U'\u2029':
            return true;
        default:
            return false;
    }
}

// str.isspace() covers the regex \s set plus \x1c-\x1f.
bool isPySpace(char32_t r) {
    if (r < 0x80) {
        return (r == ' ' || (r >= '\t' && r <= '\r') ||
                r == '\x1c' || r == '\x1d' || r == '\x1e' || r == '\x1f');
    }
    if (r == 0x85 || r == 0xA0 || r == 0x1680) return true;
    if (r >= 0x2000 && r <= 0x200A) return true;
    if (r == 0x2028 || r == 0x2029 || r == 0x202F || r == 0x205F || r == 0x3000)
        return true;
    return false;
}

}  // namespace

std::vector<std::string> pySplitLines(const std::string& s) {
    std::u32string rs = utf8ToU32(s);
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < rs.size(); i++) {
        if (!isPyLineSep(rs[i])) continue;
        size_t breakEnd = i;
        if (rs[i] == U'\r' && i + 1 < rs.size() && rs[i + 1] == U'\n') {
            i++;  // CRLF is a single boundary
        }
        lines.push_back(u32SliceToUtf8(rs, start, breakEnd));
        start = i + 1;
    }
    if (start < rs.size()) lines.push_back(u32SliceToUtf8(rs, start, rs.size()));
    return lines;
}

std::string pyStrip(const std::string& s) {
    std::u32string rs = utf8ToU32(s);
    size_t b = 0, e = rs.size();
    while (b < e && isPySpace(rs[b])) b++;
    while (e > b && isPySpace(rs[e - 1])) e--;
    return u32SliceToUtf8(rs, b, e);
}

std::string pyStr(const std::string& s) {
    std::u32string rs = utf8ToU32(s);
    std::string out = "\"";
    char buf[32];
    for (char32_t ch : rs) {
        uint32_t r = static_cast<uint32_t>(ch);
        switch (r) {
            case '"': out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '\n': out += "\\n"; continue;
            case '\r': out += "\\r"; continue;
            case '\t': out += "\\t"; continue;
            case '\b': out += "\\b"; continue;
            case '\f': out += "\\f"; continue;
            default: break;
        }
        // CPython's ensure_ascii encoder escapes [^\x20-\x7e] as \uXXXX,
        // surrogate-pairing code points above the BMP.
        if (r < 0x20 || r > 0x7E) {
            if (r > 0xFFFF) {
                uint32_t v = r - 0x10000;
                uint32_t hi = 0xD800 + (v >> 10);
                uint32_t lo = 0xDC00 + (v & 0x3FF);
                std::snprintf(buf, sizeof buf, "\\u%04x\\u%04x", hi, lo);
            } else {
                std::snprintf(buf, sizeof buf, "\\u%04x", r);
            }
            out += buf;
        } else {
            out += static_cast<char>(r);
        }
    }
    out += "\"";
    return out;
}

std::string pyFloat(double f) {
    if (std::isnan(f)) return "NaN";
    if (std::isinf(f)) return f > 0 ? "Infinity" : "-Infinity";
    // Shortest round-trip via printf + strtod probe, then Python's repr layout
    // rules: decimal notation for exponents in [-4, 16).
    char buf[64];
    for (int prec = 1; prec <= 17; prec++) {
        std::snprintf(buf, sizeof buf, "%.*e", prec - 1, f);
        double w = std::strtod(buf, nullptr);
        if (std::memcmp(&w, &f, 8) == 0) break;
    }
    const char* s = buf;
    bool neg = (*s == '-');
    if (neg) s++;
    std::string digits;
    digits += *s++;
    if (*s == '.') {
        s++;
        while (*s != 'e' && *s != '\0') digits += *s++;
    }
    while (*s != 'e' && *s != '\0') s++;
    int exp10 = std::atoi(s + 1);
    if (exp10 >= 16 || exp10 < -4) {
        return std::string(buf);  // printf's scientific form matches Python's
    }
    std::string out;
    int n = static_cast<int>(digits.size());
    if (n - 1 <= exp10) {
        out = digits + std::string(static_cast<size_t>(exp10 - (n - 1)), '0');
        out += ".0";
    } else if (exp10 >= 0) {
        out = digits.substr(0, static_cast<size_t>(exp10) + 1) + "." +
              digits.substr(static_cast<size_t>(exp10) + 1);
    } else {
        out = "0." + std::string(static_cast<size_t>(-exp10) - 1, '0') + digits;
    }
    if (neg) out = "-" + out;
    return out;
}

double round1(double f) {
    // Python round(x, 1) == strtod(sprintf("%.1f", x)): both round the exact
    // binary value to the nearest tenth, ties-to-even — and a tie is impossible
    // because x.x5 is never a dyadic rational.
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f", f);
    return std::strtod(buf, nullptr);
}

std::string truncateRunes(const std::string& s, size_t n) {
    if (utf8RuneCount(s) <= n) return s;
    size_t count = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (count == n) return s.substr(0, i);
            count++;
        }
    }
    return s;
}

bool pyIntParse(const std::string& s, long long& out) {
    size_t b = 0, e = s.size();
    auto pySpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
               c == '\r';
    };
    while (b < e && pySpace(s[b])) b++;
    while (e > b && pySpace(s[e - 1])) e--;
    if (b == e) return false;
    bool neg = false;
    if (s[b] == '+' || s[b] == '-') {
        neg = s[b] == '-';
        b++;
    }
    if (b == e) return false;
    unsigned long long v = 0;
    bool lastUnderscore = true;  // a leading underscore is invalid
    for (size_t i = b; i < e; i++) {
        char c = s[i];
        if (c == '_') {
            if (lastUnderscore) return false;  // "__" or leading "_"
            lastUnderscore = true;
            continue;
        }
        if (c < '0' || c > '9') return false;
        lastUnderscore = false;
        if (v > (18446744073709551615ULL - static_cast<unsigned>(c - '0')) / 10)
            return false;  // overflow
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    if (lastUnderscore) return false;  // trailing "_"
    unsigned long long lim = neg ? 9223372036854775808ULL : 9223372036854775807ULL;
    if (v > lim) return false;
    out = neg ? static_cast<long long>(~v + 1) : static_cast<long long>(v);
    return true;
}

}  // namespace leanoff
