package logs

import (
	"bytes"
	"regexp"
	"strconv"
	"strings"
	"time"
	"unicode"
)

// maxLineBytes caps one log line. A runaway process printing without
// newlines must not exhaust memory or make one line unreadable.
const maxLineBytes = 16 << 10

// ansiRE matches ANSI escape sequences: CSI (colours, cursor movement) and
// OSC (window titles, hyperlinks) terminated by BEL or ST.
var ansiRE = regexp.MustCompile(`\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)`)

// lineWriter turns a raw byte stream into clean text lines. It is an
// io.Writer that never fails. Each completed line is cleaned and passed to
// emit:
//
//   - lines longer than 16 KiB are truncated with an "…[truncated N bytes]" notice
//   - carriage returns and other control characters (except tab) are removed
//   - ANSI colour and cursor sequences are removed
//   - invalid UTF-8 is replaced with U+FFFD
//
// Close flushes a final line that had no newline. A lineWriter is not safe
// for concurrent use.
type lineWriter struct {
	emit    func(string)
	buf     []byte
	skipped int  // bytes dropped from the current line by the cap
	pending bool // the current line has had any bytes (so an empty tail is not a line)
}

// newLineWriter returns a lineWriter calling emit for every line.
func newLineWriter(emit func(string)) *lineWriter { return &lineWriter{emit: emit} }

// Write implements io.Writer.
func (w *lineWriter) Write(p []byte) (int, error) {
	n := len(p)
	for len(p) > 0 {
		i := bytes.IndexByte(p, '\n')
		chunk := p
		if i >= 0 {
			chunk = p[:i]
		}
		if len(chunk) > 0 {
			w.pending = true
		}
		if room := maxLineBytes - len(w.buf); len(chunk) > room {
			w.skipped += len(chunk) - room
			chunk = chunk[:room]
		}
		w.buf = append(w.buf, chunk...)
		if i < 0 {
			break
		}
		w.finish()
		p = p[i+1:]
	}
	return n, nil
}

// Close emits a final partial line, if there is one.
func (w *lineWriter) Close() error {
	if w.pending {
		w.finish()
	}
	return nil
}

func (w *lineWriter) finish() {
	line := cleanLine(string(w.buf))
	if w.skipped > 0 {
		line += "…[truncated " + strconv.Itoa(w.skipped) + " bytes]"
	}
	w.buf, w.skipped, w.pending = w.buf[:0], 0, false
	w.emit(line)
}

// cleanLine makes s safe and readable in a text log.
func cleanLine(s string) string {
	s = strings.ToValidUTF8(s, "�")
	if strings.IndexByte(s, 0x1b) >= 0 {
		s = ansiRE.ReplaceAllString(s, "")
	}
	return strings.Map(func(r rune) rune {
		if r == '\t' || !unicode.IsControl(r) {
			return r
		}
		return -1
	}, s)
}

// splitRuntimeTimestamp splits a `logs --timestamps` line into the runtime's
// timestamp and the container's own text. A line that does not start with an
// RFC 3339 timestamp is returned whole with ok false.
func splitRuntimeTimestamp(line string) (ts time.Time, rest string, ok bool) {
	head, tail, _ := strings.Cut(line, " ")
	t, err := time.Parse(time.RFC3339Nano, head)
	if err != nil {
		return time.Time{}, line, false
	}
	return t, tail, true
}
