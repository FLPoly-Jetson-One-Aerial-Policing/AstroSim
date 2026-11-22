package main

import (
	"bytes"
	"context"
	"errors"
	"log/slog"
	"net"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/instance"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/logging"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime/fake"
)

func tcp(s string) net.Addr {
	a, err := net.ResolveTCPAddr("tcp", s)
	if err != nil {
		panic(err)
	}
	return a
}

func TestDashboardURL(t *testing.T) {
	for in, want := range map[string]string{
		"127.0.0.1:8770": "http://127.0.0.1:8770",
		"[::1]:9000":     "http://[::1]:9000",
		"0.0.0.0:8770":   "http://127.0.0.1:8770",
		"[::]:8770":      "http://127.0.0.1:8770",
		"10.1.2.3:80":    "http://10.1.2.3:80",
	} {
		if got := dashboardURL(tcp(in)); got != want {
			t.Errorf("dashboardURL(%s) = %s, want %s", in, got, want)
		}
	}
}

func TestAllowedHosts(t *testing.T) {
	host := func() (string, error) { return "Sim-Box", nil }
	ifaces := func() ([]net.Addr, error) {
		return []net.Addr{&net.IPNet{IP: net.ParseIP("192.168.1.5"), Mask: net.CIDRMask(24, 32)}, &net.IPAddr{}}, nil
	}
	tests := []struct {
		name string
		addr string
		want []string
	}{
		{"loopback", "127.0.0.1:8770", []string{"127.0.0.1:8770", "localhost:8770"}},
		{"specific address", "10.0.0.2:8770", []string{"10.0.0.2:8770", "localhost:8770", "127.0.0.1:8770"}},
		{"wildcard", "0.0.0.0:8770", []string{"0.0.0.0:8770", "localhost:8770", "127.0.0.1:8770", "sim-box:8770", "192.168.1.5:8770"}},
	}
	for _, tt := range tests {
		got := allowedHosts(tcp(tt.addr), host, ifaces)
		if !reflect.DeepEqual(got, tt.want) {
			t.Errorf("%s: got %q, want %q", tt.name, got, tt.want)
		}
	}
	// Lookup failures just mean fewer names.
	got := allowedHosts(tcp("0.0.0.0:1"), func() (string, error) { return "", errors.New("x") },
		func() ([]net.Addr, error) { return nil, errors.New("x") })
	if len(got) != 3 {
		t.Errorf("got %q", got)
	}
}

func TestRunExitCodes(t *testing.T) {
	var out, errb strings.Builder
	env := func(string) string { return "" }
	if code := run([]string{"--version"}, env, &out, &errb); code != 0 || out.String() != "astrosim dev\n" {
		t.Errorf("--version: %d %q", code, out.String())
	}
	if code := run([]string{"--help"}, env, &out, &errb); code != 0 {
		t.Errorf("--help: %d", code)
	}
	for _, args := range [][]string{{"--bogus"}, {"--restart-max=0"}, {"--listen=0.0.0.0:1"}, {"--network=overlay"}} {
		errb.Reset()
		if code := run(args, env, &out, &errb); code != 2 || !strings.Contains(errb.String(), "astrosim") {
			t.Errorf("%v: code %d, stderr %q", args, code, errb.String())
		}
	}
}

func TestCleanupCommand(t *testing.T) {
	ctx := context.Background()
	managed := map[string]string{"astrosim.managed": "true"}
	setup := func() *fake.Runtime {
		rt := fake.New()
		rt.SetImage("img", true)
		_, _ = rt.Run(ctx, runtime.RunSpec{Name: "astrosim-engine", Image: "img", Labels: managed})
		_, _ = rt.Run(ctx, runtime.RunSpec{Name: "by-hand", Image: "img"})
		_ = rt.EnsureNetwork(ctx, "astrosim", managed)
		return rt
	}
	tests := []struct {
		name     string
		prune    bool
		pingErr  error
		wantCode int
		wantOut  string
		wantNets int
	}{
		{"cleanup keeps the network", false, nil, exitOK, "removed container astrosim-engine\n", 1},
		{"prune removes the network", true, nil, exitOK, "removed container astrosim-engine\nremoved network astrosim\n", 0},
		{"no runtime", false, runtime.ErrNotRunning, exitFailure, "", 1},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			rt := setup()
			rt.PingErr = tt.pingErr
			var out, errb bytes.Buffer
			code := cleanup(rt, tt.prune, &out, logging.New(&errb, slog.LevelInfo))
			if code != tt.wantCode || out.String() != tt.wantOut {
				t.Errorf("code=%d out=%q, want %d %q (log %q)", code, out.String(), tt.wantCode, tt.wantOut, errb.String())
			}
			if n := len(rt.Networks()); n != tt.wantNets {
				t.Errorf("networks = %d, want %d", n, tt.wantNets)
			}
			if _, err := rt.Inspect(ctx, "by-hand"); err != nil {
				t.Error("unlabelled container was touched")
			}
		})
	}
	t.Run("nothing to clean up", func(t *testing.T) {
		var out bytes.Buffer
		if code := cleanup(fake.New(), true, &out, logging.New(&bytes.Buffer{}, slog.LevelInfo)); code != exitOK || out.String() != "nothing to clean up\n" {
			t.Errorf("code=%d out=%q", code, out.String())
		}
	})
}

func TestRefuseCleanup(t *testing.T) {
	path := filepath.Join(t.TempDir(), "lock")
	lock, err := instance.Acquire(path)
	if err != nil {
		t.Fatal(err)
	}
	defer lock.Release()
	if err := lock.Publish("http://127.0.0.1:8770"); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	code := refuseCleanup(path, &out, logging.New(&bytes.Buffer{}, slog.LevelInfo))
	if code != exitFailure || out.String() != "AstroSim is running at http://127.0.0.1:8770; stop it before cleaning up\n" {
		t.Errorf("code=%d out=%q", code, out.String())
	}
}
