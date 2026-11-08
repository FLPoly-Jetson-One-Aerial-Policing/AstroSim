// OpenXR viewer. The head sits in the Jetson ONE's cockpit; twin sticks from the Touch controllers.
// Renders with raylib's rlgl straight into the OpenXR swapchains, using the same scene as viewer.cpp (scenery.hpp,
// jetson_one.hpp, avionics.hpp).
// usage: viewer_xr [host=127.0.0.1] [port=8765] [--mode full|yaw] [--seat H] [--hover] [--ghost-cage] [--frames N]
//   --record out.mp4 captures the left eye (1280x720, needs ffmpeg). The mirror window shows the same view.
//   keys in the mirror window: 1 = full attitude coupling, 2 = yaw-only coupling (comfort experiment), R = recenter,
//   PgUp/PgDn = seat height, T = cage, Space = auto hover, WASD/arrows fly when no headset sticks are live.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

#include "avionics.hpp"
#include "input_map.hpp"
#include "jetson_one.hpp"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "scenery.hpp"
#include "udp_link.hpp"
#include "xr_session.hpp"

using namespace astrosim;

// Where the user's head sits in the vehicle body frame (raylib axes: +x forward, +y up, +z right) after recentering.
static Vector3 kSeat = astrosim::jetson::kEye;  // --seat H, PgUp/PgDn adjust the height

// Yaw-only version of the vehicle attitude (about raylib +Y, body +x forward).
static Quaternion yawOnly(Quaternion q) {
  Vector3 f = Vector3RotateByQuaternion({1, 0, 0}, q);
  return QuaternionFromAxisAngle({0, 1, 0}, std::atan2(-f.z, f.x));
}

constexpr int kSpecW = 1280, kSpecH = 720;
static float g_propAngle = 0;
static bool g_cageGhost = false;
static ControlMsg g_ctl;
static Texture2D g_screen;
static bool g_engine = false;

// Everything the eyes see: the map, then the aircraft (pilot's head hidden: the user's own head is there).
static void drawWorld(const StateMsg& s, bool engine) {
  Vector3 t = toRay(s.pos);
  drawScenery(t);
  if (onMap(t.x, t.z)) {
    float yaw = std::atan2(2 * (s.quat[3] * s.quat[2] + s.quat[0] * s.quat[1]), 1 - 2 * (s.quat[1] * s.quat[1] + s.quat[2] * s.quat[2]));
    rlPushMatrix();
    rlTranslatef(t.x, 0.45f, t.z);
    rlRotatef(yaw * RAD2DEG, 0, 1, 0);
    DrawCube({0, 0, 0}, 2.7f, 0.01f, 1.6f, {0, 0, 0, 90});
    rlPopMatrix();
  }
  jetson::draw(t, toRayQ(s.quat), g_ctl, true, engine, g_propAngle, g_screen, g_cageGhost);
}

