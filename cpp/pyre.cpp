// pyre.cpp — backtracking regex engine, a port of internal/leanoff/pyre.go.
// Leftmost-first with alternatives tried in pattern order, exactly like
// CPython's backtracking matcher; character classes use the CPython 3.10
// rune tables (pyunicode.cpp).
#include "pyre.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "pycompat.h"
#include "pyunicode.h"

namespace leanoff {

// ---------------------------------------------------------------- AST nodes

struct rxNode {
    virtual ~rxNode() = default;
};

// Sequences are non-owning; the parser keeps every node alive in `owned`.
using Seq = std::vector<rxNode*>;

struct rxLit : rxNode {
    char32_t ch;
    explicit rxLit(char32_t c) : ch(c) {}
};

struct rxClass : rxNode {
    std::vector<RxSpan> spans;
    bool neg = false;

    bool matches(char32_t r) const {
        uint32_t u = static_cast<uint32_t>(r);
        size_t lo = 0, hi = spans.size();
        bool in = false;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            const RxSpan& s = spans[mid];
            if (u < s.lo) {
                hi = mid;
            } else if (u > s.hi) {
                lo = mid + 1;
            } else {
                in = true;
                break;
            }
        }
        return in != neg;
    }
};

struct rxDot : rxNode {};
struct rxCaret : rxNode {};
struct rxDollar : rxNode {};

struct rxAlt : rxNode {
    std::vector<Seq> alts;
};

struct rxGroup : rxNode {
    int idx = 0;
    std::vector<Seq> alts;
};

struct rxRep : rxNode {
    rxNode* node = nullptr;
    int min = 0;
    int max = -1;  // -1 = unbounded
    bool lazy = false;
};

// ---------------------------------------------------------------- match state

struct rxState {
    const std::u32string& s;
    std::vector<std::optional<std::pair<int, int>>>& caps;

    rxState(const std::u32string& subject,
            std::vector<std::optional<std::pair<int, int>>>& groups)
        : s(subject), caps(groups) {}

    using Cont = std::function<bool(int)>;

    bool matchSeq(const Seq& nodes, size_t from, int pos, const Cont& cont) const;
    bool matchRep(const rxRep& v, int pos, int count, const Cont& cont) const;
    bool matchAtom(const rxNode& n, int pos, const Cont& cont) const;
};

// ---------------------------------------------------------------- compile

struct rxParser {
    std::u32string src;
    size_t pos = 0;
    int ngroups = 0;
    std::vector<std::unique_ptr<rxNode>> owned;

    explicit rxParser(std::u32string p) : src(std::move(p)) {}

    template <typename T, typename... Args>
    T* make(Args&&... args) {
        owned.push_back(std::make_unique<T>(std::forward<Args>(args)...));
        return static_cast<T*>(owned.back().get());
    }

    bool eof() const { return pos >= src.size(); }
    char32_t peek() const { return eof() ? char32_t(-1) : src[pos]; }
    char32_t peekAt(size_t n) const {
        return pos + n >= src.size() ? char32_t(-1) : src[pos + n];
    }
    char32_t next() { return src[pos++]; }

    [[noreturn]] void fail(const std::string& msg) const {
        throw std::runtime_error("pyre: " + msg);
    }

    std::vector<Seq> parseAlternation() {
        std::vector<Seq> alts;
        while (true) {
            alts.push_back(parseSeq());
            if (peek() == '|') {
                next();
                continue;
            }
            return alts;
        }
    }

    Seq parseSeq() {
        Seq seq;
        while (!eof()) {
            char32_t c = peek();
            if (c == '|' || c == ')') return seq;
            rxNode* atom = parseAtom();
            atom = parseQuantifier(atom);
            seq.push_back(atom);
        }
        return seq;
    }

    rxNode* parseAtom() {
        char32_t c = next();
        switch (c) {
        case '.':
            return make<rxDot>();
        case '^':
            return make<rxCaret>();
        case '$':
            return make<rxDollar>();
        case '(': {
            ngroups++;
            int idx = ngroups;
            std::vector<Seq> alts = parseAlternation();
            if (next() != ')') fail("missing ) in pattern");
            auto* g = make<rxGroup>();
            g->idx = idx;
            g->alts = std::move(alts);
            return g;
        }
        case '[':
            return parseClass();
        case '\\':
            return parseEscapeAtom();
        case char32_t(-1):
            fail("unexpected end of pattern");
        default:
            if (c == '*' || c == '+' || c == '?' || c == '{') {
                fail("nothing to repeat at " + std::to_string(pos - 1));
            }
            return make<rxLit>(c);
        }
    }

