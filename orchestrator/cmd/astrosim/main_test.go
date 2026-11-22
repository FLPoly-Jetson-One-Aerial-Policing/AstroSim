package main

import (
	"errors"
	"net"
	"reflect"
	"strings"
	"testing"
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
