package wire

import (
	"bytes"
	"errors"
	"math"
	"os"
	"path/filepath"
	"testing"
)

// testdataDir holds the golden vectors shared with the C++ implementation.
const testdataDir = "../../../shared/proto/testdata"

// golden loads a vector; a missing file fails the test rather than skipping,
// so a broken checkout cannot silently disable conformance.
func golden(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(testdataDir, name))
	if err != nil {
		t.Fatalf("golden vector %s: %v", name, err)
	}
	return b
}

func TestGoldenControl(t *testing.T) {
	b := golden(t, "control.bin")
	c, err := DecodeControl(b)
	if err != nil {
		t.Fatal(err)
	}
	want := [MotorCount]float32{0, 0.125, 0.25, 0.5, 0.75, 1, 0.5, 0.0625}
	if c.Seq != 7 || c.Power != want {
		t.Errorf("got %+v", c)
	}
	if got := c.Encode(); !bytes.Equal(got, b) {
		t.Errorf("re-encode differs:\n got %x\nwant %x", got, b)
	}
}

func TestGoldenState(t *testing.T) {
	b := golden(t, "state.bin")
	s, err := DecodeState(b)
	if err != nil {
		t.Fatal(err)
	}
	if s.Seq != 0xFFFFFFFE || s.SimTime != 123.456 ||
		s.Pos != (Vec3{1234.5, -678.25, 90.125}) ||
		s.Quat != (Quat{0.1, -0.2, 0.3, 0.9273618}) ||
		s.Vel != (Vec3f{1.5, -2.5, 3.5}) ||
		s.AngVel != (Vec3f{0.01, -0.02, 0.03}) ||
		s.Flags != 3 || s.EchoSeq != 4242 {
		t.Errorf("got %+v", s)
	}
	for i, m := range s.MotorSpeed {
		if m != float32(100*(i+1)) {
			t.Errorf("motor %d = %v", i, m)
		}
	}
	if !s.EngineOK() {
		t.Error("EngineOK should be set")
	}
	if got := s.Encode(); !bytes.Equal(got, b) {
		t.Errorf("re-encode differs:\n got %x\nwant %x", got, b)
	}
}

func TestGoldenEngineSubscribe(t *testing.T) {
	b := golden(t, "subscribe.bin")
	s, err := DecodeEngineSubscribe(b)
	if err != nil {
		t.Fatal(err)
	}
	if s.Seq != 1 {
		t.Errorf("seq = %d", s.Seq)
	}
	if got := s.Encode(); !bytes.Equal(got, b) {
		t.Errorf("re-encode differs: %x", got)
	}
}

func TestGoldenTelemetry(t *testing.T) {
	b := golden(t, "fc_telemetry.bin")
	tm, err := DecodeTelemetry(b)
	if err != nil {
		t.Fatal(err)
	}
	if tm.Seq != 9 || tm.TimeS != 123.456 || tm.Status != StatusConnected|StatusArmed ||
		tm.Mode != ModeNoGPSHold || tm.GPSFix != 3 || tm.GPSSats != 12 ||
		tm.Provenance != ProvSensorsSynthesized|ProvPilotSpoofed ||
		tm.Warnings != WarnEKFUnhealthy|WarnLowBattery ||
		tm.Roll != 0.05 || tm.Pitch != -0.1 || tm.Heading != 1.5 ||
		tm.AltMSL != 160.5 || tm.AltAGL != 10.25 || tm.VSpeed != 0.75 ||
		tm.GroundSpeed != 2.5 || tm.Course != 3.0 ||
		tm.BatteryPct != 87.5 || tm.BatteryV != 48.25 {
		t.Errorf("got %+v", tm)
	}
	for i, m := range tm.Motors {
		if m != float32(0.1*float64(i+1)) && math.Abs(float64(m)-0.1*float64(i+1)) > 1e-6 {
			t.Errorf("motor %d = %v", i, m)
		}
	}
	if !tm.Connected() {
		t.Error("Connected should be set")
	}
	if got := tm.Encode(); !bytes.Equal(got, b) {
		t.Errorf("re-encode differs:\n got %x\nwant %x", got, b)
	}
}

func TestGoldenFCSubscribe(t *testing.T) {
	b := golden(t, "fc_subscribe.bin")
	s, err := DecodeFCSubscribe(b)
	if err != nil {
		t.Fatal(err)
	}
	if s.Seq != 1 {
		t.Errorf("seq = %d", s.Seq)
	}
	if got := s.Encode(); !bytes.Equal(got, b) {
		t.Errorf("re-encode differs: %x", got)
	}
}

func TestDecodeDispatch(t *testing.T) {
	if v, err := DecodeEngine(golden(t, "state.bin")); err != nil {
		t.Fatal(err)
	} else if _, ok := v.(State); !ok {
		t.Errorf("got %T", v)
	}
	if v, err := DecodeEngine(golden(t, "control.bin")); err != nil {
		t.Fatal(err)
	} else if _, ok := v.(Control); !ok {
		t.Errorf("got %T", v)
	}
	if v, err := DecodeEngine(golden(t, "subscribe.bin")); err != nil {
		t.Fatal(err)
	} else if _, ok := v.(EngineSubscribe); !ok {
		t.Errorf("got %T", v)
	}
	if v, err := DecodeFC(golden(t, "fc_telemetry.bin")); err != nil {
		t.Fatal(err)
	} else if _, ok := v.(Telemetry); !ok {
		t.Errorf("got %T", v)
	}
	if v, err := DecodeFC(golden(t, "fc_subscribe.bin")); err != nil {
		t.Fatal(err)
	} else if _, ok := v.(FCSubscribe); !ok {
		t.Errorf("got %T", v)
	}
}

