// Package wire is the minimal Go side of the gateway protocol: encode a Control, decode a State.
// Layout is defined in proto/wire.hpp; golden vectors in proto/testdata keep both sides in step.
package wire

import (
	"encoding/binary"
	"errors"
	"math"
)

const (
	Magic     = 0xA57A
	Version   = 1
	TypeCtl   = 1
	TypeState = 2

	HeaderSize  = 8
	ControlSize = HeaderSize + 16
	StateSize   = HeaderSize + 65

	FlagFailed      = 1
	FlagGzConnected = 2
)

type State struct {
	Seq     uint32
	SimTime float64
	Pos     [3]float32
	Quat    [4]float32
	Vel     [3]float32
	AngVel  [3]float32
	Flags   uint8
	EchoSeq uint32
}

func header(t byte, seq uint32, n int) []byte {
	b := make([]byte, n)
	binary.LittleEndian.PutUint16(b[0:], Magic)
	b[2] = Version
	b[3] = t
	binary.LittleEndian.PutUint32(b[4:], seq)
	return b
}

func EncodeControl(seq uint32, roll, pitch, yaw, throttle float32) []byte {
	b := header(TypeCtl, seq, ControlSize)
	for i, v := range []float32{roll, pitch, yaw, throttle} {
		binary.LittleEndian.PutUint32(b[HeaderSize+4*i:], math.Float32bits(v))
	}
	return b
}

var ErrBad = errors.New("wire: bad packet")

func DecodeState(b []byte) (State, error) {
	var s State
	if len(b) != StateSize || binary.LittleEndian.Uint16(b) != Magic || b[2] != Version || b[3] != TypeState {
		return s, ErrBad
	}
	s.Seq = binary.LittleEndian.Uint32(b[4:])
	p := HeaderSize
	f32 := func() float32 { v := math.Float32frombits(binary.LittleEndian.Uint32(b[p:])); p += 4; return v }
	s.SimTime = math.Float64frombits(binary.LittleEndian.Uint64(b[p:]))
	p += 8
	for i := range s.Pos {
		s.Pos[i] = f32()
	}
	for i := range s.Quat {
		s.Quat[i] = f32()
	}
	for i := range s.Vel {
		s.Vel[i] = f32()
	}
	for i := range s.AngVel {
		s.AngVel[i] = f32()
	}
	s.Flags = b[p]
	p++
	s.EchoSeq = binary.LittleEndian.Uint32(b[p:])
	return s, nil
}
