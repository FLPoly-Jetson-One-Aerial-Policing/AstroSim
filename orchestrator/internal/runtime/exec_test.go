package runtime

import (
	"context"
	"os"
	"strings"
	"testing"
	"time"
)

// TestExecStream runs this test binary as a helper process (the standard
// os/exec test idiom) so no particular shell or tool is needed.
func TestExecStream(t *testing.T) {
	if os.Getenv("ASTROSIM_STREAM_HELPER") == "1" {
		os.Stdout.WriteString("to-out\n")
		os.Stderr.WriteString("to-err\n")
		if os.Getenv("ASTROSIM_STREAM_EXIT") != "" {
			os.Exit(7)
		}
		if os.Getenv("ASTROSIM_STREAM_SLEEP") != "" {
			time.Sleep(time.Minute)
		}
		os.Exit(0)
	}
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	run := func(ctx context.Context, env ...string) (string, string, int, error) {
		var out, errb strings.Builder
		// t.Setenv on the parent test accumulates, so reset the knobs first.
		t.Setenv("ASTROSIM_STREAM_HELPER", "1")
		t.Setenv("ASTROSIM_STREAM_EXIT", "")
		t.Setenv("ASTROSIM_STREAM_SLEEP", "")
		for _, e := range env {
			k, v, _ := strings.Cut(e, "=")
			t.Setenv(k, v)
		}
		code, err := ExecCommander{}.Stream(ctx, &out, &errb, self, "-test.run=^TestExecStream$")
		return out.String(), errb.String(), code, err
	}

	t.Run("separate streams", func(t *testing.T) {
		out, errs, code, err := run(context.Background())
		if err != nil || code != 0 || !strings.Contains(out, "to-out") || strings.Contains(out, "to-err") || !strings.Contains(errs, "to-err") {
			t.Fatalf("out=%q err=%q code=%d err=%v", out, errs, code, err)
		}
	})
	t.Run("exit code", func(t *testing.T) {
		_, _, code, err := run(context.Background(), "ASTROSIM_STREAM_EXIT=1")
		if err != nil || code != 7 {
			t.Fatalf("code=%d err=%v", code, err)
		}
	})
	t.Run("cancel", func(t *testing.T) {
		ctx, cancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
		defer cancel()
		_, _, _, err := run(ctx, "ASTROSIM_STREAM_SLEEP=1")
		if err != context.DeadlineExceeded {
			t.Fatalf("err = %v", err)
		}
	})
	t.Run("missing executable", func(t *testing.T) {
		_, err := ExecCommander{}.Stream(context.Background(), nil, nil, "astrosim-no-such-binary")
		if err == nil {
			t.Fatal("expected error")
		}
	})
}
