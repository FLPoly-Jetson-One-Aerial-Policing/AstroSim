// SPIKE (poc/gzsim-openxr): OpenXR viewer. Head sits on the multicopter; twin sticks from the Touch controllers.
// Renders with raylib's rlgl straight into the OpenXR swapchains, so the scene code matches viewer.cpp.
// usage: viewer_xr [host=127.0.0.1] [port=8765] [--mode full|yaw] [--frames N]
//   keys in the mirror window: 1 = full attitude coupling, 2 = yaw-only coupling (comfort experiment), WASD/arrows fly.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

#include "input_map.hpp"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "udp_link.hpp"
#include "xr_session.hpp"

using namespace astrosim;

// gz is ENU / Z-up; raylib is Y-up. Same mapping as viewer.cpp.
static Vector3 toRay(const float p[3]) { return {p[0], p[2], -p[1]}; }
static Quaternion toRayQ(const float q[4]) { return {q[0], q[2], -q[1], q[3]}; }

// Where the user's seat is, in the vehicle body frame (raylib axes: +x forward, +y up, +z right).
static Vector3 kSeat = {0.0f, 0.8f, 0.0f};  // head height above the vehicle origin after recentering (--seat H, PgUp/PgDn)

static void drawVehicle(const StateMsg& s) {  // duplicated from viewer.cpp on purpose (spike)
  Vector3 p = toRay(s.pos);
  Matrix rot = QuaternionToMatrix(toRayQ(s.quat));
  rlPushMatrix();
  rlTranslatef(p.x, p.y, p.z);
  rlMultMatrixf(MatrixToFloat(rot));
  DrawCube({0, 0, 0}, 0.28f, 0.08f, 0.28f, DARKGRAY);
  rlPushMatrix();
  rlRotatef(45, 0, 1, 0);
  DrawCube({0, 0, 0}, 0.9f, 0.03f, 0.04f, GRAY);
  DrawCube({0, 0, 0}, 0.04f, 0.03f, 0.9f, GRAY);
  const float r = 0.45f;
  const float rx[4] = {r, -r, 0, 0}, rz[4] = {0, 0, r, -r};
  for (int i = 0; i < 4; ++i)
    DrawCylinder({rx[i], 0.04f, rz[i]}, 0.16f, 0.16f, 0.01f, 16, i == 0 || i == 2 ? RED : LIGHTGRAY);
  rlPopMatrix();
  DrawCube({0.2f, 0.05f, 0}, 0.08f, 0.04f, 0.08f, RED);
  rlPopMatrix();
}

static void drawWorld(const StateMsg& s) {
  DrawPlane({0, 0, 0}, {400, 400}, {90, 110, 80, 255});
  DrawGrid(200, 1.0f);
  // Scale/motion references: pylons every 10 m, taller ones every 50 m.
  for (int x = -100; x <= 100; x += 10)
    for (int z = -100; z <= 100; z += 10) {
      if (x == 0 && z == 0) continue;
      bool big = x % 50 == 0 && z % 50 == 0;
      float h = big ? 12.f : 3.f;
      DrawCube({(float)x, h / 2, (float)z}, 0.3f, h, 0.3f, big ? ORANGE : (Color){200, 200, 210, 255});
    }
  Vector3 t = toRay(s.pos);
  DrawCube({t.x, 0.005f, t.z}, 0.6f, 0.01f, 0.6f, {0, 0, 0, 90});
  DrawLine3D({t.x, 0, t.z}, t, {255, 255, 255, 120});
  drawVehicle(s);
}

// Yaw-only version of the vehicle attitude (about raylib +Y, body +x forward).
static Quaternion yawOnly(Quaternion q) {
  Vector3 f = Vector3RotateByQuaternion({1, 0, 0}, q);
  return QuaternionFromAxisAngle({0, 1, 0}, std::atan2(-f.z, f.x));
}

