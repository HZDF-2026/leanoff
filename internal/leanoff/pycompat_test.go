package leanoff

import (
	"strings"
	"testing"
)

// Python-compatibility helpers must behave exactly like their CPython
// counterparts; the ports' outputs are only byte-identical because these
// match.

func TestPySplitLines(t *testing.T) {
	cases := []struct{ in, want string }{
		{"", ""},
		{"a", "a"},
		{"\n", "\x00"}, // [""]
		{"a\n", "a"},   // no trailing empty
		{"a\nb", "a|b"},
		{"a\r\nb", "a|b"}, // CRLF is one break
		{"a\rb", "a|b"},
		{"a\n\nb", "a|\x00|b"}, // ["a", "", "b"]
		{"\r\n", "\x00"},       // [""]
		{"a\vb", "a|b"},        // \v breaks, like Python
		{"a\fb", "a|b"},        // \f breaks, like Python
		{"a\x1cb", "a|b"},
		{"a\u0085b", "a|b"},
		{"a\u2028b", "a|b"},
		{"a\n\r\nb", "a|\x00|b"}, // ["a", "", "b"]
	}
	for _, c := range cases {
		got := pySplitLines(c.in)
		var parts []string
		for _, l := range got {
			if l == "" {
				parts = append(parts, "\x00")
			} else {
				parts = append(parts, l)
			}
		}
		joined := strings.Join(parts, "|")
		if c.want == "" && len(got) == 0 {
			continue
		}
		if joined != c.want {
			t.Errorf("pySplitLines(%q) = %q, want %q", c.in, joined, c.want)
		}
	}
}

func TestPyStr(t *testing.T) {
	cases := []struct{ in, want string }{
		{`plain`, `"plain"`},
		{`say "hi"`, `"say \"hi\""`},
		{`back\slash`, `"back\\slash"`},
		{"tab\there", `"tab\there"`},
		{"new\nline", `"new\nline"`},
		{"\x01", `"` + `\u0001` + `"`},
		{"\x7f", "\"\x7f\""}, // DEL is ASCII: json.dumps keeps it raw
		{"é", `"` + `\u00e9` + `"`},
		{"中文", `"` + `\u4e2d\u6587` + `"`},
		{"𝔘", `"` + `\ud835\udd18` + `"`}, // astral: surrogate pair
	}
	for _, c := range cases {
		if got := pyStr(c.in); got != c.want {
			t.Errorf("pyStr(%q) = %s, want %s", c.in, got, c.want)
		}
	}
}

func TestPyFloat(t *testing.T) {
	cases := []struct {
		in   float64
		want string
	}{
		{0, "0.0"},
		{1, "1.0"},
		{0.5, "0.5"},
		{1.25, "1.25"},
		{12.3, "12.3"},
		{100, "100.0"},
	}
	for _, c := range cases {
		if got := pyFloat(c.in); got != c.want {
			t.Errorf("pyFloat(%v) = %s, want %s", c.in, got, c.want)
		}
	}
}

func TestTruncateRunes(t *testing.T) {
	if got := truncateRunes("hello", 10); got != "hello" {
		t.Errorf("truncateRunes passthrough = %q", got)
	}
	if got := truncateRunes("hello", 3); got != "hel" {
		t.Errorf("truncateRunes ascii = %q", got)
	}
	if got := truncateRunes(strings.Repeat("é", 5), 3); got != "ééé" {
		t.Errorf("truncateRunes multibyte = %q", got)
	}
}
