package podman

import (
	"strings"
	"testing"
)

func TestRunArgs(t *testing.T) {
	got := strings.Join(RunArgs(RunOpts{Name: "n", Image: "img", Port: 9000, TickHz: 120}), " ")
	want := "run --rm --name n -p 9000:8765/udp -e TICK_HZ=120 img"
	if got != want {
		t.Fatalf("got %q want %q", got, want)
	}
	got = strings.Join(RunArgs(RunOpts{Name: "n", Image: "img", HostNet: true}), " ")
	if got != "run --rm --name n --network=host img" {
		t.Fatalf("got %q", got)
	}
}
