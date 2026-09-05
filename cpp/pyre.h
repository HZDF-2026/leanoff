// pyre.h — a small backtracking regex engine reproducing CPython `re` semantics.
#ifndef leanoff_PYRE_H
#define leanoff_PYRE_H

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <stdexcept>
#include <utility>

namespace leanoff {

struct rxMatch {
    std::u32string s;
    // 0 = whole match, 1.. = groups; nullopt = did not participate.
    std::vector<std::optional<std::pair<int, int>>> spans;

    int start() const { return spans[0]->first; }
    int end() const { return spans[0]->second; }
    // Group text (UTF-8); "" for a group that did not participate.
    std::string group(int i) const;
};

using rxSubFn = std::function<std::string(const rxMatch&)>;

class Regexp {
public:
    // Throws std::runtime_error on a bad pattern.
    static std::unique_ptr<Regexp> compile(const std::string& pat);

    // re.match: anchored at the start.
    std::optional<rxMatch> matchString(const std::string& s) const;
    // re.search: first (leftmost) match.
    std::optional<rxMatch> find(const std::string& s) const;
    // re.match on a single line (same as matchString; mirrors matchLine).
    std::optional<rxMatch> matchLine(const std::string& line) const;
    // re.sub with a function replacement.
    std::string subFunc(const std::string& s, const rxSubFn& repl) const;
    // re.sub with a `\1`-style template replacement.
    std::string subTemplate(const std::string& s, const std::string& tpl) const;

    ~Regexp();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Regexp();
};

}  // namespace leanoff

#endif  // leanoff_PYRE_H
