package wire

import "fmt"

// Engine protocol constants (shared/proto/PROTOCOL.md).
const (
	// EngineMagic is the engine protocol magic (bytes 7A A5 on the wire).
	EngineMagic uint16 = 0xA57A

	// EngineTypeControl is the flight computer to engine actuator command.
	EngineTypeControl uint8 = 1
	// EngineTypeState is the engine to subscriber state message.
	EngineTypeState uint8 = 2
	// EngineTypeSubscribe is the header-only subscribe/keepalive message.
	EngineTypeSubscribe uint8 = 3

	// ControlSize is the exact size of a Control datagram.
	ControlSize = 40
	// StateSize is the exact size of a State datagram.
	StateSize = 117
	// EngineSubscribeSize is the exact size of an engine Subscribe datagram.
	EngineSubscribeSize = HeaderSize

	// MotorCount is the fixed number of motors in the protocol.
	MotorCount = 8
)

// State.Flags bits.
const (
	// FlagEngineOK means the engine state (odometry) is fresh.
	FlagEngineOK uint8 = 1 << 0
	// FlagControlActive means a controller holds the lease and is within the deadman window.
	FlagControlActive uint8 = 1 << 1
	// FlagDeadmanTripped means motors were zeroed because Control stopped arriving.
	FlagDeadmanTripped uint8 = 1 << 2
)

// Vec3 is a double-precision 3-vector (world position, metres).
type Vec3 struct{ X, Y, Z float64 }

// Vec3f is a single-precision 3-vector (velocity or angular velocity).
type Vec3f struct{ X, Y, Z float32 }

// Quat is a unit quaternion (x, y, z, w) rotating body-frame vectors into the world frame.
type Quat struct{ X, Y, Z, W float32 }

// Control is a flight computer to engine actuator command (type 1).
type Control struct {
	// Seq is the sender's stream counter.
	Seq uint32
	// Power holds the normalized [0,1] power for each protocol motor.
	// Decoding does not alter values (NaN and out-of-range values pass through).
	Power [MotorCount]float32
}

// State is an engine to subscriber state message (type 2).
type State struct {
	// Seq is the engine's own stream counter.
	Seq uint32
	// SimTime is the simulation time in seconds.
	SimTime float64
	// Pos is the world ENU position in metres.
	Pos Vec3
	// Quat is the body to world orientation.
	Quat Quat
	// Vel is the world ENU velocity in m/s.
	Vel Vec3f
	// AngVel is the body FLU angular velocity in rad/s.
	AngVel Vec3f
	// MotorSpeed holds the motor speeds in rad/s, protocol motor order.
	MotorSpeed [MotorCount]float32
	// Flags is the bit mask of the Flag* constants; reserved bits are preserved.
	Flags uint8
	// EchoSeq is the Seq of the last applied Control, 0 before any.
	EchoSeq uint32
}

// EngineOK reports whether the engine state is fresh (FlagEngineOK).
func (s State) EngineOK() bool { return s.Flags&FlagEngineOK != 0 }

// EngineSubscribe is the header-only engine Subscribe message (type 3).
type EngineSubscribe struct {
	// Seq is the sender's stream counter.
	Seq uint32
}

// DecodeEngine validates and decodes an engine datagram. The returned value
// is a Control, State or EngineSubscribe (by value). Errors wrap the
// sentinels in this package, checked in the order the specification mandates.
func DecodeEngine(b []byte) (any, error) {
	h, err := parseHeader(b, EngineMagic)
	if err != nil {
		return nil, err
	}
	switch h.Type {
	case EngineTypeControl:
		return DecodeControl(b)
	case EngineTypeState:
		return DecodeState(b)
	case EngineTypeSubscribe:
		return DecodeEngineSubscribe(b)
	default:
		return nil, fmt.Errorf("%w: %d", ErrUnknownType, h.Type)
	}
}

// decodeEngineHeader validates the header and that it carries the wanted
// type and exact size.
func decodeEngineHeader(b []byte, typ uint8, size int) (Header, error) {
	h, err := parseHeader(b, EngineMagic)
	if err != nil {
		return Header{}, err
	}
	if h.Type != typ {
		return Header{}, fmt.Errorf("%w: got %d, want %d", ErrUnknownType, h.Type, typ)
	}
	if len(b) != size {
		return Header{}, fmt.Errorf("%w: got %d, want %d", ErrBadSize, len(b), size)
	}
	return h, nil
}

// DecodeControl decodes a Control datagram.
func DecodeControl(b []byte) (Control, error) {
	h, err := decodeEngineHeader(b, EngineTypeControl, ControlSize)
	if err != nil {
		return Control{}, err
	}
	c := Control{Seq: h.Seq}
	r := reader{b: b, off: HeaderSize}
	for i := range c.Power {
		c.Power[i] = r.f32()
	}
	return c, nil
}

// Encode returns the 40-byte wire form of the Control message.
func (c Control) Encode() []byte {
	b := make([]byte, ControlSize)
	putHeader(b, EngineMagic, EngineTypeControl, c.Seq)
	w := writer{b: b, off: HeaderSize}
	for _, p := range c.Power {
		w.f32(p)
	}
	return b
}

// DecodeState decodes a State datagram.
func DecodeState(b []byte) (State, error) {
	h, err := decodeEngineHeader(b, EngineTypeState, StateSize)
	if err != nil {
		return State{}, err
	}
	s := State{Seq: h.Seq}
	r := reader{b: b, off: HeaderSize}
	s.SimTime = r.f64()
	s.Pos = Vec3{r.f64(), r.f64(), r.f64()}
	s.Quat = Quat{r.f32(), r.f32(), r.f32(), r.f32()}
	s.Vel = Vec3f{r.f32(), r.f32(), r.f32()}
	s.AngVel = Vec3f{r.f32(), r.f32(), r.f32()}
	for i := range s.MotorSpeed {
		s.MotorSpeed[i] = r.f32()
	}
	s.Flags = r.u8()
	s.EchoSeq = r.u32()
	return s, nil
}

// Encode returns the 117-byte wire form of the State message.
func (s State) Encode() []byte {
	b := make([]byte, StateSize)
	putHeader(b, EngineMagic, EngineTypeState, s.Seq)
	w := writer{b: b, off: HeaderSize}
	w.f64(s.SimTime)
	w.f64(s.Pos.X)
	w.f64(s.Pos.Y)
	w.f64(s.Pos.Z)
	w.f32(s.Quat.X)
	w.f32(s.Quat.Y)
	w.f32(s.Quat.Z)
	w.f32(s.Quat.W)
	w.f32(s.Vel.X)
	w.f32(s.Vel.Y)
	w.f32(s.Vel.Z)
	w.f32(s.AngVel.X)
	w.f32(s.AngVel.Y)
	w.f32(s.AngVel.Z)
	for _, m := range s.MotorSpeed {
		w.f32(m)
	}
	w.u8(s.Flags)
	w.u32(s.EchoSeq)
	return b
}

// DecodeEngineSubscribe decodes an engine Subscribe datagram.
func DecodeEngineSubscribe(b []byte) (EngineSubscribe, error) {
	h, err := decodeEngineHeader(b, EngineTypeSubscribe, EngineSubscribeSize)
	if err != nil {
		return EngineSubscribe{}, err
	}
	return EngineSubscribe{Seq: h.Seq}, nil
}

// Encode returns the 8-byte wire form of the Subscribe message.
func (s EngineSubscribe) Encode() []byte {
	b := make([]byte, EngineSubscribeSize)
	putHeader(b, EngineMagic, EngineTypeSubscribe, s.Seq)
	return b
}
