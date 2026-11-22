package wire

import "fmt"

// Flight computer telemetry protocol constants (shared/proto/FC_TELEMETRY.md).
const (
	// FCMagic is the FC telemetry magic (bytes FC A5 on the wire).
	FCMagic uint16 = 0xA5FC

	// FCTypeTelemetry is the FC to subscriber telemetry message.
	FCTypeTelemetry uint8 = 1
	// FCTypeSubscribe is the header-only subscribe/keepalive message.
	FCTypeSubscribe uint8 = 2

	// TelemetrySize is the exact size of a Telemetry datagram.
	TelemetrySize = 95
	// FCSubscribeSize is the exact size of an FC Subscribe datagram.
	FCSubscribeSize = HeaderSize
)

// Telemetry.Status bits.
const (
	// StatusConnected means the FC is connected to its autopilot.
	StatusConnected uint8 = 1 << 0
	// StatusArmed means the vehicle is armed.
	StatusArmed uint8 = 1 << 1
	// StatusModeRefused means the requested flight mode was not entered.
	StatusModeRefused uint8 = 1 << 2
)

// Flight modes carried in Telemetry.Mode.
const (
	// ModeGPSHold is GPS position hold.
	ModeGPSHold uint8 = 0
	// ModeNoGPSHold is no-GPS hold.
	ModeNoGPSHold uint8 = 1
)

// Telemetry.Provenance bits.
const (
	// ProvSensorsSynthesized means sensors are synthesized from engine truth (temporary).
	ProvSensorsSynthesized uint8 = 1 << 0
	// ProvPilotSpoofed means pilot input is a scripted spoof (temporary).
	ProvPilotSpoofed uint8 = 1 << 1
)

// Telemetry.Warnings bits.
const (
	// WarnGPSLost is set when GPS is lost.
	WarnGPSLost uint16 = 1 << 0
	// WarnEKFUnhealthy is set when the state estimator is unhealthy.
	WarnEKFUnhealthy uint16 = 1 << 1
	// WarnPilotLost is set when pilot input is lost.
	WarnPilotLost uint16 = 1 << 2
	// WarnLowBattery is set when the battery is low.
	WarnLowBattery uint16 = 1 << 3
	// WarnMotorFailure is set when a motor has failed.
	WarnMotorFailure uint16 = 1 << 4
)

// Telemetry is the FC avionics message (type 1). All values are the FC's own view.
type Telemetry struct {
	// Seq is the FC's stream counter.
	Seq uint32
	// TimeS is the simulation time of the sensor frame, in seconds.
	TimeS float64
	// Status is the bit mask of the Status* constants; reserved bits are preserved.
	Status uint8
	// Mode is the active flight mode (ModeGPSHold or ModeNoGPSHold).
	Mode uint8
	// GPSFix is 0 for no fix, 2 for 2D, 3 for 3D.
	GPSFix uint8
	// GPSSats is the number of satellites used.
	GPSSats uint8
	// Provenance is the bit mask of the Prov* constants.
	Provenance uint8
	// Warnings is the bit mask of the Warn* constants.
	Warnings uint16
	// Roll, Pitch, Heading are in radians.
	Roll, Pitch, Heading float32
	// AltMSL and AltAGL are altitudes in metres.
	AltMSL, AltAGL float32
	// VSpeed is the vertical speed in m/s, positive up.
	VSpeed float32
	// GroundSpeed is in m/s.
	GroundSpeed float32
	// Course is in radians clockwise from true north.
	Course float32
	// BatteryPct is the battery charge in percent.
	BatteryPct float32
	// BatteryV is the battery voltage in volts.
	BatteryV float32
	// Motors holds the commanded outputs in [0,1], protocol motor order.
	Motors [MotorCount]float32
}

// Connected reports whether the connected status bit is set.
func (t Telemetry) Connected() bool { return t.Status&StatusConnected != 0 }

