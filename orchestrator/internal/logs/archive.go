package logs

import (
	"archive/zip"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const (
	zipSuffix     = ".zip"
	partialSuffix = ".zip.partial"
)

// removeRetries and removeRetryWait bound how long Archive tries to delete
// the folder it just archived. Windows refuses to delete files that another
// process (an antivirus scanner, an indexer) briefly holds open.
const (
	removeRetries   = 3
	removeRetryWait = 200 * time.Millisecond
)

// Archive zips the session folder dir into dir+".zip" and removes the
// folder. The zip is written to dir+".zip.partial", synced and renamed, so a
// ".zip" that exists is always complete. Entries are named
// "<folder name>/<file>" and keep the files' modification times.
//
// If the zip was written but the folder could not be removed, Archive
// returns the zip path together with the error: the next Recover sees the
// complete zip and finishes the cleanup.
func Archive(dir string) (zipPath string, err error) {
	dir = filepath.Clean(dir)
	zipPath = dir + zipSuffix
	partial := dir + partialSuffix
	if err := writeZip(partial, dir); err != nil {
		os.Remove(partial)
		return "", err
	}
	if err := os.Rename(partial, zipPath); err != nil {
		os.Remove(partial)
		return "", err
	}
	syncDir(filepath.Dir(zipPath))
	if err := removeAllRetry(dir); err != nil {
		return zipPath, fmt.Errorf("logs: archived to %s but could not remove the folder: %w", zipPath, err)
	}
	return zipPath, nil
}

// writeZip writes the files under dir to a new zip at path and syncs it.
func writeZip(path, dir string) error {
	out, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o600)
	if err != nil {
		return err
	}
	zw := zip.NewWriter(out)
	err = addFiles(zw, dir, filepath.Base(dir))
	if cerr := zw.Close(); err == nil {
		err = cerr
	}
	if serr := out.Sync(); err == nil {
		err = serr
	}
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	return err
}

// addFiles copies every regular file under dir into zw as <prefix>/<rel>,
// in name order.
func addFiles(zw *zip.Writer, dir, prefix string) error {
	return filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.Type().IsRegular() {
			return nil
		}
		rel, err := filepath.Rel(dir, p)
		if err != nil {
			return err
		}
		fi, err := d.Info()
		if err != nil {
			return err
		}
		return addFile(zw, p, prefix+"/"+filepath.ToSlash(rel), fi, -1)
	})
}

// addFile copies the file at path into zw as name. With limit >= 0 only that
// many bytes are copied, which is how a live file is archived consistently.
func addFile(zw *zip.Writer, path, name string, fi fs.FileInfo, limit int64) error {
	h, err := zip.FileInfoHeader(fi)
	if err != nil {
		return err
	}
	h.Name, h.Method = name, zip.Deflate
	w, err := zw.CreateHeader(h)
	if err != nil {
		return err
	}
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	var src io.Reader = f
	if limit >= 0 {
		src = io.LimitReader(f, limit)
	}
	_, err = io.Copy(w, src)
	return err
}

func removeAllRetry(dir string) error {
	var err error
	for i := 0; i < removeRetries; i++ {
		if err = os.RemoveAll(dir); err == nil {
			return nil
		}
		time.Sleep(removeRetryWait)
	}
	return err
}

// syncDir flushes a directory so a rename survives power loss. It is best
// effort: some platforms cannot open or sync directories.
func syncDir(dir string) {
	if d, err := os.Open(dir); err == nil {
		d.Sync()
		d.Close()
	}
}

