// raylib viewer: renders the vehicle from gateway State, sends Control from gamepad (preferred) or keyboard.
// usage: viewer [host=127.0.0.1] [port=8765] [--shot out.png] [--frames N]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "input_map.hpp"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "udp_link.hpp"

using namespace astrosim;

// gz is ENU / Z-up; raylib is Y-up. (x, y, z) -> (x, z, -y), a proper rotation, so vectors and quaternion vector parts map alike.
static Vector3 toRay(const float p[3]) { return {p[0], p[2], -p[1]}; }
static Quaternion toRayQ(const float q[4]) { return {q[0], q[2], -q[1], q[3]}; }

static int findGamepad() {
  for (int i = 0; i < 4; ++i) if (IsGamepadAvailable(i)) return i;
  return -1;
}

static void drawVehicle(const StateMsg& s) {
  Vector3 p = toRay(s.pos);
  Matrix rot = QuaternionToMatrix(toRayQ(s.quat));
  rlPushMatrix();
  rlTranslatef(p.x, p.y, p.z);
  rlMultMatrixf(MatrixToFloat(rot));
  // body frame in raylib axes: +x forward, +y up, -z left
  DrawCube({0, 0, 0}, 0.28f, 0.08f, 0.28f, DARKGRAY);
  rlPushMatrix();
  rlRotatef(45, 0, 1, 0);
  DrawCube({0, 0, 0}, 0.9f, 0.03f, 0.04f, GRAY);
  DrawCube({0, 0, 0}, 0.04f, 0.03f, 0.9f, GRAY);
  const float r = 0.45f;
  // arms lie along the rotated local x / z axes; local +x and +z point to the front pair
  const float rx[4] = {r, -r, 0, 0}, rz[4] = {0, 0, r, -r};
  for (int i = 0; i < 4; ++i)
    DrawCylinder({rx[i], 0.04f, rz[i]}, 0.16f, 0.16f, 0.01f, 16, i == 0 || i == 2 ? RED : LIGHTGRAY);
  rlPopMatrix();
  DrawCube({0.2f, 0.05f, 0}, 0.08f, 0.04f, 0.08f, RED);  // nose marker
  rlPopMatrix();
}

int main(int argc, char** argv) {
  std::string host = "127.0.0.1", shot;
  int port = 8765, maxFrames = 0, positional = 0;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
    else if (positional == 0) { host = argv[i]; ++positional; }
    else if (positional == 1) { port = std::atoi(argv[i]); ++positional; }
  }
  if (!shot.empty() && maxFrames == 0) maxFrames = 120;

  UdpLink link(host, static_cast<uint16_t>(port));
  if (!link.ok()) { std::fprintf(stderr, "viewer: bad host/port %s:%d\n", host.c_str(), port); return 1; }

  SetConfigFlags(FLAG_MSAA_4X_HINT);
  InitWindow(1280, 720, "AstroSim PoC viewer");
  SetTargetFPS(60);

  float camYaw = 0;
  int frame = 0;
  ControlMsg ctl;
  while (!WindowShouldClose()) {
    // ---- input ----
    int pad = findGamepad();
    const char* source = "none";
    if (!IsWindowFocused() && maxFrames == 0) {
      ctl = {};
    } else if (pad >= 0) {
      ctl = mapGamepad(GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X), GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y),
                       GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_X), GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y));
      source = "gamepad";
    } else {
      Keys k;
      k.w = IsKeyDown(KEY_W); k.s = IsKeyDown(KEY_S); k.a = IsKeyDown(KEY_A); k.d = IsKeyDown(KEY_D);
      k.up = IsKeyDown(KEY_UP); k.down = IsKeyDown(KEY_DOWN); k.left = IsKeyDown(KEY_LEFT); k.right = IsKeyDown(KEY_RIGHT);
      ctl = mapKeys(k);
      source = "keyboard";
    }
    link.send(ctl);  // always send (zeros included): it is also the keepalive that registers us with the gateway

    // ---- state ----
    StateMsg s = link.latest();
    bool alive = link.gatewayAlive(), engine = link.connected();
    const float x = s.quat[0], y = s.quat[1], z = s.quat[2], w = s.quat[3];
    float yaw = std::atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));
    float diff = std::remainder(yaw - camYaw, 2 * PI);
    camYaw += diff * 0.05f;
    float upz = 1 - 2 * (x * x + y * y);  // body-up . world-up
    float tilt = std::acos(std::fmax(-1.f, std::fmin(1.f, upz))) * RAD2DEG;
    float speed = std::sqrt(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1] + s.vel[2] * s.vel[2]);

    // ---- draw ----
    Vector3 target = toRay(s.pos);
    Camera3D cam{};
    cam.target = {target.x, target.y + 0.3f, target.z};
    cam.position = {target.x - std::cos(camYaw) * 4.5f, target.y + 2.0f, target.z + std::sin(camYaw) * 4.5f};
    cam.up = {0, 1, 0};
    cam.fovy = 55;
    cam.projection = CAMERA_PERSPECTIVE;

    BeginDrawing();
    ClearBackground({135, 175, 215, 255});
    BeginMode3D(cam);
    DrawPlane({0, 0, 0}, {200, 200}, {90, 110, 80, 255});
    DrawGrid(100, 1.0f);
    DrawCube({target.x, 0.005f, target.z}, 0.6f, 0.01f, 0.6f, {0, 0, 0, 90});  // shadow blob
    DrawLine3D({target.x, 0, target.z}, target, {255, 255, 255, 120});          // altitude line
    drawVehicle(s);
    EndMode3D();

    const char* status = !alive ? "NO GATEWAY" : (!engine ? "ENGINE DOWN" : "CONNECTED");
    Color sc = engine ? GREEN : (alive ? ORANGE : RED);
    DrawRectangle(10, 10, 330, 190, {0, 0, 0, 140});
    DrawText(TextFormat("%s  %s:%d", status, host.c_str(), port), 20, 18, 20, sc);
    DrawText(TextFormat("alt   %6.2f m", s.pos[2]), 20, 48, 20, WHITE);
    DrawText(TextFormat("speed %6.2f m/s", speed), 20, 72, 20, WHITE);
    DrawText(TextFormat("tilt  %6.1f deg", tilt), 20, 96, 20, WHITE);
    DrawText(TextFormat("sim t %6.1f s", s.sim_time), 20, 120, 20, WHITE);
    DrawText(TextFormat("input: %s", source), 20, 148, 20, pad >= 0 ? SKYBLUE : YELLOW);
    DrawText(TextFormat("thr %+.2f yaw %+.2f", ctl.throttle, ctl.yaw), 20, 172, 16, LIGHTGRAY);
    DrawText(TextFormat("pit %+.2f rol %+.2f", ctl.pitch, ctl.roll), 190, 172, 16, LIGHTGRAY);
    if (!engine) DrawText(alive ? "ENGINE DOWN - holding" : "waiting for gateway...", 400, 230, 36, sc);
    DrawText(pad >= 0 ? "L: throttle/yaw   R: pitch/roll" : "WASD: throttle/yaw   Arrows: pitch/roll", 20, 690, 18, WHITE);
    EndDrawing();

    if (maxFrames && ++frame >= maxFrames) {
      if (!shot.empty()) TakeScreenshot(shot.c_str());
      break;
    }
  }
  link.send(ControlMsg{});
  CloseWindow();
  return 0;
}
