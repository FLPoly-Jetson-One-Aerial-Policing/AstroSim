package engine

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"

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
	if got := d.Env(bridge); len(got) != 0 {
		t.Errorf("bridge env = %v, want empty", got)
	}
	host := bridge
	host.NetworkMode = component.NetworkHost
	want := map[string]string{"GZ_PARTITION": "astrosim", "GZ_IP": "127.0.0.1"}
	if got := d.Env(host); !reflect.DeepEqual(got, want) {
		t.Errorf("host env = %v, want %v", got, want)
	}
}

func TestClassifyGolden(t *testing.T) {
	now := time.Unix(0, 0)
	c := NewClassifier(func() time.Time { return now })
	obs, ok := c(golden(t, "state.bin")) // flags 3: ENGINE_OK + CONTROL_ACTIVE
	if !ok || !obs.Healthy {
		t.Fatalf("ok=%v healthy=%v", ok, obs.Healthy)
	}
	f := facts(obs)
	if f["sim_time"] != "2:03.5" || f["state_rate"] != "measuring" {
		t.Errorf("facts = %v", f)
	}
}

func TestClassifyEngineNotOK(t *testing.T) {
	st, err := wire.DecodeState(golden(t, "state.bin"))
	if err != nil {
		t.Fatal(err)
	}
	st.Flags &^= wire.FlagEngineOK
	obs, ok := NewClassifier(time.Now)(st.Encode())
	if !ok || obs.Healthy {
		t.Fatalf("ok=%v healthy=%v, want ok and unhealthy", ok, obs.Healthy)
	}
}

func TestClassifyRejectsGarbage(t *testing.T) {
	c := NewClassifier(time.Now)
	for _, b := range [][]byte{nil, {1, 2, 3}, golden(t, "control.bin"), golden(t, "fc_telemetry.bin")} {
		if _, ok := c(b); ok {
			t.Errorf("accepted %d bytes", len(b))
		}
	}
}

func TestReceiveRate(t *testing.T) {
	now := time.Unix(0, 0)
	c := NewClassifier(func() time.Time { return now })
	b := golden(t, "state.bin")
	var last health.Observation
	// 250 Hz for 2 seconds.
	for i := 0; i <= 500; i++ {
		now = time.Unix(0, 0).Add(time.Duration(i) * 4 * time.Millisecond)
		last, _ = c(b)
	}
	if got := facts(last)["state_rate"]; got != "250 per second" {
		t.Errorf("rate = %q", got)
	}
}

func TestProbeAddr(t *testing.T) {
	sc := component.DefaultStackContext()
	p := Definition().Probe(sc).(*health.UDPSubscribe)
	if p.Addr != "127.0.0.1:8765" {
		t.Errorf("addr = %s", p.Addr)
	}
	if got, err := wire.DecodeEngineSubscribe(p.Subscribe(5)); err != nil || got.Seq != 5 {
		t.Errorf("subscribe = %+v, %v", got, err)
	}
}