struct EyeTargets {
  std::unordered_map<unsigned, unsigned> fbo;  // swapchain texture -> fbo
  unsigned depth = 0;
  int dw = 0, dh = 0;
};

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  int port = 8765, maxFrames = 0, positional = 0;
  bool yawMode = false, startHover = false;
  std::string record;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) yawMode = !std::strcmp(argv[++i], "yaw");
    else if (!std::strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
    else if (!std::strcmp(argv[i], "--hover")) startHover = true;
    else if (!std::strcmp(argv[i], "--ghost-cage")) g_cageGhost = true;
    else if (!std::strcmp(argv[i], "--seat") && i + 1 < argc) kSeat.y = (float)std::atof(argv[++i]);
    else if (positional == 0) { host = argv[i]; ++positional; }
    else if (positional == 1) { port = std::atoi(argv[i]); ++positional; }
  }

  const std::string hostPort = host + ":" + std::to_string(port);
  UdpLink link(host, static_cast<uint16_t>(port));
  if (!link.ok()) { std::fprintf(stderr, "viewer_xr: bad host/port %s:%d\n", host.c_str(), port); return 1; }

  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  InitWindow(960, 540, "AstroSim XR mirror");
  SetTargetFPS(0);  // never let the mirror window pace the headset

  rlSetClipPlanes(0.05, 3000);
  RenderTexture2D screen = LoadRenderTexture(avionics::kWidth, avionics::kHeight);
  SetTextureFilter(screen.texture, TEXTURE_FILTER_BILINEAR);
  g_screen = screen.texture;
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
  HoverAssist hover;
  hover.on = startHover;

  RenderTexture2D spectator = LoadRenderTexture(kSpecW, kSpecH);
  FILE* ff = nullptr;
  if (!record.empty()) {
    char cmd[1024];
    std::snprintf(cmd, sizeof cmd,
                  "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s %dx%d -use_wallclock_as_timestamps 1 -i - "
                  "-vf vflip,fps=60 -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p '%s'",
                  kSpecW, kSpecH, record.c_str());
    ff = popen(cmd, "w");
    if (!ff) std::fprintf(stderr, "viewer_xr: could not start ffmpeg; not recording\n");
    else std::fprintf(stderr, "recording the left eye to %s (R = recenter, close the window to finish)\n", record.c_str());
  }

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

    const float n = 0.05f, f = 3000.f;
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
    drawWorld(s, g_engine);
    rlDrawRenderBatchActive();
    rlDisableDepthTest();
    rlDisableFramebuffer();

    // Copy the left eye into the spectator view (shown in the mirror window and recorded with --record).
    // Crop the middle 16:9 band of the square-ish eye image; swapchain textures are bottom-up like render textures.
    if (eye == 0) {
      const float cw = (float)ev.w, ch = std::min((float)ev.h, cw * 9 / 16);
      Texture2D eyeTex = {ev.colorTex, ev.w, ev.h, 1, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
      BeginTextureMode(spectator);
      DrawTexturePro(eyeTex, {0, (ev.h - ch) / 2, cw, -ch}, {0, 0, (float)kSpecW, (float)kSpecH}, {0, 0}, 0, WHITE);
      EndTextureMode();
    }

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
    if (IsKeyPressed(KEY_T)) g_cageGhost = !g_cageGhost;
    if (IsKeyPressed(KEY_SPACE)) hover.toggle();
    s = link.latest();
    g_engine = link.connected();
    if (g_engine) {  // act only on fresh state
      const float qx = s.quat[0], qy = s.quat[1], qz = s.quat[2], qw = s.quat[3];
      hover.apply(ctl, s.vel, std::atan2(2 * (qw * qz + qx * qy), 1 - 2 * (qy * qy + qz * qz)));
    }
    link.send(ctl);  // always send: it is also the keepalive that registers us with the gateway
    g_ctl = ctl;
    if (g_engine) g_propAngle = std::fmod(g_propAngle + 1500.0f * GetFrameTime(), 360.0f);

    // The cockpit screen: render the avionics display into its texture before the eyes need it.
    {
      avionics::Data av;
      av.status = g_engine ? "CONNECTED" : (link.gatewayAlive() ? "ENGINE DOWN" : "NO GATEWAY");
      av.statusColor = g_engine ? GREEN : (link.gatewayAlive() ? ORANGE : RED);
      av.link = hostPort.c_str();
      av.input = sticks.valid ? "xr sticks" : "keyboard";
      av.gamepad = sticks.valid;
      av.alt = s.pos[2] - jetson::kRideHeight;
      av.speed = std::sqrt(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1] + s.vel[2] * s.vel[2]);
      av.vs = s.vel[2];
      av.tilt = std::acos(std::fmax(-1.f, std::fmin(1.f, 1 - 2 * (s.quat[0] * s.quat[0] + s.quat[1] * s.quat[1])))) * RAD2DEG;
      av.simTime = s.sim_time;
      av.ctl = ctl;
      av.hover = hover.on;
      av.hoverSettled = g_engine && hover.settled(s.vel);
      avionics::attitude(s.quat, av.pitch, av.roll, av.heading);
      BeginTextureMode(screen);
      avionics::draw(av);
      EndTextureMode();
    }

    if (!xr.frame(renderEye, sticks)) break;
    if (!xr.running()) WaitTime(0.01);
    else ++shown;

    if (ff && shown) {  // only frames the headset actually showed
      Image img = LoadImageFromTexture(spectator.texture);
      std::fwrite(img.data, 1, (size_t)kSpecW * kSpecH * 4, ff);
      UnloadImage(img);
    }

    BeginDrawing();
    ClearBackground(DARKGRAY);
    DrawTexturePro(spectator.texture, {0, 0, (float)kSpecW, -(float)kSpecH}, {0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()}, {0, 0}, 0, WHITE);
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
  if (ff) pclose(ff);  // lets ffmpeg finish the file
  UnloadRenderTexture(spectator);
  UnloadRenderTexture(screen);
  xrp.reset();  // tear OpenXR down while the GL context still exists
  CloseWindow();
  return 0;
}
