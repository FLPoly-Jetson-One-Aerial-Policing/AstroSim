#pragma once
// Jetson ONE drawn from raylib primitives, matching container/models/jetson_one (2.70 x 1.60 x 1.12 m).
// Body frame in raylib axes: +x forward, +y up, +z right. Origin is the gz base_link, kRideHeight above the ground.
#include <cmath>

#include "messages.hpp"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

namespace astrosim::jetson {

constexpr float kRideHeight = 0.45f;            // base_link above ground when landed
constexpr Vector3 kEye = {0.02f, 0.48f, 0.0f};  // pilot eye point, ~0.93 m above ground, helmet clears the cage
constexpr float kPropRadius = 0.30f;
// Rotor stack centers (gz rotor_0..3 at x=+-1.05, y=+-0.5, z=0.45; gz y-left becomes raylib -z).
constexpr Vector3 kRotors[4] = {{1.05f, 0.45f, 0.5f}, {-1.05f, 0.45f, -0.5f}, {1.05f, 0.45f, -0.5f}, {-1.05f, 0.45f, 0.5f}};
constexpr bool kRotorCcw[4] = {true, true, false, false};

constexpr Color kWhite = {232, 232, 228, 255}, kCarbon = {38, 38, 42, 255}, kRed = {200, 30, 36, 255};
constexpr Color kEdge = {20, 20, 20, 255}, kSuit = {50, 70, 110, 255}, kHelmet = {240, 240, 240, 255};

// Avionics screen on a post above the nose, tilted to face the pilot's eye. Local frame: +x away from the
// pilot (into the screen), +y up the screen, +z right.
constexpr Vector3 kScreenCenter = {0.45f, 0.25f, 0.0f};
constexpr float kScreenTiltDeg = -28.2f;  // about body z, so local +y points at the eye's elevation
constexpr float kScreenHalfW = 0.12f, kScreenHalfH = 0.075f;

inline void box(Vector3 c, Vector3 s, Color col) {
  DrawCubeV(c, s, col);
  DrawCubeWiresV(c, s, kEdge);
}

inline void rod(Vector3 a, Vector3 b, float r, Color col) { DrawCylinderEx(a, b, r, r, 8, col); }

// Two-blade prop plus a translucent blur disc; angle in degrees.
inline void prop(Vector3 c, float angle, bool spinning) {
  rlPushMatrix();
  rlTranslatef(c.x, c.y, c.z);
  rlRotatef(angle, 0, 1, 0);
  DrawCube({0, 0, 0}, 2 * kPropRadius, 0.012f, 0.05f, kCarbon);
  rlPopMatrix();
  if (spinning) DrawCylinder({c.x, c.y - 0.003f, c.z}, kPropRadius, kPropRadius, 0.006f, 32, {200, 200, 210, 60});
}

// Seated pilot. The head is skipped in first person (the camera sits inside it).
inline void pilot(const ControlMsg& ctl, bool head) {
  const Vector3 hip = {-0.18f, -0.12f, 0}, chest = {-0.2f, 0.32f, 0};
  DrawCapsule(hip, chest, 0.15f, 8, 4, kSuit);
  for (float side : {-1.f, 1.f}) {
    Vector3 h = {hip.x, hip.y, 0.1f * side}, knee = {0.28f, -0.02f, 0.13f * side}, foot = {0.42f, -0.26f, 0.13f * side};
    DrawCapsule(h, knee, 0.07f, 6, 3, kSuit);
    DrawCapsule(knee, foot, 0.06f, 6, 3, kSuit);
  }
  // Right hand on the joystick, left hand on the throttle lever; both follow the live input.
  Vector3 stickBase = {0.2f, -0.05f, 0.24f};
  Vector3 stickTop = {stickBase.x + 0.06f * ctl.pitch, stickBase.y + 0.22f, stickBase.z + 0.06f * ctl.roll};
  Vector3 lever = {0.05f + 0.08f * ctl.throttle, 0.12f, -0.26f};
  rod(stickBase, stickTop, 0.012f, kCarbon);
  DrawSphere(stickTop, 0.025f, kRed);
  rod({0.0f, -0.05f, -0.26f}, lever, 0.012f, kCarbon);
  DrawSphere(lever, 0.025f, kRed);
  DrawCapsule({chest.x, chest.y, 0.17f}, stickTop, 0.05f, 6, 3, kSuit);
  DrawCapsule({chest.x, chest.y, -0.17f}, lever, 0.05f, 6, 3, kSuit);
  if (head) {
    DrawSphere({kEye.x - 0.06f, kEye.y, 0}, 0.13f, kHelmet);
    DrawSphere({kEye.x + 0.02f, kEye.y, 0}, 0.09f, {30, 30, 40, 255});  // visor
  }
}

// Screen bezel plus the avionics render texture (raylib render textures are stored bottom-up, so v is flipped).
inline void screen(Texture2D tex) {
  rod({0.56f, 0.08f, 0}, {kScreenCenter.x + 0.03f, kScreenCenter.y - 0.04f, 0}, 0.015f, kCarbon);  // mount post
  rlPushMatrix();
  rlTranslatef(kScreenCenter.x, kScreenCenter.y, kScreenCenter.z);
  rlRotatef(kScreenTiltDeg, 0, 0, 1);
  box({0.012f, 0, 0}, {0.02f, 2 * kScreenHalfH + 0.03f, 2 * kScreenHalfW + 0.03f}, kCarbon);
  const float x = -0.001f, h = kScreenHalfH, w = kScreenHalfW;
  rlSetTexture(tex.id);
  rlBegin(RL_QUADS);
  rlColor4ub(255, 255, 255, 255);
  rlNormal3f(-1, 0, 0);
  rlTexCoord2f(0, 1); rlVertex3f(x, h, -w);   // top left, as the pilot sees it
  rlTexCoord2f(0, 0); rlVertex3f(x, -h, -w);
  rlTexCoord2f(1, 0); rlVertex3f(x, -h, w);
  rlTexCoord2f(1, 1); rlVertex3f(x, h, w);
  rlEnd();
  rlSetTexture(0);
  rlPopMatrix();
}

// Overhead roll cage: arched side rails from the nose up over the pilot to the rear hoop, with a front
// crossbar. Its top (0.67 m) is the 1.12 m envelope height. Translucent mode keeps it out of the pilot's view.
inline void cage(bool translucent) {
  const Color col = translucent ? Color{232, 232, 228, 60} : kWhite;
  constexpr float kTop = 0.67f, kZ = 0.25f, kFront = 0.15f, kRear = -0.36f;
  for (float side : {-1.f, 1.f}) {
    const float z = kZ * side;
    // Quarter ellipse from the tub's front corner (0.55, 0.08) to the top of the rail.
    Vector3 prev = {0.55f, 0.08f, z};
    for (int i = 1; i <= 6; ++i) {
      float a = i * (PI / 2) / 6;
      Vector3 p = {kFront + 0.4f * std::cos(a), 0.08f + (kTop - 0.08f) * std::sin(a), z};
      rod(prev, p, 0.022f, col);
      prev = p;
    }
    rod(prev, {kRear, kTop, z}, 0.022f, col);
  }
  rod({kFront, kTop, -kZ}, {kFront, kTop, kZ}, 0.022f, col);
}

// Whole aircraft. firstPerson hides the pilot's head; propAngle animates the blades (degrees); avionics is the
// screen texture; cageTranslucent ghosts the roll cage.
inline void draw(Vector3 pos, Quaternion q, const ControlMsg& ctl, bool firstPerson, bool spinning, float propAngle,
                 Texture2D avionics, bool cageTranslucent) {
  rlPushMatrix();
  rlTranslatef(pos.x, pos.y, pos.z);
  rlMultMatrixf(MatrixToFloat(QuaternionToMatrix(q)));

  // Cockpit tub: open-topped, so the pilot can see over the sides.
  box({0, -0.32f, 0}, {1.2f, 0.06f, 0.62f}, kCarbon);
  box({0, -0.12f, 0.29f}, {1.2f, 0.4f, 0.04f}, kWhite);
  box({0, -0.12f, -0.29f}, {1.2f, 0.4f, 0.04f}, kWhite);
  box({0.58f, -0.12f, 0}, {0.04f, 0.4f, 0.62f}, kRed);
  box({0.3f, 0.09f, 0.29f}, {0.5f, 0.02f, 0.05f}, kRed);  // side stripes
  box({0.3f, 0.09f, -0.29f}, {0.5f, 0.02f, 0.05f}, kRed);
  box({-0.42f, -0.05f, 0}, {0.36f, 0.5f, 0.58f}, kCarbon);  // battery bay behind the seat
  box({-0.12f, -0.23f, 0}, {0.3f, 0.12f, 0.5f}, kCarbon);   // seat pan
  box({-0.34f, 0.3f, 0}, {0.06f, 0.6f, 0.46f}, kCarbon);    // seat back
  // Roll hoop behind the head defines the 1.12 m height.
  rod({-0.36f, 0.2f, 0.25f}, {-0.36f, 0.67f, 0.25f}, 0.025f, kWhite);
  rod({-0.36f, 0.2f, -0.25f}, {-0.36f, 0.67f, -0.25f}, 0.025f, kWhite);
  rod({-0.36f, 0.67f, 0.25f}, {-0.36f, 0.67f, -0.25f}, 0.025f, kWhite);

  // Landing skids.
  for (float side : {-1.f, 1.f}) {
    rod({-0.75f, -0.42f, 0.4f * side}, {0.75f, -0.42f, 0.4f * side}, 0.025f, kCarbon);
    rod({0.4f, -0.32f, 0.28f * side}, {0.4f, -0.42f, 0.4f * side}, 0.02f, kCarbon);
    rod({-0.4f, -0.32f, 0.28f * side}, {-0.4f, -0.42f, 0.4f * side}, 0.02f, kCarbon);
  }

  pilot(ctl, !firstPerson);
  screen(avionics);

  // Arms out to the four stacked motor pairs.
  for (int i = 0; i < 4; ++i) {
    Vector3 r = kRotors[i];
    rod({r.x > 0 ? 0.45f : -0.45f, 0.0f, r.z > 0 ? 0.3f : -0.3f}, {r.x, r.y - 0.04f, r.z}, 0.035f, kCarbon);
    DrawCylinder({r.x, r.y - 0.09f, r.z}, 0.06f, 0.06f, 0.18f, 12, kCarbon);
    DrawCylinderWires({r.x, r.y - 0.09f, r.z}, 0.06f, 0.06f, 0.18f, 12, kEdge);
  }
  for (int i = 0; i < 4; ++i) {
    // Each stack is a counter-rotating pair: the upper prop turns with the gz rotor, the lower one against it.
    Vector3 r = kRotors[i];
    float a = kRotorCcw[i] ? propAngle : -propAngle;
    prop({r.x, r.y + 0.07f, r.z}, a + 30.0f * i, spinning);
    prop({r.x, r.y - 0.07f, r.z}, -a + 30.0f * i + 90.0f, spinning);
  }
  cage(cageTranslucent);  // last: it may be translucent
  rlPopMatrix();
}

}  // namespace astrosim::jetson