    rxNode* classOf(const RxSpan* spans, size_t n, bool neg) {
        auto* cls = make<rxClass>();
        cls->spans.assign(spans, spans + n);
        cls->neg = neg;
        return cls;
    }

    rxNode* parseEscapeAtom() {
        char32_t c = next();
        switch (c) {
        case 's': return classOf(pyClassS, pyClassSCount, false);
        case 'S': return classOf(pyClassS, pyClassSCount, true);
        case 'w': return classOf(pyClassW, pyClassWCount, false);
        case 'W': return classOf(pyClassW, pyClassWCount, true);
        case 'd': return classOf(pyClassD, pyClassDCount, false);
        case 'D': return classOf(pyClassD, pyClassDCount, true);
        case 'u': return make<rxLit>(parseHexRune(4));
        case 'x': return make<rxLit>(parseHexRune(2));
        case 'n': return make<rxLit>('\n');
        case 't': return make<rxLit>('\t');
        case 'r': return make<rxLit>('\r');
        case 'f': return make<rxLit>('\f');
        case 'v': return make<rxLit>('\v');
        case char32_t(-1):
            fail("bad escape (end of pattern)");
        default:
            return make<rxLit>(c);
        }
    }

    char32_t parseHexRune(int n) {
        if (pos + static_cast<size_t>(n) > src.size()) {
            fail("incomplete escape \\u");
        }
        uint32_t v = 0;
        for (int i = 0; i < n; i++) {
            int d = hexVal(src[pos + i]);
            if (d < 0) fail("invalid hex digit in escape");
            v = v * 16 + static_cast<uint32_t>(d);
        }
        pos += static_cast<size_t>(n);
        return static_cast<char32_t>(v);
    }

    static int hexVal(char32_t r) {
        if (r >= '0' && r <= '9') return static_cast<int>(r - '0');
        if (r >= 'a' && r <= 'f') return static_cast<int>(r - 'a') + 10;
        if (r >= 'A' && r <= 'F') return static_cast<int>(r - 'A') + 10;
        return -1;
    }

    rxNode* parseQuantifier(rxNode* atom) {
        if (eof()) return atom;
        char32_t c = peek();
        switch (c) {
        case '*':
            next();
            return rep(atom, 0, -1);
        case '+':
            next();
            return rep(atom, 1, -1);
        case '?':
            next();
            return rep(atom, 0, 1);
        case '{':
            return parseBrace(atom);
        default:
            return atom;
        }
    }

    rxRep* rep(rxNode* node, int min, int max) {
        auto* r = make<rxRep>();
        r->node = node;
        r->min = min;
        r->max = max;
        r->lazy = takeLazy();
        return r;
    }

    bool takeLazy() {
        if (peek() == '?') {
            next();
            return true;
        }
        return false;
    }

    // {m}, {m,}, {m,n}; a malformed brace is a literal '{'.
    rxNode* parseBrace(rxNode* atom) {
        size_t save = pos;
        next();  // '{'
        int lo = 0;
        if (!parseDecimal(lo)) {
            pos = save;
            return make<rxLit>('{');
        }
        int hi = lo;
        if (peek() == ',') {
            next();
            if (peek() == '}') {
                hi = -1;
            } else {
                int v = 0;
                if (!parseDecimal(v)) {
                    pos = save;
                    return make<rxLit>('{');
                }
                hi = v;
            }
        }
        if (peek() != '}') {
            pos = save;
            return make<rxLit>('{');
        }
        next();
        return rep(atom, lo, hi);
    }

    bool parseDecimal(int& out) {
        size_t start = pos;
        long long v = 0;
        while (!eof() && peek() >= '0' && peek() <= '9') {
            v = v * 10 + (next() - '0');
        }
        out = static_cast<int>(v);
        return pos > start;
    }

