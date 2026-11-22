package logs

import (
	"reflect"
	"strings"
	"testing"
	"time"
)

// lines feeds chunks to a lineWriter and returns the emitted lines.
func lines(t *testing.T, chunks ...string) []string {
	t.Helper()
	var got []string
	w := newLineWriter(func(s string) { got = append(got, s) })
	for _, c := range chunks {
		if n, err := w.Write([]byte(c)); n != len(c) || err != nil {
			t.Fatalf("Write(%q) = %d, %v", c, n, err)
		}
	}
	w.Close()
	return got
}

func TestLineWriter(t *testing.T) {
	long := strings.Repeat("a", maxLineBytes)
	tests := []struct {
		name   string
		chunks []string
		want   []string
	}{
		{"simple", []string{"one\ntwo\n"}, []string{"one", "two"}},
		{"split across writes", []string{"on", "e\nt", "wo\n"}, []string{"one", "two"}},
		{"newline alone", []string{"a", "\n", "b\n"}, []string{"a", "b"}},
		{"partial line flushed at close", []string{"one\ntail"}, []string{"one", "tail"}},
		{"no trailing partial after newline", []string{"one\n"}, []string{"one"}},
		{"empty lines kept", []string{"a\n\nb\n"}, []string{"a", "", "b"}},
		{"crlf", []string{"a\r\nb\r\n"}, []string{"a", "b"}},
		{"progress bar carriage returns", []string{"10%\r20%\r30%\n"}, []string{"10%20%30%"}},
		{"ansi colour", []string{"\x1b[31mred\x1b[0m plain \x1b[1;32mgreen\x1b[m\n"}, []string{"red plain green"}},
		{"ansi cursor and osc", []string{"\x1b[2K\x1b[1Gdone\x1b]0;title\x07!\n"}, []string{"done!"}},
		{"control chars", []string{"a\x00b\x07c\x7fd\te\n"}, []string{"abcd\te"}},
		{"invalid utf8", []string{"ok \xff\xfe bad\n"}, []string{"ok \uFFFD bad"}},
		{"valid utf8 kept", []string{"héllo \u2713\n"}, []string{"héllo \u2713"}},
		{"rune split across writes", []string{"\xe2\x9c", "\x93\n"}, []string{"\u2713"}},
		{"exactly at cap", []string{long + "\n"}, []string{long}},
		{"over cap truncated", []string{long + "xyz\nnext\n"}, []string{long + "…[truncated 3 bytes]", "next"}},
		{"over cap across writes", []string{long, "12345", "678\nnext\n"}, []string{long + "…[truncated 8 bytes]", "next"}},
		{"over cap partial at close", []string{long + "zz"}, []string{long + "…[truncated 2 bytes]"}},
		{"empty write", []string{"", "a\n", ""}, []string{"a"}},
		{"nothing", nil, nil},
		{"only close after partial cr", []string{"\r"}, []string{""}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if got := lines(t, tc.chunks...); !reflect.DeepEqual(got, tc.want) {
				t.Errorf("got %q\nwant %q", abbreviate(got), abbreviate(tc.want))
			}
		})
	}
}

func abbreviate(ss []string) []string {
	out := make([]string, len(ss))
	for i, s := range ss {
		if len(s) > 40 {
			s = s[:20] + "..." + s[len(s)-20:]
		}
		out[i] = s
	}
	return out
}

func TestLineWriterReusableAfterClose(t *testing.T) {
	var got []string
	w := newLineWriter(func(s string) { got = append(got, s) })
	w.Write([]byte("a"))
	w.Close()
	w.Close() // idempotent: nothing pending
	w.Write([]byte("b\n"))
	if !reflect.DeepEqual(got, []string{"a", "b"}) {
		t.Errorf("got %q", got)
	}
}

func TestSplitRuntimeTimestamp(t *testing.T) {
	est := time.FixedZone("", -5*3600)
	tests := []struct {
		name   string
		line   string
		wantTS time.Time
		want   string
		wantOK bool
	}{
		{"podman local offset", "2026-11-22T14:16:47.507071482-05:00 out 1",
			time.Date(2026, 11, 22, 14, 16, 47, 507071482, est), "out 1", true},
		{"docker utc", "2026-11-22T19:16:47.507071482Z hello world",
			time.Date(2026, 11, 22, 19, 16, 47, 507071482, time.UTC), "hello world", true},
		{"no fraction", "2026-11-22T19:16:47Z x", time.Date(2026, 11, 22, 19, 16, 47, 0, time.UTC), "x", true},
		{"timestamp only", "2026-11-22T19:16:47Z", time.Date(2026, 11, 22, 19, 16, 47, 0, time.UTC), "", true},
		{"empty content", "2026-11-22T19:16:47Z ", time.Date(2026, 11, 22, 19, 16, 47, 0, time.UTC), "", true},
		{"content keeps spaces", "2026-11-22T19:16:47Z  indented  ", time.Date(2026, 11, 22, 19, 16, 47, 0, time.UTC), " indented  ", true},
		{"no timestamp", "just a line", time.Time{}, "just a line", false},
		{"empty", "", time.Time{}, "", false},
		{"date but not rfc3339", "2026-11-22 19:16:47 x", time.Time{}, "2026-11-22 19:16:47 x", false},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			ts, rest, ok := splitRuntimeTimestamp(tc.line)
			if ok != tc.wantOK || rest != tc.want || !ts.Equal(tc.wantTS) {
				t.Errorf("got (%v, %q, %v), want (%v, %q, %v)", ts, rest, ok, tc.wantTS, tc.want, tc.wantOK)
			}
		})
	}
}
