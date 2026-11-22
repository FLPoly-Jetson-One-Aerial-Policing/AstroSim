package format

import (
	"math"
	"testing"
)

func TestSimTime(t *testing.T) {
	for _, tc := range []struct {
		in   float64
		want string
	}{
		{0, "0:00.0"}, {123.456, "2:03.5"}, {59.96, "1:00.0"}, {3600, "1:00:00.0"},
		{3725.25, "1:02:05.3"}, {-1, "unknown"}, {math.NaN(), "unknown"}, {math.Inf(1), "unknown"},
	} {
		if got := SimTime(tc.in); got != tc.want {
			t.Errorf("SimTime(%v) = %q, want %q", tc.in, got, tc.want)
		}
	}
}