    // '[' has already been consumed.
    rxNode* parseClass() {
        bool neg = false;
        if (peek() == '^') {
            next();
            neg = true;
        }
        std::vector<RxSpan> spans;
        bool first = true;
        while (true) {
            if (eof()) fail("unterminated character set");
            char32_t c = next();
            if (c == ']' && !first) break;
            first = false;
            char32_t lo = 0;
            const RxSpan* shorthand = nullptr;
            size_t shorthandCount = 0;
            parseClassItem(c, lo, shorthand, shorthandCount);
            if (shorthand != nullptr) {
                spans.insert(spans.end(), shorthand, shorthand + shorthandCount);
                continue;
            }
            // possible range: item '-' <hi>, only when '-' is not the last item
            if (peek() == '-' && peekAt(1) != ']' && peekAt(1) != char32_t(-1)) {
                next();  // '-'
                char32_t hc = next();
                char32_t hi = 0;
                const RxSpan* shorthand2 = nullptr;
                size_t shorthand2Count = 0;
                parseClassItem(hc, hi, shorthand2, shorthand2Count);
                if (shorthand2 != nullptr) fail("bad character range with class shorthand");
                if (hi < lo) fail("bad character range");
                spans.push_back(RxSpan{static_cast<uint32_t>(lo), static_cast<uint32_t>(hi)});
                continue;
            }
            spans.push_back(RxSpan{static_cast<uint32_t>(lo), static_cast<uint32_t>(lo)});
        }
        mergeSpans(spans);
        auto* cls = make<rxClass>();
        cls->spans = std::move(spans);
        cls->neg = neg;
        return cls;
    }

    // One class item: a literal rune, or a \s-style shorthand (shorthand != null).
    void parseClassItem(char32_t c, char32_t& lo, const RxSpan*& shorthand, size_t& shorthandCount) {
        if (c != '\\') {
            lo = c;
            shorthand = nullptr;
            return;
        }
        char32_t e = next();
        switch (e) {
        case 's':
            shorthand = pyClassS;
            shorthandCount = pyClassSCount;
            return;
        case 'w':
            shorthand = pyClassW;
            shorthandCount = pyClassWCount;
            return;
        case 'd':
            shorthand = pyClassD;
            shorthandCount = pyClassDCount;
            return;
        case 'u':
            lo = parseHexRune(4);
            shorthand = nullptr;
            return;
        case 'x':
            lo = parseHexRune(2);
            shorthand = nullptr;
            return;
        case 'n':
            lo = '\n';
            shorthand = nullptr;
            return;
        case 't':
            lo = '\t';
            shorthand = nullptr;
            return;
        case 'r':
            lo = '\r';
            shorthand = nullptr;
            return;
        case char32_t(-1):
            fail("bad escape (end of pattern)");
        default:
            lo = e;
            shorthand = nullptr;
            return;
        }
    }

    static void mergeSpans(std::vector<RxSpan>& spans) {
        if (spans.empty()) return;
        std::sort(spans.begin(), spans.end(), [](const RxSpan& a, const RxSpan& b) {
            if (a.lo != b.lo) return a.lo < b.lo;
            return a.hi < b.hi;
        });
        std::vector<RxSpan> out;
        out.push_back(spans[0]);
        for (size_t i = 1; i < spans.size(); i++) {
            RxSpan& last = out.back();
            const RxSpan& s = spans[i];
            if (s.lo <= last.hi + 1) {
                if (s.hi > last.hi) last.hi = s.hi;
            } else {
                out.push_back(s);
            }
        }
        spans = std::move(out);
    }
};

// ---------------------------------------------------------------- matching

