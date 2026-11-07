// raylib viewer: renders the vehicle from gateway State, sends Control from gamepad (preferred) or keyboard.
// usage: viewer [host=127.0.0.1] [port=8765] [--cockpit] [--map] [--hover] [--ghost-cage] [--shot out.png] [--frames N]
// C (gamepad Y / triangle) toggles the cockpit and chase cameras; T (gamepad X / square) toggles a see-through roll cage;
// Space (gamepad A / cross) toggles auto hover; M shows the whole map from above (to check the scenery).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "avionics.hpp"
#include "campus.hpp"
#include "input_map.hpp"
#include "jetson_one.hpp"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "udp_link.hpp"

using namespace astrosim;

// gz is ENU / Z-up; raylib is Y-up. (x, y, z) -> (x, z, -y), a proper rotation, so vectors and quaternion vector parts map alike.
static Vector3 toRay(const float p[3]) { return {p[0], p[2], -p[1]}; }
static Quaternion toRayQ(const float q[4]) { return {q[0], q[2], -q[1], q[3]}; }

// raylib reports every joystick as a gamepad, including keyboards' media-key interfaces (e.g. a Keychron
// "System Control" with one axis); require two sticks' worth of axes.
static int findGamepad() {
  for (int i = 0; i < 4; ++i)
    if (IsGamepadAvailable(i) && GetGamepadAxisCount(i) >= 4) return i;
  return -1;
}

// Map ground, matching the ground box in container/worlds/poc.sdf: 800 m east-west (gz x, raylib x) by
// 1100 m north-south (gz y, raylib -z), centred on the campus (shared/scene/campus.hpp), not the spawn pad.
// Past the edge there is no ground.
constexpr float kMapHalfX = campus::kMapHalfX, kMapHalfZ = campus::kMapHalfY;
constexpr float kMapCx = campus::kMapCx, kMapCz = -campus::kMapCy;  // raylib z = -gz y
static bool onMap(float x, float z) { return std::fabs(x - kMapCx) <= kMapHalfX && std::fabs(z - kMapCz) <= kMapHalfZ; }

// Visual-only scenery for a sense of speed and height; gz has nothing here, so the trees have no collision.
// The trees are generated around the vehicle (`at`) and clipped to the map; they stay put in the world, so they show
// motion. The campus (above) is the main visual reference.
// The campus greybox (shared/scene/campus.hpp), drawn from the same primitives that become gz collision.
// gz (x, y, z) -> raylib (x, z, -y); a gz yaw is the same angle about raylib's y axis.
static void drawCampus() {
  for (const campus::Prim& p : campus::scene()) {
    const Color col = {p.col.r, p.col.g, p.col.b, 255}, edge = {40, 40, 44, 255};
    rlPushMatrix();
    rlTranslatef(p.x, p.z, -p.y);
    rlRotatef(p.yaw * RAD2DEG, 0, 1, 0);
    if (p.pitch != 0) rlRotatef(-p.pitch * RAD2DEG, 0, 0, 1);  // gz y is raylib -z
    switch (p.kind) {
      case campus::Kind::Box:
        DrawCubeV({0, 0, 0}, {p.a, p.c, p.b}, col);
        if (p.edges) DrawCubeWiresV({0, 0, 0}, {p.a, p.c, p.b}, edge);
        break;
      case campus::Kind::Cylinder:
        if (p.horizontal) rlRotatef(90, 0, 0, 1);
        DrawCylinder({0, -p.c / 2, 0}, p.a, p.a, p.c, 12, col);
        break;
      case campus::Kind::Ellipse:
        rlScalef(p.a, 1, p.b);
        DrawCylinder({0, -p.c / 2, 0}, 1, 1, p.c, 64, col);
        if (p.edges) DrawCylinderWires({0, -p.c / 2, 0}, 1, 1, p.c, 64, edge);
        break;
      case campus::Kind::Sphere: DrawSphereEx({0, 0, 0}, p.a, 6, 10, col); break;
    }
    rlPopMatrix();
  }
}

