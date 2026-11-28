package runtime

import "testing"

func TestPullable(t *testing.T) {
	for _, tc := range []struct {
		ref  string
		want bool
	}{
		{"astrosim-engine", false},
		{"astrosim-engine:dev", false},
		{"library/x", false},
		{"localhost/x", false},
		{"localhost/astrosim-engine:dev", false},
		{"ghcr.io", false},
		{"", false},
		{"/x", false},
		{"ghcr.io/o/x:1", true},
		{"docker.io/library/x", true},
		{"localhost:5000/x", true},
		{"registry:5000/x", true},
	} {
		if got := Pullable(tc.ref); got != tc.want {
			t.Errorf("Pullable(%q) = %v, want %v", tc.ref, got, tc.want)
		}
	}
}
