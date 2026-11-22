// Package logging provides the orchestrator's log handler. Its output
// matches the containers' log format, "YYYY-MM-DD HH:MM:SS LEVEL message",
// so lines from every part of the stack read alike.
package logging

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"log/slog"
	"strconv"
	"strings"
	"sync"
	"time"
)

// ParseLevel converts "debug", "info", "warn" (or "warning") or "error",
// in any letter case, to a slog level.
func ParseLevel(s string) (slog.Level, error) {
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "debug":
		return slog.LevelDebug, nil
	case "info":
		return slog.LevelInfo, nil
	case "warn", "warning":
		return slog.LevelWarn, nil
	case "error":
		return slog.LevelError, nil
	}
	return 0, fmt.Errorf("unknown log level %q (want debug, info, warn or error)", s)
}

// Handler is an slog.Handler that writes one line per record:
//
//	2026-01-02 03:04:05 INFO message key=value key2="value with spaces"
//
// Times are local time. Groups are flattened into dotted keys.
type Handler struct {
	mu     *sync.Mutex
	w      io.Writer
	level  slog.Leveler
	attrs  []slog.Attr
	prefix string
}

// NewHandler returns a Handler writing to w, dropping records below level.
func NewHandler(w io.Writer, level slog.Leveler) *Handler {
	return &Handler{mu: &sync.Mutex{}, w: w, level: level}
}

// New returns a Logger using a Handler on w.
func New(w io.Writer, level slog.Leveler) *slog.Logger {
	return slog.New(NewHandler(w, level))
}

// Enabled implements slog.Handler.
func (h *Handler) Enabled(_ context.Context, l slog.Level) bool { return l >= h.level.Level() }

// Handle implements slog.Handler.
func (h *Handler) Handle(_ context.Context, r slog.Record) error {
	t := r.Time
	if t.IsZero() {
		t = time.Now()
	}
	var b bytes.Buffer
	b.WriteString(t.Local().Format("2006-01-02 15:04:05"))
	b.WriteByte(' ')
	b.WriteString(r.Level.String())
	b.WriteByte(' ')
	b.WriteString(r.Message)
	for _, a := range h.attrs {
		writeAttr(&b, "", a)
	}
	r.Attrs(func(a slog.Attr) bool {
		writeAttr(&b, h.prefix, a)
		return true
	})
	b.WriteByte('\n')
	h.mu.Lock()
	defer h.mu.Unlock()
	_, err := h.w.Write(b.Bytes())
	return err
}

// WithAttrs implements slog.Handler.
func (h *Handler) WithAttrs(attrs []slog.Attr) slog.Handler {
	c := *h
	c.attrs = append([]slog.Attr(nil), h.attrs...)
	for _, a := range attrs {
		a.Key = h.prefix + a.Key
		c.attrs = append(c.attrs, a)
	}
	return &c
}

// WithGroup implements slog.Handler.
func (h *Handler) WithGroup(name string) slog.Handler {
	if name == "" {
		return h
	}
	c := *h
	c.prefix = h.prefix + name + "."
	return &c
}

func writeAttr(b *bytes.Buffer, prefix string, a slog.Attr) {
	a.Value = a.Value.Resolve()
	if a.Equal(slog.Attr{}) {
		return
	}
	if a.Value.Kind() == slog.KindGroup {
		p := prefix
		if a.Key != "" {
			p += a.Key + "."
		}
		for _, g := range a.Value.Group() {
			writeAttr(b, p, g)
		}
		return
	}
	b.WriteByte(' ')
	b.WriteString(prefix + a.Key)
	b.WriteByte('=')
	s := a.Value.String()
	if s == "" || strings.ContainsAny(s, " \t\r\n\"=") {
		s = strconv.Quote(s)
	}
	b.WriteString(s)
}
