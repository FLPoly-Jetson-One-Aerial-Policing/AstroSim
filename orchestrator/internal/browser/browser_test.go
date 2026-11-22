package browser

import (
	"reflect"
	"testing"
)

func TestCommand(t *testing.T) {
	const url = "http://127.0.0.1:8770"
	tests := []struct {
		goos string
		name string
		args []string
		err  bool
	}{
		{"linux", "xdg-open", []string{url}, false},
		{"darwin", "open", []string{url}, false},
		{"windows", "rundll32", []string{"url.dll,FileProtocolHandler", url}, false},
		{"plan9", "", nil, true},
	}
	for _, tt := range tests {
		name, args, err := command(tt.goos, url)
		if (err != nil) != tt.err || name != tt.name || !reflect.DeepEqual(args, tt.args) {
			t.Errorf("command(%s) = %q %q %v", tt.goos, name, args, err)
		}
	}
}