bool rxState::matchSeq(const Seq& nodes, size_t from, int pos, const Cont& cont) const {
    if (from >= nodes.size()) return cont(pos);
    const rxNode& n = *nodes[from];
    const size_t rest = from + 1;
    if (dynamic_cast<const rxCaret*>(&n)) {
        if (pos == 0) return matchSeq(nodes, rest, pos, cont);
        return false;
    }
    if (dynamic_cast<const rxDollar*>(&n)) {
        if (pos == static_cast<int>(s.size()) ||
            (pos == static_cast<int>(s.size()) - 1 && s[pos] == '\n')) {
            return matchSeq(nodes, rest, pos, cont);
        }
        return false;
    }
    if (auto* v = dynamic_cast<const rxLit*>(&n)) {
        if (pos < static_cast<int>(s.size()) && s[pos] == v->ch) {
            return matchSeq(nodes, rest, pos + 1, cont);
        }
        return false;
    }
    if (dynamic_cast<const rxDot*>(&n)) {
        if (pos < static_cast<int>(s.size()) && s[pos] != '\n') {
            return matchSeq(nodes, rest, pos + 1, cont);
        }
        return false;
    }
    if (auto* v = dynamic_cast<const rxClass*>(&n)) {
        if (pos < static_cast<int>(s.size()) && v->matches(s[pos])) {
            return matchSeq(nodes, rest, pos + 1, cont);
        }
        return false;
    }
    if (auto* v = dynamic_cast<const rxAlt*>(&n)) {
        for (const Seq& alt : v->alts) {
            if (matchSeq(alt, 0, pos, [&](int end) {
                    return matchSeq(nodes, rest, end, cont);
                })) {
                return true;
            }
        }
        return false;
    }
    if (auto* v = dynamic_cast<const rxGroup*>(&n)) {
        for (const Seq& alt : v->alts) {
            auto saved = caps[v->idx];
            bool ok = matchSeq(alt, 0, pos, [&](int end) {
                caps[v->idx] = std::make_pair(pos, end);
                if (matchSeq(nodes, rest, end, cont)) return true;
                caps[v->idx] = saved;
                return false;
            });
            if (ok) return true;
            caps[v->idx] = saved;
        }
        return false;
    }
    if (auto* v = dynamic_cast<const rxRep*>(&n)) {
        return matchRep(*v, pos, 0, [&](int p) {
            return matchSeq(nodes, rest, p, cont);
        });
    }
    throw std::runtime_error("pyre: unknown node");
}

bool rxState::matchRep(const rxRep& v, int pos, int count, const Cont& cont) const {
    bool canMore = v.max < 0 || count < v.max;
    if (v.lazy) {
        if (count >= v.min && cont(pos)) return true;
        if (canMore) {
            return matchAtom(*v.node, pos, [&](int p) {
                if (p == pos) return false;  // guard against empty repetitions
                return matchRep(v, p, count + 1, cont);
            });
        }
        return false;
    }
    if (canMore) {
        if (matchAtom(*v.node, pos, [&](int p) {
                if (p == pos) return false;
                return matchRep(v, p, count + 1, cont);
            })) {
            return true;
        }
    }
    return count >= v.min && cont(pos);
}

bool rxState::matchAtom(const rxNode& n, int pos, const Cont& cont) const {
    if (auto* v = dynamic_cast<const rxGroup*>(&n)) {
        for (const Seq& alt : v->alts) {
            auto saved = caps[v->idx];
            bool ok = matchSeq(alt, 0, pos, [&](int end) {
                caps[v->idx] = std::make_pair(pos, end);
                if (cont(end)) return true;
                caps[v->idx] = saved;
                return false;
            });
            if (ok) return true;
            caps[v->idx] = saved;
        }
        return false;
    }
    if (auto* v = dynamic_cast<const rxAlt*>(&n)) {
        for (const Seq& alt : v->alts) {
            if (matchSeq(alt, 0, pos, cont)) return true;
        }
        return false;
    }
    Seq one{const_cast<rxNode*>(&n)};
    return matchSeq(one, 0, pos, cont);
}

// ---------------------------------------------------------------- impl

struct Regexp::Impl {
    std::vector<std::unique_ptr<rxNode>> owned;
    Seq prog;
    int ngroups = 0;
};

Regexp::Regexp() : impl_(new Impl) {}
Regexp::~Regexp() = default;

std::unique_ptr<Regexp> Regexp::compile(const std::string& pat) {
    rxParser p(utf8ToU32(pat));
    std::vector<Seq> alts = p.parseAlternation();
    if (p.pos != p.src.size()) {
        throw std::runtime_error("pyre: unexpected char in pattern");
    }
    auto re = std::unique_ptr<Regexp>(new Regexp());
    if (alts.size() > 1) {
        auto* alt = p.make<rxAlt>();
        alt->alts = std::move(alts);
        re->impl_->prog.push_back(alt);
    } else {
        re->impl_->prog = std::move(alts[0]);
    }
    re->impl_->ngroups = p.ngroups;
    re->impl_->owned = std::move(p.owned);
    return re;
}

