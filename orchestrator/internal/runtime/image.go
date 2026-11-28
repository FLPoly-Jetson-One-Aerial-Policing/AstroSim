package runtime

import "strings"

// Pullable reports whether ref names its registry explicitly, so that pulling
// it can never fall back to a default registry such as docker.io. That holds
// when ref has a '/' and its first path component contains a '.' or a ':'
// (a host name or a host:port), which is the rule the container tools
// themselves use to tell a registry from a repository namespace.
//
// A bare "localhost/" prefix is deliberately not pullable: podman files
// locally built images under it (localhost/astrosim-engine), so they can only
// be built, and pulling one would fail with a confusing network error instead
// of the build hint. "localhost:5000/x" names a registry and is pullable.
func Pullable(ref string) bool {
	host, _, ok := strings.Cut(ref, "/")
	if !ok {
		return false
	}
	return strings.ContainsAny(host, ".:")
}