static void drawScenery(Vector3 at) {
  DrawCube({kMapCx, -0.5f, kMapCz}, 2 * kMapHalfX, 1, 2 * kMapHalfZ, {90, 110, 80, 255});
  DrawCubeWires({kMapCx, -0.5f, kMapCz}, 2 * kMapHalfX, 1, 2 * kMapHalfZ, {60, 75, 55, 255});
  // Landing pad at the spawn point.
  DrawCylinder({0, 0.0f, 0}, 3.2f, 3.2f, 0.02f, 48, {70, 72, 78, 255});
  DrawCylinderWires({0, 0.0f, 0}, 2.9f, 2.9f, 0.025f, 48, {240, 200, 40, 255});
  DrawCube({0, 0.025f, -0.6f}, 1.6f, 0.01f, 0.25f, WHITE);  // "H", legs along the spawn heading
  DrawCube({0, 0.025f, 0.6f}, 1.6f, 0.01f, 0.25f, WHITE);
  DrawCube({0, 0.025f, 0}, 0.25f, 0.01f, 1.2f, WHITE);
  drawCampus();
  // Forest: each 20 m cell hashes to at most one tree, so a tree is always at the same world spot.
  constexpr float kCell = 20, kRange = 200;
  const int cx = static_cast<int>(std::floor(at.x / kCell)), cz = static_cast<int>(std::floor(at.z / kCell));
  const int n = static_cast<int>(kRange / kCell);
  for (int i = cx - n; i <= cx + n; ++i) {
    for (int j = cz - n; j <= cz + n; ++j) {
      unsigned seed = static_cast<unsigned>(i) * 73856093u ^ static_cast<unsigned>(j) * 19349663u;
      auto rnd = [&seed] { seed = seed * 1103515245u + 12345u; return ((seed >> 16) & 0x7fff) / 32767.f; };
      if (rnd() > 0.35f) continue;
      Vector3 base = {(i + rnd()) * kCell, 0, (j + rnd()) * kCell};
      if (base.x * base.x + base.z * base.z < 30 * 30) continue;  // clearing around the pad
      if (campus::insideRing(base.x, -base.z, -20)) continue;  // the campus is not woodland
      if (!onMap(base.x, base.z) || !onMap(base.x + 2, base.z + 2) || !onMap(base.x - 2, base.z - 2)) continue;  // canopy stays on the map
      float h = 4 + rnd() * 6;
      unsigned char shade = static_cast<unsigned char>(rnd() * 30);
      DrawCylinder(base, 0.25f, 0.35f, h * 0.4f, 6, {95, 70, 45, 255});
      DrawCylinder({base.x, h * 0.3f, base.z}, 0, h * 0.28f, h * 0.7f, 8,
                   {static_cast<unsigned char>(45 + shade / 2), static_cast<unsigned char>(100 + shade), 50, 255});
    }
  }
}

static Camera3D chaseCamera(Vector3 target, float yaw) {
  Camera3D cam{};
  cam.target = {target.x, target.y + 0.5f, target.z};
  cam.position = {target.x - std::cos(yaw) * 7.5f, target.y + 3.0f, target.z + std::sin(yaw) * 7.5f};
  cam.up = {0, 1, 0};
  cam.fovy = 55;
  cam.projection = CAMERA_PERSPECTIVE;
  return cam;
}

// Pilot's eye, rigidly attached to the airframe: rolls and pitches with it.
static Camera3D cockpitCamera(Vector3 pos, Quaternion q) {
  Camera3D cam{};
  cam.position = Vector3Add(pos, Vector3RotateByQuaternion(jetson::kEye, q));
  cam.target = Vector3Add(cam.position, Vector3RotateByQuaternion({1, -0.12f, 0}, q));  // eyes slightly down
  cam.up = Vector3RotateByQuaternion({0, 1, 0}, q);
  cam.fovy = 75;
  cam.projection = CAMERA_PERSPECTIVE;
  return cam;
}

