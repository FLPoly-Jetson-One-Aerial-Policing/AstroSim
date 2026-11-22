package status

import (
	"encoding/json"
	"strings"
	"testing"
	"time"
)

func TestBroadcasterCoalesces(t *testing.T) {
	b := NewBroadcaster()
	ch, cancel := b.Subscribe()
	defer cancel()
	for i := 1; i <= 5; i++ {
		b.Publish(Snapshot{Attempt: i})
	}
	select {
	case s := <-ch:
		if s.Attempt != 5 {
			t.Fatalf("got attempt %d, want latest 5", s.Attempt)
		}
	case <-time.After(time.Second):
		t.Fatal("no snapshot")
	}
	select {
	case s := <-ch:
		t.Fatalf("unexpected extra snapshot %+v", s)
	default:
	}
}

func TestBroadcasterLateSubscriberGetsLatest(t *testing.T) {
	b := NewBroadcaster()
	if _, ok := b.Latest(); ok {
		t.Fatal("Latest before Publish")
	}
	b.Publish(Snapshot{Phase: "stopped"})
	ch, cancel := b.Subscribe()
	defer cancel()
	if s := <-ch; s.Phase != "stopped" {
		t.Fatalf("got %q", s.Phase)
	}
}

func TestBroadcasterCancel(t *testing.T) {
	b := NewBroadcaster()
	_, cancel := b.Subscribe()
	cancel()
	b.Publish(Snapshot{}) // must not block or panic
	if len(b.subs) != 0 {
		t.Fatal("subscription not removed")
	}
}

func TestSnapshotJSONKeys(t *testing.T) {
	data, err := json.Marshal(Snapshot{Components: []Component{{ID: "e"}}})
	if err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{`"version"`, `"phase"`, `"since"`, `"reason"`, `"attempt"`, `"maxAttempts"`,
		`"canStart"`, `"canStop"`, `"runtime"`, `"components"`, `"buildCommand"`, `"installed"`,
		`"exitCode"`, `"lastLog"`, `"endpoints"`, `"facts"`} {
		if !strings.Contains(string(data), key) {
			t.Errorf("missing key %s in %s", key, data)
		}
	}
}