// matchAt attempts a match anchored at pos.
static bool matchAt(const std::u32string& s, int pos, const Seq& prog, int ngroups,
                    rxMatch& out) {
    std::vector<std::optional<std::pair<int, int>>> caps(
        static_cast<size_t>(ngroups) + 1);
    rxState st(s, caps);
    int end = 0;
    bool ok = st.matchSeq(prog, 0, pos, [&](int p) {
        end = p;
        return true;
    });
    if (!ok) return false;
    caps[0] = std::make_pair(pos, end);
    out.s = s;
    out.spans = std::move(caps);
    return true;
}

std::string rxMatch::group(int i) const {
    if (i < 0 || static_cast<size_t>(i) >= spans.size() || !spans[i]) return "";
    return u32SliceToUtf8(s, static_cast<size_t>(spans[i]->first),
                          static_cast<size_t>(spans[i]->second));
}

std::optional<rxMatch> Regexp::matchString(const std::string& s) const {
    std::u32string rs = utf8ToU32(s);
    rxMatch m;
    if (matchAt(rs, 0, impl_->prog, impl_->ngroups, m)) return m;
    return std::nullopt;
}

std::optional<rxMatch> Regexp::matchLine(const std::string& line) const {
    return matchString(line);
}

std::optional<rxMatch> Regexp::find(const std::string& s) const {
    std::u32string rs = utf8ToU32(s);
    for (size_t pos = 0; pos <= rs.size(); pos++) {
        rxMatch m;
        if (matchAt(rs, static_cast<int>(pos), impl_->prog, impl_->ngroups, m)) {
            return m;
        }
    }
    return std::nullopt;
}

std::string Regexp::subFunc(const std::string& s, const rxSubFn& repl) const {
    std::u32string rs = utf8ToU32(s);
    std::string b;
    size_t pos = 0;
    while (pos <= rs.size()) {
        rxMatch m;
        if (!matchAt(rs, static_cast<int>(pos), impl_->prog, impl_->ngroups, m)) {
            if (pos < rs.size()) b += u32SliceToUtf8(rs, pos, pos + 1);
            pos++;
            continue;
        }
        b += u32SliceToUtf8(rs, pos, static_cast<size_t>(m.start()));
        b += repl(m);
        if (m.start() == m.end()) {
            if (static_cast<size_t>(m.end()) < rs.size()) {
                b += u32SliceToUtf8(rs, static_cast<size_t>(m.end()),
                                    static_cast<size_t>(m.end()) + 1);
            }
            pos = static_cast<size_t>(m.end()) + 1;
        } else {
            pos = static_cast<size_t>(m.end());
        }
    }
    return b;
}

static std::string expandTemplate(const std::string& tpl, const rxMatch& m) {
    std::u32string rs = utf8ToU32(tpl);
    std::string b;
    size_t i = 0;
    while (i < rs.size()) {
        char32_t c = rs[i];
        if (c != '\\') {
            b += u32SliceToUtf8(rs, i, i + 1);
            i++;
            continue;
        }
        i++;
        if (i >= rs.size()) throw std::runtime_error("pyre: bad escape (end of template)");
        char32_t e = rs[i];
        if (e == '\\') {
            b += "\\";
            i++;
            continue;
        }
        if (e == 'g') throw std::runtime_error("pyre: \\g not used by leanoff templates");
        if (e >= '0' && e <= '9') {
            size_t j = i;
            while (j < rs.size() && rs[j] >= '0' && rs[j] <= '9') j++;
            long long num = 0;
            for (size_t k = i; k < j; k++) {
                num = num * 10 + (rs[k] - '0');
            }
            if (num >= static_cast<long long>(m.spans.size())) {
                throw std::runtime_error("pyre: invalid group reference " +
                                         std::to_string(num));
            }
            b += m.group(static_cast<int>(num));
            i = j;
            continue;
        }
        throw std::runtime_error("pyre: bad escape in template");
    }
    return b;
}

std::string Regexp::subTemplate(const std::string& s, const std::string& tpl) const {
    return subFunc(s, [&](const rxMatch& m) { return expandTemplate(tpl, m); });
}

}  // namespace leanoff
