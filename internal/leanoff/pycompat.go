package leanoff

import (
	"fmt"
	"math"
	"strconv"
	"strings"
	"unicode/utf16"
	"unicode/utf8"
)

// This file mirrors the byte-level output conventions of the Python
// reference implementation so both ports produce identical JSON and text.

// pySplitLines splits like Python's str.splitlines, which breaks on \n,
// \r, \r\n, \v, \f, \x1c-\x1e, \x85, \u2028, and \u2029 — and yields no
// trailing empty line for a string ending in a break.
func pySplitLines(s string) []string {
	runes := []rune(s)
	var lines []string
	start, i := 0, 0
	for i < len(runes) {
		if isPyLineSep(runes[i]) {
			breakEnd := i
			if runes[i] == '\r' && i+1 < len(runes) && runes[i+1] == '\n' {
				i++ // CRLF is a single boundary
			}
			lines = append(lines, string(runes[start:breakEnd]))
			i++
			start = i
		} else {
			i++
		}
	}
	if start < len(runes) {
		lines = append(lines, string(runes[start:]))
	}
	return lines
}

func isPyLineSep(r rune) bool {
	switch r {
	case '\n', '\r', '\v', '\f', '\x1c', '\x1d', '\x1e', '\x85', '\u2028', '\u2029':
		return true
	}
	return false
}

// pyStr renders s as a Python json.dumps string (ensure_ascii=True):
// control characters and non-ASCII runes become lowercase \uXXXX escapes,
// with surrogate pairs for astral runes.
func pyStr(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for _, r := range s {
		switch r {
		case '"':
			b.WriteString(`\"`)
		case '\\':
			b.WriteString(`\\`)
		case '\n':
			b.WriteString(`\n`)
		case '\r':
			b.WriteString(`\r`)
		case '\t':
			b.WriteString(`\t`)
		case '\b':
			b.WriteString(`\b`)
		case '\f':
			b.WriteString(`\f`)
		default:
			if r < 0x20 || r > 0x7f {
				if r > 0xffff {
					r1, r2 := utf16.EncodeRune(r)
					fmt.Fprintf(&b, "\\u%04x\\u%04x", r1, r2)
				} else {
					fmt.Fprintf(&b, "\\u%04x", r)
				}
			} else {
				b.WriteRune(r)
			}
		}
	}
	b.WriteByte('"')
	return b.String()
}

// pyFloat renders f the way Python's json.dumps does: shortest round-trip
// form, with integral floats keeping a ".0" suffix.
func pyFloat(f float64) string {
	switch {
	case math.IsInf(f, 1):
		return "Infinity"
	case math.IsInf(f, -1):
		return "-Infinity"
	case math.IsNaN(f):
		return "NaN"
	}
	s := strconv.FormatFloat(f, 'g', -1, 64)
	if !strings.ContainsAny(s, ".eE") {
		s += ".0"
	}
	return s
}

// round1 rounds to one decimal place, like Python's round(x, 1).
func round1(f float64) float64 {
	return math.Round(f*10) / 10
}

// truncateRunes keeps at most n code points, like Python's s[:n].
func truncateRunes(s string, n int) string {
	if utf8.RuneCountInString(s) <= n {
		return s
	}
	count := 0
	for i := range s {
		if count == n {
			return s[:i]
		}
		count++
	}
	return s
}
