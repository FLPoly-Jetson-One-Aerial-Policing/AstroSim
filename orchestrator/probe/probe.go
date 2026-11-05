// Package probe waits for the gateway to report a live engine, speaking the real wire protocol.
package probe

import (
	"context"
	"net"
	"strconv"
	"time"

	"astrosim/orchestrator/wire"
)

// WaitReady sends neutral Control packets until a State with the gz-connected flag arrives.
func WaitReady(ctx context.Context, host string, port int, interval time.Duration) (wire.State, error) {
	conn, err := net.Dial("udp", net.JoinHostPort(host, strconv.Itoa(port)))
	if err != nil {
		return wire.State{}, err
	}
	defer conn.Close()
	var seq uint32
	buf := make([]byte, 512)
	tick := time.NewTicker(interval)
	defer tick.Stop()
	for {
		seq++
		_, _ = conn.Write(wire.EncodeControl(seq, 0, 0, 0, 0)) // ECONNREFUSED while the engine boots is expected
		_ = conn.SetReadDeadline(time.Now().Add(interval))
		for {
			n, err := conn.Read(buf)
			if err != nil {
				break
			}
			if s, err := wire.DecodeState(buf[:n]); err == nil && s.Flags&wire.FlagGzConnected != 0 {
				return s, nil
			}
		}
		select {
		case <-ctx.Done():
			return wire.State{}, ctx.Err()
		case <-tick.C:
		}
	}
}
