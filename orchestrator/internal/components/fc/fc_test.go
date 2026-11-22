package fc

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/health"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/wire"
)

func golden(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("..", "..", "..", "..", "shared", "proto", "testdata", name))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func facts(o health.Observation) map[string]string {
	m := map[string]string{}
	for _, f := range o.Facts {
		m[f.Key] = f.Value
	}
	return m
}

func TestDefinitionValid(t *testing.T) {
	if err := Definition().Validate(); err != nil {
		t.Fatal(err)
	}
}

func TestEnv(t *testing.T) {
	d := Definition()
	bridge := component.DefaultStackContext()
	host := bridge
	host.NetworkMode = component.NetworkHost
	for _, tc := range []struct {
		name string
		sc   component.StackContext
		want map[string]string
	}{
		{"bridge", bridge, map[string]string{"ASTROSIM_ENGINE_HOST": "engine", "ASTROSIM_ENGINE_PORT": "8765", "ASTROSIM_TELEMETRY_PORT": "8766"}},
		{"host", host, map[string]string{"ASTROSIM_ENGINE_HOST": "127.0.0.1", "ASTROSIM_ENGINE_PORT": "8765", "ASTROSIM_TELEMETRY_PORT": "8766"}},
	} {
		if got := d.Env(tc.sc); !reflect.DeepEqual(got, tc.want) {
			t.Errorf("%s env = %v, want %v", tc.name, got, tc.want)
		}
	}
}

func TestClassifyGolden(t *testing.T) {
	obs, ok := Classify(golden(t, "fc_telemetry.bin")) // status 3: connected + armed
	if !ok || !obs.Healthy {
		t.Fatalf("ok=%v healthy=%v", ok, obs.Healthy)
	}
	want := map[string]string{
		"link": "connected", "armed": "yes", "mode": "No-GPS hold",
		"gps": "3D fix, 12 satellites", "altitude": "10.2 m", "sim_time": "2:03.5",
	}
	if got := facts(obs); !reflect.DeepEqual(got, want) {
		t.Errorf("facts = %v, want %v", got, want)
	}
}

func TestClassifyVariants(t *testing.T) {
	base, err := wire.DecodeTelemetry(golden(t, "fc_telemetry.bin"))
	if err != nil {
		t.Fatal(err)
	}
	tests := []struct {
		name    string
		edit    func(*wire.Telemetry)
		healthy bool
		fact    [2]string // key, value
	}{
		{"disconnected", func(t *wire.Telemetry) { t.Status &^= wire.StatusConnected }, false, [2]string{"link", "not connected"}},
		{"disarmed", func(t *wire.Telemetry) { t.Status &^= wire.StatusArmed }, true, [2]string{"armed", "no"}},
		{"gps hold", func(t *wire.Telemetry) { t.Mode = wire.ModeGPSHold }, true, [2]string{"mode", "GPS hold"}},
		{"mode refused", func(t *wire.Telemetry) { t.Status |= wire.StatusModeRefused }, true, [2]string{"mode", "No-GPS hold, requested mode refused"}},
		{"no fix", func(t *wire.Telemetry) { t.GPSFix, t.GPSSats = 0, 0 }, true, [2]string{"gps", "no fix, 0 satellites"}},
		{"2d fix", func(t *wire.Telemetry) { t.GPSFix = 2 }, true, [2]string{"gps", "2D fix, 12 satellites"}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			tel := base
			tc.edit(&tel)
			obs, ok := Classify(tel.Encode())
			if !ok || obs.Healthy != tc.healthy {
				t.Fatalf("ok=%v healthy=%v, want healthy=%v", ok, obs.Healthy, tc.healthy)
			}
			if got := facts(obs)[tc.fact[0]]; got != tc.fact[1] {
				t.Errorf("%s = %q, want %q", tc.fact[0], got, tc.fact[1])
			}
		})
	}
}

func TestClassifyRejectsGarbage(t *testing.T) {
	for _, b := range [][]byte{nil, {1, 2, 3}, golden(t, "state.bin")} {
		if _, ok := Classify(b); ok {
			t.Errorf("accepted %d bytes", len(b))
		}
	}
}

func TestProbeAddr(t *testing.T) {
	p := Definition().Probe(component.DefaultStackContext()).(*health.UDPSubscribe)
	if p.Addr != "127.0.0.1:8766" {
		t.Errorf("addr = %s", p.Addr)
	}
	if got, err := wire.DecodeFCSubscribe(p.Subscribe(9)); err != nil || got.Seq != 9 {
		t.Errorf("subscribe = %+v, %v", got, err)
	}
}
