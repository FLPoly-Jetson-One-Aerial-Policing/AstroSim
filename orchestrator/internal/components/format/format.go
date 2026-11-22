// Package format renders values for the plain-language facts that
// operators read in the dashboard.
package format

import (
	"fmt"
	"math"
)

// SimTime renders simulation time in seconds as "M:SS.t", or "H:MM:SS.t"
// from one hour on. Non-finite or negative values render as "unknown".
func SimTime(seconds float64) string {
	if math.IsNaN(seconds) || math.IsInf(seconds, 0) || seconds < 0 {
		return "unknown"
	}
	// Round to tenths first so 59.96 s becomes 1:00.0 rather than 0:60.0.
	t := int64(math.Round(seconds * 10))
	tenth, s := t%10, t/10
	h, m, s := s/3600, s/60%60, s%60
	if h > 0 {
		return fmt.Sprintf("%d:%02d:%02d.%d", h, m, s, tenth)
	}
	return fmt.Sprintf("%d:%02d.%d", m, s, tenth)
}