// Straight down on the whole map, north (gz +y) up the screen.
static Camera3D mapCamera() {
  Camera3D cam{};
  cam.position = {kMapCx, 900, kMapCz};
  cam.target = {kMapCx, 0, kMapCz};
  cam.up = {0, 0, -1};
  cam.fovy = 50;
  cam.projection = CAMERA_PERSPECTIVE;
  return cam;
}

int main(int argc, char** argv) {
  std::string host = "127.0.0.1", shot;
  int port = 8765, maxFrames = 0, positional = 0;
  bool mapView = false, cockpit = false, cageTranslucent = false, startHover = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--cockpit")) cockpit = true;
    else if (!std::strcmp(argv[i], "--map")) mapView = true;
    else if (!std::strcmp(argv[i], "--hover")) startHover = true;
    else if (!std::strcmp(argv[i], "--ghost-cage")) cageTranslucent = true;
    else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
    else if (positional == 0) { host = argv[i]; ++positional; }
    else if (positional == 1) { port = std::atoi(argv[i]); ++positional; }
  }
  if (!shot.empty() && maxFrames == 0) maxFrames = 120;

  UdpLink link(host, static_cast<uint16_t>(port));
  if (!link.ok()) { std::fprintf(stderr, "viewer: bad host/port %s:%d\n", host.c_str(), port); return 1; }

  SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
  InitWindow(1280, 720, "AstroSim PoC viewer");
  SetTargetFPS(60);
  rlSetClipPlanes(0.05, 3000);  // default 0.01..1000 m leaves ~0.5 m depth steps at 300 m: flat decals would shimmer
  RenderTexture2D screen = LoadRenderTexture(avionics::kWidth, avionics::kHeight);
  SetTextureFilter(screen.texture, TEXTURE_FILTER_BILINEAR);

  const std::string linkName = host + ":" + std::to_string(port);
  float camYaw = 0, propAngle = 0;
  int frame = 0;
  ControlMsg ctl;
  InputSource src = findGamepad() >= 0 ? InputSource::Gamepad : InputSource::Keyboard;
  HoverAssist hover;
  hover.on = startHover;
  while (!WindowShouldClose()) {
    StateMsg s = link.latest();
    // ---- input ----
    int pad = findGamepad();
    const char* source = "none";
    if (!IsWindowFocused() && maxFrames == 0) {
      ctl = {};
    } else {
      Keys k;
      k.w = IsKeyDown(KEY_W); k.s = IsKeyDown(KEY_S); k.a = IsKeyDown(KEY_A); k.d = IsKeyDown(KEY_D);
      k.up = IsKeyDown(KEY_UP); k.down = IsKeyDown(KEY_DOWN); k.left = IsKeyDown(KEY_LEFT); k.right = IsKeyDown(KEY_RIGHT);
      ControlMsg padCtl{};
      if (pad >= 0)
        padCtl = mapGamepad(GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X), GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y),
                            GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_X), GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y));
      src = pickSource(src, anyKey(k), anyAxis(padCtl), pad >= 0);
      ctl = src == InputSource::Gamepad ? padCtl : mapKeys(k);
      source = src == InputSource::Gamepad ? "gamepad" : "keyboard";
    }
    if (IsKeyPressed(KEY_C) || (pad >= 0 && IsGamepadButtonPressed(pad, GAMEPAD_BUTTON_RIGHT_FACE_UP))) cockpit = !cockpit;
    if (IsKeyPressed(KEY_M)) mapView = !mapView;
    if (IsKeyPressed(KEY_T) || (pad >= 0 && IsGamepadButtonPressed(pad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT)))
      cageTranslucent = !cageTranslucent;
    if (IsKeyPressed(KEY_SPACE) || (pad >= 0 && IsGamepadButtonPressed(pad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))) hover.toggle();
    if (link.connected()) {  // act only on fresh state
      const float qx = s.quat[0], qy = s.quat[1], qz = s.quat[2], qw = s.quat[3];
      hover.apply(ctl, s.vel, std::atan2(2 * (qw * qz + qx * qy), 1 - 2 * (qy * qy + qz * qz)));
    }
    link.send(ctl);  // always send (zeros included): it is also the keepalive that registers us with the gateway

    // ---- state ----
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
    Quaternion q = toRayQ(s.quat);
    if (engine) propAngle = std::fmod(propAngle + 1500.0f * GetFrameTime(), 360.0f);
    Camera3D cam = mapView ? mapCamera() : cockpit ? cockpitCamera(target, q) : chaseCamera(target, camYaw);

    avionics::Data av;
    av.status = !alive ? "NO GATEWAY" : (!engine ? "ENGINE DOWN" : "CONNECTED");
    av.statusColor = engine ? GREEN : (alive ? ORANGE : RED);
    av.link = linkName.c_str();
    av.input = source;
    av.gamepad = src == InputSource::Gamepad;
    av.alt = s.pos[2] - jetson::kRideHeight;
    av.speed = speed;
    av.vs = s.vel[2];
    av.tilt = tilt;
    av.simTime = s.sim_time;
    av.ctl = ctl;
    av.hover = hover.on;
    av.hoverSettled = engine && hover.settled(s.vel);
    avionics::attitude(s.quat, av.pitch, av.roll, av.heading);
    BeginTextureMode(screen);
    avionics::draw(av);
    EndTextureMode();

    BeginDrawing();
    ClearBackground({135, 175, 215, 255});
    BeginMode3D(cam);
    drawScenery(target);
    if (onMap(target.x, target.z)) {
      // Shadow: the 2.7 x 1.6 m footprint, turned to the vehicle's heading.
      rlPushMatrix();
      rlTranslatef(target.x, 0.45f, target.z);  // above the campus decals (up to 0.4 m)
      rlRotatef(yaw * RAD2DEG, 0, 1, 0);
      DrawCube({0, 0, 0}, 2.7f, 0.01f, 1.6f, {0, 0, 0, 90});
      rlPopMatrix();
      if (!cockpit) DrawLine3D({target.x, 0, target.z}, target, {255, 255, 255, 120});  // altitude line
    }
    jetson::draw(target, q, ctl, cockpit, engine, propAngle, screen.texture, cageTranslucent);
    EndMode3D();

    // The avionics screen is readable in the cockpit; from the chase camera, show the same display as an inset.
    if (!cockpit) {
      const float w = avionics::kWidth * 0.8f, h = avionics::kHeight * 0.8f;
      DrawTexturePro(screen.texture, {0, 0, static_cast<float>(avionics::kWidth), -static_cast<float>(avionics::kHeight)},
                     {10, 10, w, h}, {0, 0}, 0, WHITE);
    }
    // HUD text anchors to the current window size (it can be resized or fullscreened).
    const int sw = GetScreenWidth(), sh = GetScreenHeight();
    if (!engine) {
      const char* msg = alive ? "ENGINE DOWN - holding" : "waiting for gateway...";
      DrawText(msg, (sw - MeasureText(msg, 36)) / 2, sh * 5 / 12, 36, av.statusColor);
    }
    DrawText(src == InputSource::Gamepad ? "L: throttle/yaw   R: pitch/roll   Y: camera   X: cage   A: hover   M: map"
                                         : "WASD: throttle/yaw   Arrows: pitch/roll   C: camera   T: cage   Space: hover   M: map",
             20, sh - 30, 18, WHITE);
    const char* camName = cockpit ? "COCKPIT" : "CHASE";
    DrawText(camName, sw - 20 - MeasureText(camName, 20), 18, 20, WHITE);
    EndDrawing();

    if (maxFrames && ++frame >= maxFrames) {
      if (!shot.empty()) TakeScreenshot(shot.c_str());
      break;
    }
  }
  link.send(ControlMsg{});
  UnloadRenderTexture(screen);
  CloseWindow();
  return 0;
}
