package wire

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

func golden(t *testing.T, name string) []byte {
	b, err := os.ReadFile(filepath.Join("..", "..", "shared", "proto", "testdata", name))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestControlMatchesGolden(t *testing.T) {
	got := EncodeControl(7, 0.5, -0.25, 1.0, -1.0)
	if !bytes.Equal(got, golden(t, "control.bin")) {
		t.Fatalf("control mismatch:\n got %x\nwant %x", got, golden(t, "control.bin"))
	}
}

func TestStateDecodesGolden(t *testing.T) {
	s, err := DecodeState(golden(t, "state.bin"))
	if err != nil {
		t.Fatal(err)
	}
	if s.Seq != 0xFFFFFFFE || s.SimTime != 12.345 || s.Pos != [3]float32{1.5, -2.5, 3.25} ||
		s.Quat[3] != 0.70710678 || s.Vel[2] != -0.3 || s.AngVel[2] != 0.03 ||
		s.Flags != FlagGzConnected || s.EchoSeq != 4242 {
		t.Fatalf("bad decode: %+v", s)
	}
}

func TestDecodeRejects(t *testing.T) {
	good := golden(t, "state.bin")
	if _, err := DecodeState(nil); err == nil {
		t.Error("nil accepted")
	}
	for n := 0; n < len(good); n++ {
		if _, err := DecodeState(good[:n]); err == nil {
			t.Fatalf("truncated %d accepted", n)
		}
	}
	if _, err := DecodeState(append(append([]byte{}, good...), 0)); err == nil {
		t.Error("oversized accepted")
	}
	for _, mut := range []func(b []byte){
		func(b []byte) { b[0] ^= 0xff }, // magic
		func(b []byte) { b[2] = 9 },     // version
		func(b []byte) { b[3] = 1 },     // wrong type
	} {
		b := append([]byte{}, good...)
		mut(b)
		if _, err := DecodeState(b); err == nil {
			t.Error("bad header accepted")
		}
	}
	if _, err := DecodeState(golden(t, "control.bin")); err == nil {
		t.Error("control accepted as state")
	}
}
