//go:build !unix && !windows

package instance

import (
	"errors"
	"os"
)

// lockFile is the fallback for platforms without file locking: it creates
// the file exclusively, so a second instance is refused. Unlike the real
// locks this does not survive a crash: a leftover file must be deleted by
// hand, and the error says where it is.
func lockFile(path string) (*os.File, error) {
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		if errors.Is(err, os.ErrExist) {
			return nil, ErrHeld
		}
		return nil, err
	}
	return f, nil
}

// unlockFile removes the lock file, which is the lock on this platform.
func unlockFile(f *os.File) error { return os.Remove(f.Name()) }
