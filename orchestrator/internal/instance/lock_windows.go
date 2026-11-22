//go:build windows

package instance

import (
	"errors"
	"os"
	"syscall"
)

// errSharingViolation is ERROR_SHARING_VIOLATION, which package syscall does
// not name.
const errSharingViolation = syscall.Errno(32)

// lockFile opens path with a share mode that lets others read but not
// write it, and not open it a second time for writing: the open itself is
// the lock, and a sharing violation means another instance holds it.
// Allowing readers lets ReadOwnerURL work while the lock is held; Publish
// writes through the handle returned here.
func lockFile(path string) (*os.File, error) {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return nil, err
	}
	h, err := syscall.CreateFile(p,
		syscall.GENERIC_READ|syscall.GENERIC_WRITE,
		syscall.FILE_SHARE_READ,
		nil,
		syscall.OPEN_ALWAYS,
		syscall.FILE_ATTRIBUTE_NORMAL,
		0)
	if err != nil {
		if errors.Is(err, errSharingViolation) {
			return nil, ErrHeld
		}
		return nil, err
	}
	return os.NewFile(uintptr(h), path), nil
}

// unlockFile does nothing: closing the handle releases the lock.
func unlockFile(*os.File) error { return nil }
