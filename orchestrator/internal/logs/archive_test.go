package logs

import (
	"archive/zip"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"
	"time"
)

const (
	nameA = "20261120T100000Z-aaaaaaaaaaaaaaaa"
	nameB = "20261121T100000Z-bbbbbbbbbbbbbbbb"
	nameC = "20261122T100000Z-cccccccccccccccc"
)

// mkSession creates a session folder with files (name -> content).
func mkSession(t *testing.T, root, name string, files map[string]string) string {
	t.Helper()
	dir := filepath.Join(root, name)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	for f, c := range files {
		if err := os.WriteFile(filepath.Join(dir, f), []byte(c), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

// zipContents reads a zip into name -> content.
func zipContents(t *testing.T, path string) map[string]string {
	t.Helper()
	zr, err := zip.OpenReader(path)
	if err != nil {
		t.Fatal(err)
	}
	defer zr.Close()
	out := map[string]string{}
	for _, f := range zr.File {
		rc, err := f.Open()
		if err != nil {
			t.Fatal(err)
		}
		b, _ := io.ReadAll(rc)
		rc.Close()
		out[f.Name] = string(b)
	}
	return out
}

func exists(p string) bool { _, err := os.Stat(p); return err == nil }

func listNames(t *testing.T, root string) []string {
	t.Helper()
	es, err := os.ReadDir(root)
	if err != nil {
		t.Fatal(err)
	}
	var out []string
	for _, e := range es {
		out = append(out, e.Name())
	}
	sort.Strings(out)
	return out
}

var sessionFiles = map[string]string{
	"combined.log":     "# h\nline one\nline two\n",
	"fc.log":           "# h\nfc line\n",
	"fc.part002.log":   "more\n",
	"orchestrator.log": strings.Repeat("x", 5000),
}

func TestArchiveRoundTrip(t *testing.T) {
	root := t.TempDir()
	dir := mkSession(t, root, nameA, sessionFiles)
	mtime := time.Date(2026, 11, 20, 10, 0, 0, 0, time.UTC)
	os.Chtimes(filepath.Join(dir, "fc.log"), mtime, mtime)

	zp, err := Archive(dir)
	if err != nil || zp != dir+".zip" {
		t.Fatalf("Archive = %q, %v", zp, err)
	}
	if exists(dir) || exists(dir+".zip.partial") {
		t.Errorf("leftovers: %v", listNames(t, root))
	}
	got := zipContents(t, zp)
	want := map[string]string{}
	for f, c := range sessionFiles {
		want[nameA+"/"+f] = c
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("zip contents differ: %v", got)
	}
	zr, _ := zip.OpenReader(zp)
	defer zr.Close()
	for _, f := range zr.File {
		if f.Name == nameA+"/fc.log" && !f.Modified.Equal(mtime) {
			t.Errorf("mtime = %v, want %v", f.Modified, mtime)
		}
		if f.Method != zip.Deflate {
			t.Errorf("%s not deflated", f.Name)
		}
	}
	if fi, _ := os.Stat(zp); fi.Mode().Perm() != 0o600 && os.PathSeparator == '/' {
		t.Errorf("zip mode = %v", fi.Mode().Perm())
	}
}

func TestArchiveEmptyAndMissing(t *testing.T) {
	root := t.TempDir()
	dir := mkSession(t, root, nameA, nil)
	zp, err := Archive(dir)
	if err != nil || len(zipContents(t, zp)) != 0 {
		t.Fatalf("empty: %v", err)
	}
	if _, err := Archive(filepath.Join(root, nameB)); err == nil {
		t.Error("archiving a missing folder should fail")
	}
	if exists(filepath.Join(root, nameB+".zip")) || exists(filepath.Join(root, nameB+".zip.partial")) {
		t.Error("failed archive left files behind")
	}
}

func TestRecoverCases(t *testing.T) {
	root := t.TempDir()

	// Folder only: gets archived.
	mkSession(t, root, nameA, sessionFiles)

	// Partial zip next to a folder: the partial is discarded, folder archived.
	dirB := mkSession(t, root, nameB, sessionFiles)
	os.WriteFile(dirB+".zip.partial", []byte("garbage"), 0o600)

	// Valid zip plus folder (crash between rename and delete): folder removed,
	// zip untouched.
	dirC := mkSession(t, root, nameC, sessionFiles)
	zipC := filepath.Join(t.TempDir(), "copy")
	mkSession(t, zipC, nameC, sessionFiles)
	if _, err := Archive(filepath.Join(zipC, nameC)); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(filepath.Join(zipC, nameC+".zip"))
	os.WriteFile(dirC+".zip", b, 0o600)

	// Truncated zip plus folder: re-archived.
	nameD := "20261123T100000Z-dddddddddddddddd"
	dirD := mkSession(t, root, nameD, sessionFiles)
	os.WriteFile(dirD+".zip", b[:len(b)/2], 0o600)

	// Zip that lacks a file the folder has: re-archived.
	nameE := "20261124T100000Z-eeeeeeeeeeeeeeee"
	dirE := mkSession(t, root, nameE, sessionFiles)
	os.WriteFile(dirE+".zip", zipBytes(t, map[string]string{nameE + "/combined.log": sessionFiles["combined.log"]}), 0o600)

	// Zip with a wrong size: re-archived.
	nameF := "20261125T100000Z-ffffffffffffffff"
	dirF := mkSession(t, root, nameF, sessionFiles)
	wrong := map[string]string{}
	for f, c := range sessionFiles {
		wrong[nameF+"/"+f] = c
	}
	wrong[nameF+"/fc.log"] = "short"
	os.WriteFile(dirF+".zip", zipBytes(t, wrong), 0o600)

	// Orphan partial: deleted.
	orphan := "20261126T100000Z-0000000000000000.zip.partial"
	os.WriteFile(filepath.Join(root, orphan), []byte("x"), 0o600)

	// The live session is skipped entirely, including its folder.
	skip := "20261127T100000Z-1111111111111111"
	mkSession(t, root, skip, sessionFiles)

	// Things that are not ours stay.
	os.WriteFile(filepath.Join(root, "notes.txt"), []byte("mine"), 0o600)
	os.WriteFile(filepath.Join(root, "other.zip.partial"), []byte("mine"), 0o600)
	mkSession(t, root, "my-folder", map[string]string{"a": "b"})
	mkSession(t, root, "20261128T100000Z-short", map[string]string{"a": "b"})

	archived, err := Recover(root, skip)
	if err != nil {
		t.Fatal(err)
	}
	var base []string
	for _, a := range archived {
		base = append(base, filepath.Base(a))
	}
	sort.Strings(base)
	wantArchived := []string{nameA + ".zip", nameB + ".zip", nameD + ".zip", nameE + ".zip", nameF + ".zip"}
	if !reflect.DeepEqual(base, wantArchived) {
		t.Errorf("archived = %v\nwant       %v", base, wantArchived)
	}

	wantNames := []string{
		"20261128T100000Z-short", "my-folder", "notes.txt", "other.zip.partial", skip,
		nameA + ".zip", nameB + ".zip", nameC + ".zip", nameD + ".zip", nameE + ".zip", nameF + ".zip",
	}
	sort.Strings(wantNames)
	if got := listNames(t, root); !reflect.DeepEqual(got, wantNames) {
		t.Errorf("root = %v\nwant   %v", got, wantNames)
	}
	for _, n := range []string{nameA, nameB, nameC, nameD, nameE, nameF} {
		got := zipContents(t, filepath.Join(root, n+".zip"))
		if len(got) != len(sessionFiles) || got[n+"/fc.log"] != sessionFiles["fc.log"] {
			t.Errorf("%s.zip incomplete: %d entries, fc.log=%q", n, len(got), got[n+"/fc.log"])
		}
	}
}

func zipBytes(t *testing.T, files map[string]string) []byte {
	t.Helper()
	p := filepath.Join(t.TempDir(), "z.zip")
	f, _ := os.Create(p)
	zw := zip.NewWriter(f)
	for n, c := range files {
		w, _ := zw.Create(n)
		io.WriteString(w, c)
	}
	zw.Close()
	f.Close()
	b, _ := os.ReadFile(p)
	return b
}

func TestRecoverMissingRootAndIdempotent(t *testing.T) {
	if got, err := Recover(filepath.Join(t.TempDir(), "nope"), ""); err != nil || got != nil {
		t.Errorf("missing root: %v, %v", got, err)
	}
	root := t.TempDir()
	mkSession(t, root, nameA, sessionFiles)
	if got, err := Recover(root, ""); err != nil || len(got) != 1 {
		t.Fatalf("first: %v, %v", got, err)
	}
	if got, err := Recover(root, ""); err != nil || len(got) != 0 {
		t.Errorf("second run should do nothing: %v, %v", got, err)
	}
}

func TestRetain(t *testing.T) {
	names := []string{nameA, nameB, nameC, "20261123T100000Z-dddddddddddddddd", "20261124T100000Z-eeeeeeeeeeeeeeee"}
	mk := func(sizes ...int) string {
		root := t.TempDir()
		for i, n := range names {
			size := 100
			if i < len(sizes) {
				size = sizes[i]
			}
			os.WriteFile(filepath.Join(root, n+".zip"), make([]byte, size), 0o600)
		}
		// Unrelated files are never counted or deleted.
		os.WriteFile(filepath.Join(root, "notes.zip"), make([]byte, 10000), 0o600)
		os.WriteFile(filepath.Join(root, "20261101T100000Z-aaaaaaaaaaaaaaaa.txt"), make([]byte, 10000), 0o600)
		mkSession(t, root, "20261102T100000Z-aaaaaaaaaaaaaaaa", map[string]string{"x": "y"})
		return root
	}
	remaining := func(root string) []string {
		var out []string
		for _, n := range listNames(t, root) {
			if strings.HasSuffix(n, ".zip") && IsSessionName(strings.TrimSuffix(n, ".zip")) {
				out = append(out, strings.TrimSuffix(n, ".zip"))
			}
		}
		return out
	}

	tests := []struct {
		name     string
		sizes    []int
		keep     int
		max      int64
		wantKeep []string
	}{
		{"under both limits", nil, 10, 10000, names},
		{"by count", nil, 2, 10000, names[3:]},
		{"by size", nil, 10, 250, names[3:]},
		{"size boundary is inclusive", nil, 10, 300, names[2:]},
		{"count and size, count wins", nil, 1, 10000, names[4:]},
		{"newest never deleted even if too big", []int{1, 1, 1, 1, 9999}, 10, 50, names[4:]},
		{"no size limit", nil, 3, 0, names[2:]},
		{"keep below one acts as one", nil, 0, 0, names[4:]},
		{"big old archive goes first", []int{5000, 100, 100, 100, 100}, 10, 450, names[1:]},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			root := mk(tc.sizes...)
			removed, err := Retain(root, tc.keep, tc.max)
			if err != nil {
				t.Fatal(err)
			}
			if got := remaining(root); !reflect.DeepEqual(got, tc.wantKeep) {
				t.Errorf("remaining = %v, want %v", got, tc.wantKeep)
			}
			if len(removed) != len(names)-len(tc.wantKeep) {
				t.Errorf("removed = %v", removed)
			}
			for _, keepMe := range []string{"notes.zip", "20261101T100000Z-aaaaaaaaaaaaaaaa.txt", "20261102T100000Z-aaaaaaaaaaaaaaaa"} {
				if !exists(filepath.Join(root, keepMe)) {
					t.Errorf("%s was deleted", keepMe)
				}
			}
		})
	}
	if got, err := Retain(filepath.Join(t.TempDir(), "nope"), 1, 1); err != nil || got != nil {
		t.Errorf("missing root: %v, %v", got, err)
	}
}
