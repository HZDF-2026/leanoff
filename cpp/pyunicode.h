// pyunicode.h — generated rune tables for Python-compatible semantics.
#ifndef leanoff_PYUNICODE_H
#define leanoff_PYUNICODE_H

#include <cstddef>
#include <cstdint>

namespace leanoff {

struct RxSpan {
    uint32_t lo, hi;
};

extern const RxSpan pyClassS[];
extern const size_t pyClassSCount;
extern const RxSpan pyClassW[];
extern const size_t pyClassWCount;
extern const RxSpan pyClassD[];
extern const size_t pyClassDCount;

// Nd digit runs: each entry is the rune of digit 0 of a 10-wide run.
extern const uint32_t pyDigits[];
extern const size_t pyDigitsCount;

// str.isprintable(): L, M, N, P, S and ASCII space.
extern const RxSpan pyPrintable[];
extern const size_t pyPrintableCount;

bool runeInSpans(const RxSpan* spans, size_t n, uint32_t r);

}  // namespace leanoff

#endif  // leanoff_PYUNICODE_H