// Recover finishes the work of sessions that did not shut down cleanly
// (killed, crashed, power loss). In root, for every session folder other
// than skip (the live session's folder name):
//
//   - if a complete zip of it exists, the folder is removed;
//   - otherwise any partial zip is discarded and the folder is archived.
//
// Leftover ".zip.partial" files whose folder is gone are deleted. Files and
// folders that are not session names are never touched. It returns the zips
// it created. Errors are collected and do not stop the other sessions.
func Recover(root, skip string) (archived []string, err error) {
	entries, rerr := os.ReadDir(root)
	if rerr != nil {
		if errors.Is(rerr, fs.ErrNotExist) {
			return nil, nil
		}
		return nil, rerr
	}
	var errs []error
	folders := map[string]bool{}
	for _, e := range entries {
		if e.IsDir() && IsSessionName(e.Name()) {
			folders[e.Name()] = true
		}
	}
	for _, e := range entries {
		name := e.Name()
		switch {
		case e.IsDir() && folders[name] && name != skip:
			dir := filepath.Join(root, name)
			if zipComplete(dir+zipSuffix, dir) {
				if err := removeAllRetry(dir); err != nil {
					errs = append(errs, err)
				}
				os.Remove(dir + partialSuffix)
				continue
			}
			os.Remove(dir + partialSuffix)
			zp, err := Archive(dir)
			if zp != "" {
				archived = append(archived, zp)
			}
			if err != nil {
				errs = append(errs, err)
			}
		case !e.IsDir() && strings.HasSuffix(name, partialSuffix):
			base := strings.TrimSuffix(name, partialSuffix)
			if IsSessionName(base) && !folders[base] && base != skip {
				if err := os.Remove(filepath.Join(root, name)); err != nil {
					errs = append(errs, err)
				}
			}
		}
	}
	return archived, errors.Join(errs...)
}

// zipComplete reports whether the zip at zipPath is readable and holds every
// file of dir with the same size. A zip that was truncated by a crash has no
// central directory and fails to open.
func zipComplete(zipPath, dir string) bool {
	zr, err := zip.OpenReader(zipPath)
	if err != nil {
		return false
	}
	defer zr.Close()
	sizes := map[string]uint64{}
	for _, f := range zr.File {
		sizes[f.Name] = f.UncompressedSize64
	}
	prefix := filepath.Base(dir)
	complete := true
	err = filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.Type().IsRegular() {
			return nil
		}
		rel, err := filepath.Rel(dir, p)
		if err != nil {
			return err
		}
		fi, err := d.Info()
		if err != nil {
			return err
		}
		if sz, ok := sizes[prefix+"/"+filepath.ToSlash(rel)]; !ok || sz != uint64(fi.Size()) {
			complete = false
		}
		return nil
	})
	return err == nil && complete
}

// Retain deletes old session archives in root so that at most keep remain
// and their total size is at most maxBytes, oldest first (names sort
// chronologically). The newest archive is never deleted, even if it alone
// exceeds maxBytes, and keep is treated as at least 1. A maxBytes of zero or
// less means no size limit. Only "<session name>.zip" files are considered.
// It returns the paths it deleted.
func Retain(root string, keep int, maxBytes int64) (removed []string, err error) {
	entries, rerr := os.ReadDir(root)
	if rerr != nil {
		if errors.Is(rerr, fs.ErrNotExist) {
			return nil, nil
		}
		return nil, rerr
	}
	type archive struct {
		path string
		size int64
	}
	var all []archive
	var total int64
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() || !strings.HasSuffix(name, zipSuffix) || !IsSessionName(strings.TrimSuffix(name, zipSuffix)) {
			continue
		}
		fi, err := e.Info()
		if err != nil {
			continue
		}
		all = append(all, archive{filepath.Join(root, name), fi.Size()})
		total += fi.Size()
	}
	sort.Slice(all, func(i, j int) bool { return all[i].path < all[j].path })
	if keep < 1 {
		keep = 1
	}
	var errs []error
	for len(all) > 1 && (len(all) > keep || (maxBytes > 0 && total > maxBytes)) {
		oldest := all[0]
		all = all[1:]
		total -= oldest.size
		if err := os.Remove(oldest.path); err != nil && !errors.Is(err, fs.ErrNotExist) {
			errs = append(errs, err)
			continue
		}
		removed = append(removed, oldest.path)
	}
	return removed, errors.Join(errs...)
}