// FCSubscribe is the header-only FC Subscribe message (type 2).
type FCSubscribe struct {
	// Seq is the sender's stream counter.
	Seq uint32
}

// DecodeFC validates and decodes an FC datagram. The returned value is a
// Telemetry or FCSubscribe (by value). Errors wrap the package sentinels,
// checked in the order the specification mandates.
func DecodeFC(b []byte) (any, error) {
	h, err := parseHeader(b, FCMagic)
	if err != nil {
		return nil, err
	}
	switch h.Type {
	case FCTypeTelemetry:
		return DecodeTelemetry(b)
	case FCTypeSubscribe:
		return DecodeFCSubscribe(b)
	default:
		return nil, fmt.Errorf("%w: %d", ErrUnknownType, h.Type)
	}
}

func decodeFCHeader(b []byte, typ uint8, size int) (Header, error) {
	h, err := parseHeader(b, FCMagic)
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

// DecodeTelemetry decodes a Telemetry datagram. A mode other than 0 or 1 is
// rejected with ErrBadValue (validation step 6).
func DecodeTelemetry(b []byte) (Telemetry, error) {
	h, err := decodeFCHeader(b, FCTypeTelemetry, TelemetrySize)
	if err != nil {
		return Telemetry{}, err
	}
	t := Telemetry{Seq: h.Seq}
	r := reader{b: b, off: HeaderSize}
	t.TimeS = r.f64()
	t.Status = r.u8()
	t.Mode = r.u8()
	if t.Mode != ModeGPSHold && t.Mode != ModeNoGPSHold {
		return Telemetry{}, fmt.Errorf("%w: mode %d", ErrBadValue, t.Mode)
	}
	t.GPSFix = r.u8()
	t.GPSSats = r.u8()
	t.Provenance = r.u8()
	t.Warnings = r.u16()
	t.Roll = r.f32()
	t.Pitch = r.f32()
	t.Heading = r.f32()
	t.AltMSL = r.f32()
	t.AltAGL = r.f32()
	t.VSpeed = r.f32()
	t.GroundSpeed = r.f32()
	t.Course = r.f32()
	t.BatteryPct = r.f32()
	t.BatteryV = r.f32()
	for i := range t.Motors {
		t.Motors[i] = r.f32()
	}
	return t, nil
}

// Encode returns the 95-byte wire form of the Telemetry message.
func (t Telemetry) Encode() []byte {
	b := make([]byte, TelemetrySize)
	putHeader(b, FCMagic, FCTypeTelemetry, t.Seq)
	w := writer{b: b, off: HeaderSize}
	w.f64(t.TimeS)
	w.u8(t.Status)
	w.u8(t.Mode)
	w.u8(t.GPSFix)
	w.u8(t.GPSSats)
	w.u8(t.Provenance)
	w.u16(t.Warnings)
	w.f32(t.Roll)
	w.f32(t.Pitch)
	w.f32(t.Heading)
	w.f32(t.AltMSL)
	w.f32(t.AltAGL)
	w.f32(t.VSpeed)
	w.f32(t.GroundSpeed)
	w.f32(t.Course)
	w.f32(t.BatteryPct)
	w.f32(t.BatteryV)
	for _, m := range t.Motors {
		w.f32(m)
	}
	return b
}

// DecodeFCSubscribe decodes an FC Subscribe datagram.
func DecodeFCSubscribe(b []byte) (FCSubscribe, error) {
	h, err := decodeFCHeader(b, FCTypeSubscribe, FCSubscribeSize)
	if err != nil {
		return FCSubscribe{}, err
	}
	return FCSubscribe{Seq: h.Seq}, nil
}

// Encode returns the 8-byte wire form of the Subscribe message.
func (s FCSubscribe) Encode() []byte {
	b := make([]byte, FCSubscribeSize)
	putHeader(b, FCMagic, FCTypeSubscribe, s.Seq)
	return b
}