// mutate returns a copy of b with f applied.
func mutate(b []byte, f func([]byte) []byte) []byte {
	return f(append([]byte(nil), b...))
}

func TestMalformedEngine(t *testing.T) {
	state := golden(t, "state.bin")
	ctrl := golden(t, "control.bin")
	sub := golden(t, "subscribe.bin")
	tests := []struct {
		name string
		in   []byte
		want error
	}{
		{"nil", nil, ErrTooShort},
		{"seven bytes", state[:7], ErrTooShort},
		{"bad magic", mutate(state, func(b []byte) []byte { b[0] = 0; return b }), ErrBadMagic},
		{"bad version 0", mutate(state, func(b []byte) []byte { b[2] = 0; return b }), ErrBadVersion},
		{"bad version 2", mutate(state, func(b []byte) []byte { b[2] = 2; return b }), ErrBadVersion},
		{"type 0", mutate(state, func(b []byte) []byte { b[3] = 0; return b }), ErrUnknownType},
		{"type 4", mutate(state, func(b []byte) []byte { b[3] = 4; return b }), ErrUnknownType},
		{"state truncated", state[:StateSize-1], ErrBadSize},
		{"state padded", append(append([]byte(nil), state...), 0), ErrBadSize},
		{"control truncated", ctrl[:ControlSize-1], ErrBadSize},
		{"control as state size", mutate(state, func(b []byte) []byte { b[3] = 1; return b }), ErrBadSize},
		{"subscribe padded", append(append([]byte(nil), sub...), 0), ErrBadSize},
		{"magic checked before version", []byte{0, 0, 9, 9, 0, 0, 0, 0}, ErrBadMagic},
		{"fc telemetry", golden(t, "fc_telemetry.bin"), ErrBadMagic},
		{"fc subscribe", golden(t, "fc_subscribe.bin"), ErrBadMagic},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := DecodeEngine(tc.in); !errors.Is(err, tc.want) {
				t.Errorf("err = %v, want %v", err, tc.want)
			}
		})
	}
}

func TestMalformedFC(t *testing.T) {
	tel := golden(t, "fc_telemetry.bin")
	sub := golden(t, "fc_subscribe.bin")
	tests := []struct {
		name string
		in   []byte
		want error
	}{
		{"nil", nil, ErrTooShort},
		{"seven bytes", tel[:7], ErrTooShort},
		{"bad magic", mutate(tel, func(b []byte) []byte { b[1] = 0; return b }), ErrBadMagic},
		{"bad version", mutate(tel, func(b []byte) []byte { b[2] = 2; return b }), ErrBadVersion},
		{"type 0", mutate(tel, func(b []byte) []byte { b[3] = 0; return b }), ErrUnknownType},
		{"type 3", mutate(tel, func(b []byte) []byte { b[3] = 3; return b }), ErrUnknownType},
		{"telemetry truncated", tel[:TelemetrySize-1], ErrBadSize},
		{"telemetry padded", append(append([]byte(nil), tel...), 0), ErrBadSize},
		{"subscribe padded", append(append([]byte(nil), sub...), 0), ErrBadSize},
		{"mode 2", mutate(tel, func(b []byte) []byte { b[17] = 2; return b }), ErrBadValue},
		{"mode 255", mutate(tel, func(b []byte) []byte { b[17] = 255; return b }), ErrBadValue},
		{"engine state", golden(t, "state.bin"), ErrBadMagic},
		{"engine subscribe", golden(t, "subscribe.bin"), ErrBadMagic},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := DecodeFC(tc.in); !errors.Is(err, tc.want) {
				t.Errorf("err = %v, want %v", err, tc.want)
			}
		})
	}
}

// TestReservedBitsPreserved checks that reserved flag bits survive a
// round trip, so a decode/encode proxy would not alter datagrams.
func TestReservedBitsPreserved(t *testing.T) {
	b := golden(t, "state.bin")
	b[112] = 0xFF
	s, err := DecodeState(b)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(s.Encode(), b) {
		t.Error("reserved flag bits not preserved")
	}
}

// FuzzDecode asserts the decoders never panic and that anything they accept
// re-encodes to the same bytes.
func FuzzDecode(f *testing.F) {
	for _, n := range []string{"control.bin", "state.bin", "subscribe.bin", "fc_telemetry.bin", "fc_subscribe.bin"} {
		if b, err := os.ReadFile(filepath.Join(testdataDir, n)); err == nil {
			f.Add(b)
		}
	}
	f.Fuzz(func(t *testing.T, b []byte) {
		if v, err := DecodeEngine(b); err == nil {
			var out []byte
			switch m := v.(type) {
			case Control:
				out = m.Encode()
			case State:
				out = m.Encode()
			case EngineSubscribe:
				out = m.Encode()
			}
			if !bytes.Equal(out, b) {
				t.Fatalf("engine round trip differs: %x vs %x", out, b)
			}
		}
		if v, err := DecodeFC(b); err == nil {
			var out []byte
			switch m := v.(type) {
			case Telemetry:
				out = m.Encode()
			case FCSubscribe:
				out = m.Encode()
			}
			if !bytes.Equal(out, b) {
				t.Fatalf("fc round trip differs: %x vs %x", out, b)
			}
		}
	})
}
