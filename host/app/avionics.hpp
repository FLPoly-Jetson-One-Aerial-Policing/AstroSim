#pragma once
// Pseudo avionics display: attitude indicator on the left, flight and link data on the right. Drawn in 2D
// into whatever target is active (the viewer renders it to a texture shown on the cockpit screen).
#include <cmath>

#include "messages.hpp"
#include "raylib.h"

namespace astrosim::avionics {

constexpr int kWidth = 512, kHeight = 320;

struct Data {
  const char* status = "";
  Color statusColor = WHITE;
  const char* link = "";   // host:port
  const char* input = "";  // "keyboard" / "gamepad" / "none"
  bool gamepad = false;
  float alt = 0, speed = 0, vs = 0;          // m, m/s, m/s
  float pitch = 0, roll = 0, heading = 0;    // deg; nose-up and right-bank positive; compass heading
  float tilt = 0;                            // deg from level
  double simTime = 0;
  ControlMsg ctl;
};

// Flight attitude from a gz body (FLU) to world (ENU) quaternion {x, y, z, w}.
inline void attitude(const float q[4], float& pitchDeg, float& rollDeg, float& headingDeg) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  rollDeg = std::atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)) * RAD2DEG;           // +x roll lowers the right side
  pitchDeg = -std::asin(std::fmax(-1.f, std::fmin(1.f, 2 * (w * y - z * x)))) * RAD2DEG;  // FLU +y pitch is nose down
  float yaw = std::atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)) * RAD2DEG;         // ENU, from east, CCW
  headingDeg = std::fmod(90 - yaw + 720, 360.f);                                           // compass: from north, CW
}

constexpr Color kBg = {12, 14, 18, 255}, kFrame = {70, 75, 85, 255}, kSky = {40, 110, 190, 255},
                kGround = {120, 80, 40, 255}, kSymbol = {255, 200, 0, 255}, kText = {225, 235, 225, 255},
                kDim = {130, 140, 135, 255};

// Attitude indicator in the square (x, y, size): horizon and pitch ladder move, the aircraft symbol is fixed.
inline void adi(int x, int y, int size, float pitch, float roll) {
  const float cx = x + size / 2.f, cy = y + size / 2.f, pxPerDeg = size / 60.f;
  // Screen rotation of the horizon: banking right turns it counterclockwise (negative in raylib's y-down 2D).
  const float a = -roll * DEG2RAD, ca = std::cos(a), sa = std::sin(a);
  auto at = [&](float along, float down) {  // point in the horizon's rotated frame, offset from the screen center
    return Vector2{cx + along * ca - down * sa, cy + along * sa + down * ca};
  };
  BeginScissorMode(x, y, size, size);
  DrawRectangle(x, y, size, size, kSky);
  Vector2 h = at(0, pitch * pxPerDeg);  // nose up puts the horizon below center
  DrawRectanglePro({h.x, h.y, 4.f * size, 4.f * size}, {2.f * size, 0}, -roll, kGround);
  DrawLineEx(at(-2.f * size, pitch * pxPerDeg), at(2.f * size, pitch * pxPerDeg), 2, WHITE);
  for (int p = -30; p <= 30; p += 10) {
    if (p == 0) continue;
    float half = (p % 20 == 0 ? 0.22f : 0.14f) * size, down = (pitch - p) * pxPerDeg;
    DrawLineEx(at(-half, down), at(half, down), 2, WHITE);
    Vector2 l = at(half + 6, down);
    DrawText(TextFormat("%d", p < 0 ? -p : p), static_cast<int>(l.x), static_cast<int>(l.y) - 6, 12, WHITE);
  }
  EndScissorMode();
  // Fixed aircraft symbol and bank pointer.
  DrawLineEx({cx - 0.3f * size, cy}, {cx - 0.1f * size, cy}, 4, kSymbol);
  DrawLineEx({cx + 0.1f * size, cy}, {cx + 0.3f * size, cy}, 4, kSymbol);
  DrawCircleV({cx, cy}, 4, kSymbol);
  DrawTriangle({cx, static_cast<float>(y) + 4}, {cx - 7, static_cast<float>(y) + 16}, {cx + 7, static_cast<float>(y) + 16},
               kSymbol);
  DrawRectangleLines(x, y, size, size, kFrame);
}

inline void draw(const Data& d) {
  ClearBackground(kBg);
  DrawRectangleLines(1, 1, kWidth - 2, kHeight - 2, kFrame);
  // Title bar: link status.
  DrawText(d.status, 12, 8, 20, d.statusColor);
  DrawText(d.link, kWidth - 12 - MeasureText(d.link, 14), 12, 14, kDim);
  DrawLine(8, 34, kWidth - 8, 34, kFrame);

  adi(12, 44, 264, d.pitch, d.roll);

  // Data column.
  int x = 292, y = 46;
  auto row = [&](const char* label, const char* value, Color c = kText) {
    DrawText(label, x, y + 4, 14, kDim);
    DrawText(value, x + 70, y, 22, c);
    y += 30;
  };
  row("ALT", TextFormat("%6.1f m", d.alt));
  row("SPD", TextFormat("%5.1f km/h", d.speed * 3.6f));
  row("V/S", TextFormat("%+5.1f m/s", d.vs));
  row("HDG", TextFormat("%03.0f", d.heading));
  row("TILT", TextFormat("%5.1f deg", d.tilt));
  row("SIM", TextFormat("%6.1f s", d.simTime));
  row("INPUT", d.input, d.gamepad ? SKYBLUE : YELLOW);
  // Control positions as small bars, -1..1.
  y += 2;
  const char* names[4] = {"THR", "YAW", "PIT", "ROL"};
  const float vals[4] = {d.ctl.throttle, d.ctl.yaw, d.ctl.pitch, d.ctl.roll};
  for (int i = 0; i < 4; ++i) {
    int bx = x + (i % 2) * 108, by = y + (i / 2) * 22;
    DrawText(names[i], bx, by, 12, kDim);
    DrawRectangleLines(bx + 30, by + 1, 70, 10, kFrame);
    float v = std::fmax(-1.f, std::fmin(1.f, vals[i]));
    int mid = bx + 65, len = static_cast<int>(v * 34);
    DrawRectangle(len < 0 ? mid + len : mid, by + 2, len < 0 ? -len : len, 8, kSymbol);
    DrawLine(mid, by, mid, by + 12, kDim);
  }
}

}  // namespace astrosim::avionics
