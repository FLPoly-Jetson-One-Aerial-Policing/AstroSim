package logs

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
)

// fileInfo names one file of a session folder and its size at a point in
// time, with Rel relative to the folder.
type fileInfo struct {
	Rel  string
	Size int64
}

// keepTailParts is how many of its newest parts a size-capped file keeps in
// addition to its first part.
const keepTailParts = 2

// partFile is one logical log file with a live size cap. It writes to a
// first part, <base>.log, and when a part reaches partMax starts the next,
// <base>.part002.log and so on. Only the first part and the newest
// keepTailParts parts are kept, so a file never grows beyond
// (1+keepTailParts)*partMax plus one line, yet always shows how the run
// started and how it ended. When a middle part is deleted a marker line is
// appended to the first part. A partFile is not safe for concurrent use; the
// sink's writer goroutine owns it.
type partFile struct {
	dir     string
	base    string
	partMax int64
	// deferRotate, if it returns true, postpones rotation (and so deletion
	// of old parts) while a download is reading the files.
	deferRotate func() bool

	f    *os.File
	w    *bufio.Writer
	size int64 // bytes written to the current part
	n    int   // number of the current part, from 1
	kept []int // part numbers still on disk, oldest first, always starting with 1
}

// newPartFile creates the first part with header as its first line.
func newPartFile(dir, base, header string, partMax int64, deferRotate func() bool) (*partFile, error) {
	p := &partFile{dir: dir, base: base, partMax: partMax, deferRotate: deferRotate}
	if err := p.open(1); err != nil {
		return nil, err
	}
	if err := p.writeLine(header); err != nil {
		p.close()
		return nil, err
	}
	return p, nil
}

func (p *partFile) partName(n int) string {
	if n == 1 {
		return p.base + ".log"
	}
	return fmt.Sprintf("%s.part%03d.log", p.base, n)
}

func (p *partFile) open(n int) error {
	f, err := os.OpenFile(filepath.Join(p.dir, p.partName(n)), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
	if err != nil {
		return err
	}
	p.f, p.w, p.n, p.size = f, bufio.NewWriterSize(f, 32<<10), n, 0
	p.kept = append(p.kept, n)
	return nil
}

// writeLine appends line and a newline, rotating first if the current part
// is full.
func (p *partFile) writeLine(line string) error {
	if p.size >= p.partMax && (p.deferRotate == nil || !p.deferRotate()) {
		if err := p.rotate(); err != nil {
			return err
		}
	}
	n, err := p.w.WriteString(line)
	p.size += int64(n)
	if err != nil {
		return err
	}
	if err := p.w.WriteByte('\n'); err != nil {
		return err
	}
	p.size++
	return nil
}

// rotate closes the current part and starts the next, deleting the oldest
// middle part if more than keepTailParts newer ones would remain.
func (p *partFile) rotate() error {
	if err := p.flush(); err != nil {
		return err
	}
	if err := p.f.Close(); err != nil {
		return err
	}
	next := p.n + 1
	if err := p.open(next); err != nil {
		return err
	}
	if err := p.writeLine(fmt.Sprintf("# %s continued in part %d", p.base, next)); err != nil {
		return err
	}
	// kept holds part 1 followed by the tail; trim the tail to the newest
	// keepTailParts, counting the part just opened.
	for len(p.kept) > 1+keepTailParts {
		old := p.kept[1]
		p.kept = append(p.kept[:1], p.kept[2:]...)
		if err := os.Remove(filepath.Join(p.dir, p.partName(old))); err != nil && !os.IsNotExist(err) {
			return err
		}
		p.noteRemoved(old)
	}
	return nil
}

// noteRemoved appends a marker to the first part saying that part n is gone.
// It is best effort: the marker is a courtesy and must not fail logging.
func (p *partFile) noteRemoved(n int) {
	f, err := os.OpenFile(filepath.Join(p.dir, p.partName(1)), os.O_WRONLY|os.O_APPEND, 0o600)
	if err != nil {
		return
	}
	defer f.Close()
	fmt.Fprintf(f, "==== part %d removed (log size limit) ====\n", n)
}

func (p *partFile) flush() error {
	if p.w == nil {
		return nil
	}
	return p.w.Flush()
}

// infos flushes and returns every part on disk, oldest first, with sizes as
// they are now.
func (p *partFile) infos() ([]fileInfo, error) {
	if err := p.flush(); err != nil {
		return nil, err
	}
	out := make([]fileInfo, 0, len(p.kept))
	for _, n := range p.kept {
		fi, err := os.Stat(filepath.Join(p.dir, p.partName(n)))
		if err != nil {
			return nil, err
		}
		out = append(out, fileInfo{Rel: p.partName(n), Size: fi.Size()})
	}
	return out, nil
}

func (p *partFile) close() error {
	if p.f == nil {
		return nil
	}
	err := p.flush()
	if cerr := p.f.Close(); err == nil {
		err = cerr
	}
	p.f, p.w = nil, nil
	return err
}
