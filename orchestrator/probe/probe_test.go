package probe

import (
	"context"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"astrosim/orchestrator/wire"
)

// fakeGateway answers every Control with the golden State, with the gz flag set after `after` packets.
func fakeGateway(t *testing.T, after int) (port int, stop func()) {
	t.Helper()
	pc, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	gold, err := os.ReadFile(filepath.Join("..", "..", "shared", "proto", "testdata", "state.bin"))
	if err != nil {
		t.Fatal(err)
	}
	go func() {
		buf := make([]byte, 64)
		n := 0
		for {
			_, addr, err := pc.ReadFrom(buf)
			if err != nil {
				return
			}
			n++
			pkt := append([]byte{}, gold...)
			if n <= after {
				pkt[wire.StateSize-5] = 0 // flags byte: engine not connected yet
			}
			pc.WriteTo(pkt, addr)
		}
	}()
	return pc.LocalAddr().(*net.UDPAddr).Port, func() { pc.Close() }
}

func TestReadyWhenFlagSet(t *testing.T) {
	port, stop := fakeGateway(t, 3)
	defer stop()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := WaitReady(ctx, "127.0.0.1", port, 20*time.Millisecond)
	if err != nil || s.Flags&wire.FlagGzConnected == 0 {
		t.Fatalf("err=%v state=%+v", err, s)
	}
}

func TestTimesOutWithNoGateway(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
	defer cancel()
	if _, err := WaitReady(ctx, "127.0.0.1", 1, 20*time.Millisecond); err == nil {
		t.Fatal("expected timeout")
	}
}

func TestTimesOutWhenEngineNeverConnects(t *testing.T) {
	port, stop := fakeGateway(t, 1<<30)
	defer stop()
	ctx, cancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
	defer cancel()
	if _, err := WaitReady(ctx, "127.0.0.1", port, 20*time.Millisecond); err == nil {
		t.Fatal("expected timeout")
	}
}
