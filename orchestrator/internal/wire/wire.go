// Package wire decodes and encodes the AstroSim UDP wire protocols byte-exactly.
//
// Two protocols are implemented, each in its own file:
//
//   - the engine protocol (engine.go), specified in shared/proto/PROTOCOL.md
//   - the flight computer telemetry protocol (fc.go), specified in
//     shared/proto/FC_TELEMETRY.md
//
// Both use an 8-byte little-endian header with distinct magic numbers, so a
// datagram for one protocol is always rejected by the other's decoder.
// Decoders never panic on arbitrary input and report failures with the
// sentinel errors below, in the validation order the specifications mandate.
package wire

import (
	"encoding/binary"
	"errors"
	"math"
)

// Validation errors returned by the decoders. Callers match them with
// errors.Is; the concrete error may carry extra context.
var (
	// ErrTooShort means the datagram is shorter than the 8-byte header.
	ErrTooShort = errors.New("wire: datagram shorter than header")
	// ErrBadMagic means the magic number does not belong to this protocol.
	ErrBadMagic = errors.New("wire: bad magic")
	// ErrBadVersion means the protocol version is not supported.
	ErrBadVersion = errors.New("wire: unsupported version")
	// ErrUnknownType means the message type is not defined by this protocol.
	ErrUnknownType = errors.New("wire: unknown message type")
	// ErrBadSize means the datagram length is wrong for its message type.
	ErrBadSize = errors.New("wire: bad datagram size")
	// ErrBadValue means a field holds a value outside its defined set.
	ErrBadValue = errors.New("wire: bad field value")
)

// Version is the protocol version both protocols currently speak.
const Version = 1

// HeaderSize is the size in bytes of the header shared by all messages.
const HeaderSize = 8

// Header is the 8-byte header that starts every message of either protocol.
// The magic is implied by the protocol and therefore not stored.
type Header struct {
	// Version is the protocol version (always Version for valid messages).
	Version uint8
	// Type is the protocol-specific message type.
	Type uint8
	// Seq is the sender's per-stream counter; it wraps at 2^32.
	Seq uint32
}

// parseHeader performs validation steps 1 to 3 (length, magic, version).
// The type and size checks are protocol specific and left to the caller.
func parseHeader(b []byte, magic uint16) (Header, error) {
	if len(b) < HeaderSize {
		return Header{}, ErrTooShort
	}
	if binary.LittleEndian.Uint16(b[0:2]) != magic {
		return Header{}, ErrBadMagic
	}
	if b[2] != Version {
		return Header{}, ErrBadVersion
	}
	return Header{Version: b[2], Type: b[3], Seq: binary.LittleEndian.Uint32(b[4:8])}, nil
}

// putHeader writes the header for the given magic into b[0:8].
func putHeader(b []byte, magic uint16, typ uint8, seq uint32) {
	binary.LittleEndian.PutUint16(b[0:2], magic)
	b[2] = Version
	b[3] = typ
	binary.LittleEndian.PutUint32(b[4:8], seq)
}

// reader and writer are tiny cursors over a pre-sized buffer. Callers have
// already verified the length, so they never run past the end.
type reader struct {
	b   []byte
	off int
}

func (r *reader) u8() uint8 {
	v := r.b[r.off]
	r.off++
	return v
}

func (r *reader) u16() uint16 {
	v := binary.LittleEndian.Uint16(r.b[r.off:])
	r.off += 2
	return v
}

func (r *reader) u32() uint32 {
	v := binary.LittleEndian.Uint32(r.b[r.off:])
	r.off += 4
	return v
}

func (r *reader) f32() float32 { return math.Float32frombits(r.u32()) }

func (r *reader) f64() float64 {
	v := binary.LittleEndian.Uint64(r.b[r.off:])
	r.off += 8
	return math.Float64frombits(v)
}

type writer struct {
	b   []byte
	off int
}

func (w *writer) u8(v uint8) {
	w.b[w.off] = v
	w.off++
}

func (w *writer) u16(v uint16) {
	binary.LittleEndian.PutUint16(w.b[w.off:], v)
	w.off += 2
}

func (w *writer) u32(v uint32) {
	binary.LittleEndian.PutUint32(w.b[w.off:], v)
	w.off += 4
}

func (w *writer) f32(v float32) { w.u32(math.Float32bits(v)) }

func (w *writer) f64(v float64) {
	binary.LittleEndian.PutUint64(w.b[w.off:], math.Float64bits(v))
	w.off += 8
}
