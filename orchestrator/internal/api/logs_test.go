package api

import (
	"archive/zip"
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/logs"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/status"
)

// stubArchive is a LogArchive whose behaviour a test sets.
type stubArchive struct {
	name string
	zip  func(ctx context.Context, w io.Writer) error
}

func (s stubArchive) Name() string { return s.name }
func (s stubArchive) WriteZip(ctx context.Context, w io.Writer) error {
	return s.zip(ctx, w)
}

func logServer(t *testing.T, la LogArchive) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(NewHandler(Options{
		Session: &fakeSession{}, Broadcaster: status.NewBroadcaster(),
		AllowedHosts: []string{host}, Logs: la,
	}))
	t.Cleanup(srv.Close)
	return srv
}

func TestLogDownloadStatuses(t *testing.T) {
	ok := func(_ context.Context, w io.Writer) error { _, err := io.WriteString(w, "PKdata"); return err }
	tests := []struct {
		name string
		la   LogArchive
		host string
		hdr  map[string]string
		want int
		body string
	}{
		{"no archive", nil, host, nil, 503, "logs_unavailable"},
		{"unavailable name", stubArchive{zip: ok}, host, nil, 503, "logs_unavailable"},
		{"nil recorder", (*logs.Recorder)(nil), host, nil, 503, "logs_unavailable"},
		{"busy", stubArchive{name: "s", zip: func(context.Context, io.Writer) error { return logs.ErrBusy }}, host, nil, 429, "busy"},
		{"failure before output", stubArchive{name: "s", zip: func(context.Context, io.Writer) error { return errors.New("disk") }}, host, nil, 500, "logs_failed"},
		{"ok", stubArchive{name: "s", zip: ok}, host, nil, 200, "PKdata"},
		{"bad host", stubArchive{name: "s", zip: ok}, "evil.example", nil, 403, ""},
		{"bad origin", stubArchive{name: "s", zip: ok}, host, map[string]string{"Origin": "http://evil.example"}, 403, ""},
		{"good origin", stubArchive{name: "s", zip: ok}, host, map[string]string{"Origin": "http://" + host}, 200, "PKdata"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			res := do(t, logServer(t, tc.la), "GET", "/api/v1/logs/download", tc.host, tc.hdr)
			b, _ := io.ReadAll(res.Body)
			if res.StatusCode != tc.want || !strings.Contains(string(b), tc.body) {
				t.Fatalf("got %d %q, want %d containing %q", res.StatusCode, b, tc.want, tc.body)
			}
			if tc.want == 200 {
				if got := res.Header.Get("Content-Type"); got != "application/zip" {
					t.Errorf("Content-Type = %q", got)
				}
				if got := res.Header.Get("Content-Disposition"); got != `attachment; filename="astrosim-logs-s.zip"` {
					t.Errorf("Content-Disposition = %q", got)
				}
			}
		})
	}
}

func TestLogDownloadAbortsMidStream(t *testing.T) {
	la := stubArchive{name: "s", zip: func(_ context.Context, w io.Writer) error {
		_, _ = io.WriteString(w, "PK")
		return errors.New("boom")
	}}
	srv := logServer(t, la)
	req, _ := http.NewRequest("GET", srv.URL+"/api/v1/logs/download", nil)
	req.Host = host
	res, err := http.DefaultClient.Do(req)
	if err != nil {
		return // the abort may already surface here
	}
	defer res.Body.Close()
	if _, err := io.ReadAll(res.Body); err == nil {
		t.Fatal("a truncated download must not look complete")
	}
}

// A download from a real Recorder is a valid zip even while lines keep arriving.
func TestLogDownloadWhileWriting(t *testing.T) {
	rec, err := logs.Open(logs.Options{Root: t.TempDir(), SessionID: "0123456789abcdef", Version: "test"})
	if err != nil {
		t.Fatal(err)
	}
	stop := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; ; i++ {
			select {
			case <-stop:
				return
			default:
			}
			rec.Note("fc", fmt.Sprintf("line %d", i))
			if i%50 == 0 {
				time.Sleep(time.Millisecond)
			}
		}
	}()
	defer func() {
		close(stop)
		wg.Wait()
		rec.Close(context.Background())
	}()
	time.Sleep(20 * time.Millisecond)

	srv := logServer(t, rec)
	for i := 0; i < 3; i++ {
		res := do(t, srv, "GET", "/api/v1/logs/download", host, nil)
		b, err := io.ReadAll(res.Body)
		if err != nil || res.StatusCode != 200 {
			t.Fatalf("status %d, err %v", res.StatusCode, err)
		}
		zr, err := zip.NewReader(bytes.NewReader(b), int64(len(b)))
		if err != nil {
			t.Fatalf("not a valid zip: %v", err)
		}
		names := map[string]bool{}
		for _, f := range zr.File {
			names[f.Name] = true
			rc, err := f.Open()
			if err != nil {
				t.Fatal(err)
			}
			if _, err := io.Copy(io.Discard, rc); err != nil {
				t.Fatalf("%s: %v", f.Name, err)
			}
			rc.Close()
		}
		if !names[rec.Name()+"/combined.log"] {
			t.Errorf("entries %v lack combined.log", names)
		}
	}
}