struct EyeTargets {
  std::unordered_map<unsigned, unsigned> fbo;  // swapchain texture -> fbo
  unsigned depth = 0;
  int dw = 0, dh = 0;
};

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  int port = 8765, maxFrames = 0, positional = 0;
  bool yawMode = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) yawMode = !std::strcmp(argv[++i], "yaw");
    else if (!std::strcmp(argv[i], "--seat") && i + 1 < argc) kSeat.y = (float)std::atof(argv[++i]);
    else if (positional == 0) { host = argv[i]; ++positional; }
    else if (positional == 1) { port = std::atoi(argv[i]); ++positional; }
  }

  UdpLink link(host, static_cast<uint16_t>(port));
  if (!link.ok()) { std::fprintf(stderr, "viewer_xr: bad host/port %s:%d\n", host.c_str(), port); return 1; }

  InitWindow(640, 360, "AstroSim XR mirror");
  SetTargetFPS(0);  // never let the mirror window pace the headset

  auto xrp = std::make_unique<XrSession>();
  XrSession& xr = *xrp;
  if (!xr.init()) { std::fprintf(stderr, "viewer_xr: OpenXR init failed (is a runtime active? see XR_RUNTIME_JSON)\n"); CloseWindow(); return 2; }

  EyeTargets tg;
  ControlMsg ctl;
  StateMsg s;
  Vector3 lastEye = {};
  int frame = 0, shown = 0;
  // Recentering: the runtime's LOCAL origin is not where your head is, so capture the head pose and treat it as the seat.
  Vector3 rawEye[2] = {}, origin = {};
  Quaternion rawQ = QuaternionIdentity();
  float originYaw = 0;
  bool autoCentered = false;
  XrSticks sticks;

  auto renderEye = [&](const EyeView& ev, int eye) {
    auto it = tg.fbo.find(ev.colorTex);
    if (it == tg.fbo.end()) {
      if (!tg.depth || tg.dw != ev.w || tg.dh != ev.h) { tg.depth = rlLoadTextureDepth(ev.w, ev.h, true); tg.dw = ev.w; tg.dh = ev.h; }
      unsigned f = rlLoadFramebuffer();
      rlFramebufferAttach(f, ev.colorTex, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
      rlFramebufferAttach(f, tg.depth, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER, 0);
      it = tg.fbo.emplace(ev.colorTex, f).first;
    }

    // Eye pose: XR local space -> vehicle body frame -> world.
    const Quaternion qb = QuaternionFromAxisAngle({0, 1, 0}, -PI / 2);  // XR looks down -z, body forward is +x
    Quaternion qe = {ev.quat[0], ev.quat[1], ev.quat[2], ev.quat[3]};
    Vector3 pe = {ev.pos[0], ev.pos[1], ev.pos[2]};
    rawEye[eye] = pe;
    if (eye == 0) rawQ = qe;
    Quaternion qy = QuaternionFromAxisAngle({0, 1, 0}, -originYaw);
    pe = Vector3RotateByQuaternion(Vector3Subtract(pe, origin), qy);
    qe = QuaternionMultiply(qy, qe);
    Vector3 pBody = Vector3Add(kSeat, Vector3RotateByQuaternion(pe, qb));
    Quaternion qBody = QuaternionMultiply(qb, qe);
    Quaternion qv = toRayQ(s.quat);
    if (yawMode) qv = yawOnly(qv);
    Vector3 pw = Vector3Add(toRay(s.pos), Vector3RotateByQuaternion(pBody, qv));
    Quaternion qw = QuaternionMultiply(qv, qBody);
    Vector3 fwd = Vector3RotateByQuaternion({0, 0, -1}, qw), up = Vector3RotateByQuaternion({0, 1, 0}, qw);
    if (eye == 0) lastEye = pw;

    const float n = 0.05f, f = 600.f;
    Matrix proj = MatrixFrustum(std::tan(ev.fov[0]) * n, std::tan(ev.fov[1]) * n, std::tan(ev.fov[3]) * n, std::tan(ev.fov[2]) * n, n, f);
    Matrix view = MatrixLookAt(pw, Vector3Add(pw, fwd), up);

    rlDrawRenderBatchActive();
    rlEnableFramebuffer(it->second);
    rlViewport(0, 0, ev.w, ev.h);
    rlClearColor(135, 175, 215, 255);
    rlClearScreenBuffers();
    rlEnableDepthTest();
    rlMatrixMode(RL_PROJECTION);
    rlLoadIdentity();
    rlMultMatrixf(MatrixToFloat(proj));
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlMultMatrixf(MatrixToFloat(view));
    drawWorld(s);
    rlDrawRenderBatchActive();
    rlDisableDepthTest();
    rlDisableFramebuffer();

    // restore the 2D state raylib expects for the mirror window
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    rlViewport(0, 0, sw, sh);
    rlMatrixMode(RL_PROJECTION);
    rlLoadIdentity();
    rlOrtho(0, sw, sh, 0, 0.f, 1.f);
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
  };

  while (!WindowShouldClose()) {
    if (IsKeyPressed(KEY_ONE)) yawMode = false;
    if (IsKeyPressed(KEY_TWO)) yawMode = true;
    if (IsKeyPressed(KEY_PAGE_UP)) kSeat.y += 0.05f;
    if (IsKeyPressed(KEY_PAGE_DOWN)) kSeat.y -= 0.05f;
    if (IsKeyPressed(KEY_R) || (!autoCentered && shown >= 60)) {
      autoCentered = true;  // auto-recenter once, ~1 s after frames start
      origin = Vector3Scale(Vector3Add(rawEye[0], rawEye[1]), 0.5f);
      Vector3 f = Vector3RotateByQuaternion({0, 0, -1}, rawQ);
      originYaw = std::atan2(-f.x, -f.z);
      std::fprintf(stderr, "recentered at (%.2f %.2f %.2f) yaw %.0f deg, seat height %.2f\n", origin.x, origin.y, origin.z, originYaw * RAD2DEG, kSeat.y);
    }

    // control: headset sticks if live, else keyboard (XR stick up is +y; mapGamepad wants raylib's up = -y)
    if (sticks.valid) ctl = mapGamepad(sticks.lx, -sticks.ly, sticks.rx, -sticks.ry);
    else {
      Keys k;
      k.w = IsKeyDown(KEY_W); k.s = IsKeyDown(KEY_S); k.a = IsKeyDown(KEY_A); k.d = IsKeyDown(KEY_D);
      k.up = IsKeyDown(KEY_UP); k.down = IsKeyDown(KEY_DOWN); k.left = IsKeyDown(KEY_LEFT); k.right = IsKeyDown(KEY_RIGHT);
      ctl = mapKeys(k);
    }
    link.send(ctl);
    s = link.latest();

    if (!xr.frame(renderEye, sticks)) break;
    if (!xr.running()) WaitTime(0.01);
    else ++shown;

    BeginDrawing();
    ClearBackground(DARKGRAY);
    DrawText(TextFormat("%s  coupling: %s", link.connected() ? "CONNECTED" : "no engine", yawMode ? "yaw-only" : "full"), 10, 10, 20, WHITE);
    DrawText(TextFormat("XR frames %d  sticks %s  seat %.2f m (PgUp/PgDn)  R=recenter", shown, sticks.valid ? "live" : "keyboard", kSeat.y), 10, 40, 20, WHITE);
    DrawText(TextFormat("eye0 world  %.2f %.2f %.2f", lastEye.x, lastEye.y, lastEye.z), 10, 70, 20, WHITE);
    DrawText(TextFormat("thr %+.2f yaw %+.2f pit %+.2f rol %+.2f", ctl.throttle, ctl.yaw, ctl.pitch, ctl.roll), 10, 100, 18, LIGHTGRAY);
    EndDrawing();

    if (shown && shown % 90 == 0 && frame != shown) std::fprintf(stderr, "frame %d eye0 world %.3f %.3f %.3f\n", shown, lastEye.x, lastEye.y, lastEye.z);
    frame = shown;
    if (maxFrames && shown >= maxFrames) break;
  }
  link.send(ControlMsg{});
  xrp.reset();  // tear OpenXR down while the GL context still exists
  CloseWindow();
  return 0;
}
