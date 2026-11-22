// Package web embeds the dashboard's static files (plain HTML, CSS and ES
// modules; no framework, no external assets).
package web

import "embed"

// Static holds the dashboard under the "static" directory.
//
//go:embed static
var Static embed.FS
