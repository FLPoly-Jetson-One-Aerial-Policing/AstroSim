package logging

import (
	"bytes"
	"context"
	"io"
	"log/slog"
	"strings"
	"testing"
	"time"
)

func TestParseLevel(t *testing.T) {
	for in, want := range map[string]slog.Level{
		"debug": slog.LevelDebug, "INFO": slog.LevelInfo, " Warn ": slog.LevelWarn,
		"warning": slog.LevelWarn, "error": slog.LevelError,
	} {
		got, err := ParseLevel(in)
		if err != nil || got != want {
			t.Errorf("ParseLevel(%q) = %v, %v", in, got, err)
		}
	}
	if _, err := ParseLevel("loud"); err == nil {
		t.Error("ParseLevel(loud) succeeded")
	}
}

func TestHandlerFormat(t *testing.T) {
	var buf bytes.Buffer
	h := NewHandler(&buf, slog.LevelInfo)
	log := slog.New(h)
	log.Debug("hidden")
	log.Info("hello", "n", 3, "who", "a b", "empty", "")
	log.With("run", "x").WithGroup("g").Warn("careful", "k", "v", slog.Group("in", "z", 1))
	log.Error("bad", "err", `say "hi"`)

	want := []string{
		"INFO hello n=3 who=\"a b\" empty=\"\"",
		"WARN careful run=x g.k=v g.in.z=1",
		"ERROR bad err=\"say \\\"hi\\\"\"",
	}
	lines := strings.Split(strings.TrimSuffix(buf.String(), "\n"), "\n")
	if len(lines) != len(want) {
		t.Fatalf("got %d lines:\n%s", len(lines), buf.String())
	}
	for i, l := range lines {
		// "YYYY-MM-DD HH:MM:SS " is 20 bytes.
		if len(l) < 20 {
			t.Fatalf("short line %q", l)
		}
		if _, err := time.ParseInLocation("2006-01-02 15:04:05", l[:19], time.Local); err != nil || l[19] != ' ' {
			t.Errorf("line %d timestamp: %q", i, l)
		}
		if l[20:] != want[i] {
			t.Errorf("line %d = %q, want %q", i, l[20:], want[i])
		}
	}
}

func TestHandlerLevel(t *testing.T) {
	var lv slog.LevelVar
	lv.Set(slog.LevelError)
	var buf bytes.Buffer
	log := New(&buf, &lv)
	log.Warn("no")
	if buf.Len() != 0 {
		t.Fatal("warn passed an error-level handler")
	}
	lv.Set(slog.LevelDebug)
	log.Debug("yes")
	if !strings.Contains(buf.String(), "DEBUG yes") {
		t.Fatalf("got %q", buf.String())
	}
}

// failWriter simulates a stderr whose terminal has gone away.
type failWriter struct{ n int }

func (w *failWriter) Write([]byte) (int, error) { w.n++; return 0, io.ErrClosedPipe }

func TestHandlerIgnoresWriteErrors(t *testing.T) {
	w := &failWriter{}
	log := New(w, slog.LevelInfo)
	log.Info("terminal is gone") // must neither panic nor block
	if w.n != 1 {
		t.Fatalf("writes = %d, want 1", w.n)
	}
	if err := NewHandler(w, slog.LevelInfo).Handle(context.Background(), slog.Record{}); err != nil {
		t.Errorf("Handle returned %v, want nil", err)
	}
}
