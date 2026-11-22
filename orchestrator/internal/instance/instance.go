// Package instance makes sure only one orchestrator runs per user.
//
// The first instance takes an exclusive lock on a file in the user's cache
// directory and writes its dashboard URL into it. A second instance fails
// to get the lock, reads the URL, and can send the user there instead of
// starting a competing stack. The operating system drops the lock when the
// owner exits for any reason, including a crash, so it never goes stale.
package instance

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// ErrHeld means another orchestrator instance holds the lock.
var ErrHeld = errors.New("instance: another AstroSim orchestrator is already running")

// DefaultPath returns the lock file location: astrosim/orchestrator.lock
// under os.UserCacheDir.
func DefaultPath() (string, error) {
	dir, err := os.UserCacheDir()
	if err != nil {
		return "", fmt.Errorf("instance: find the cache directory: %w", err)
	}
	return filepath.Join(dir, "astrosim", "orchestrator.lock"), nil
}

// Lock is a held single-instance lock.
type Lock struct {
	f    *os.File
	path string
}

// Acquire takes the lock at path, creating the file and its directory. It
// returns ErrHeld if another process holds it.
func Acquire(path string) (*Lock, error) {
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return nil, fmt.Errorf("instance: %w", err)
	}
	f, err := lockFile(path)
	if err != nil {
		return nil, err
	}
	return &Lock{f: f, path: path}, nil
}

// Publish records the dashboard URL in the lock file for ReadOwnerURL. It
// writes through the handle that holds the lock, which works even where the
// lock keeps other processes from opening the file for writing.
func (l *Lock) Publish(url string) error {
	if err := l.f.Truncate(0); err != nil {
		return fmt.Errorf("instance: publish URL: %w", err)
	}
	if _, err := l.f.WriteAt([]byte(url+"\n"), 0); err != nil {
		return fmt.Errorf("instance: publish URL: %w", err)
	}
	return l.f.Sync()
}

// Release drops the lock. It is safe to call more than once. The file stays,
// because deleting it would race with another instance acquiring it.
func (l *Lock) Release() error {
	if l == nil || l.f == nil {
		return nil
	}
	err := unlockFile(l.f)
	if cerr := l.f.Close(); err == nil {
		err = cerr
	}
	l.f = nil
	return err
}

// ReadOwnerURL returns the URL the current owner published at path. It is
// meant to be called after Acquire returned ErrHeld. It fails if the file
// cannot be read or holds no URL yet (the owner may still be starting up).
func ReadOwnerURL(path string) (string, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return "", fmt.Errorf("instance: read owner URL: %w", err)
	}
	url := strings.TrimSpace(string(b))
	if url == "" {
		return "", errors.New("instance: the running instance has not published its URL yet")
	}
	return url, nil
}
