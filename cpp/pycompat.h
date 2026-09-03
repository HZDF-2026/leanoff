// pycompat.h — the byte-level output conventions of the Python reference
// (leanoff.py): str.splitlines boundaries, json.dumps string/float escaping,
// round(x, 1), str[:n] rune truncation, and int() argument parsing.
#ifndef LEANOFF_PYCOMPAT_H
#define LEANOFF_PYCOMPAT_H

#include <string>
#include <vector>

namespace leanoff {

// UTF-8 <-> UTF-32; invalid bytes decode to U+FFFD (Python errors="replace").
std::u32string utf8ToU32(const std::string& s);
std::string u32ToUtf8(const std::u32string& s);
std::string u32SliceToUtf8(const std::u32string& s, size_t from, size_t to);
size_t utf8RuneCount(const std::string& s);

// Python str.splitlines: breaks on \n \r \r\n \v \f \x1c \x1d \x1e \x85
// \u2028 \u2029, no trailing empty line for a string ending in a break.
std::vector<std::string> pySplitLines(const std::string& s);

// Python str.strip(): removes leading/trailing characters where isspace().
std::string pyStrip(const std::string& s);

// json.dumps string (ensure_ascii=True): control and non-ASCII runes become
// lowercase \uXXXX escapes, surrogate pairs for astral runes.
std::string pyStr(const std::string& s);

// Python repr() of a float as json.dumps emits it: shortest round-trip form,
// integral floats keep ".0"; inf/nan become Infinity/-Infinity/NaN.
std::string pyFloat(double f);

// Python round(x, 1).
double round1(double f);

// Python s[:n] on a UTF-8 string (n counts code points).
std::string truncateRunes(const std::string& s, size_t n);

// Python int(): optional surrounding whitespace and sign, decimal digits with
// single underscores allowed strictly between digits.
bool pyIntParse(const std::string& s, long long& out);

}  // namespace leanoff

#endif  // LEANOFF_PYCOMPAT_H
