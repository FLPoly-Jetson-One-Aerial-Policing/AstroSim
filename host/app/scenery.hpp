#pragma once
// World drawing shared by viewer.cpp and viewer_xr.cpp: gz<->raylib frame mapping, the map ground, the campus
// greybox, the landing pad and the forest. Call inside a 3D mode (BeginMode3D, or the XR eye render).
#include <cmath>

#include "campus.hpp"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

namespace astrosim {

// gz is ENU / Z-up; raylib is Y-up. (x, y, z) -> (x, z, -y), a proper rotation, so vectors and quaternion vector parts map alike.
inline Vector3 toRay(const float p[3]) { return {p[0], p[2], -p[1]}; }
inline Quaternion toRayQ(const float q[4]) { return {q[0], q[2], -q[1], q[3]}; }

// Map ground, matching the ground box in container/worlds/poc.sdf: 800 m east-west (gz x, raylib x) by
// 1100 m north-south (gz y, raylib -z), centred on the campus (shared/scene/campus.hpp), not the spawn pad.
// Past the edge there is no ground.
constexpr float kMapHalfX = campus::kMapHalfX, kMapHalfZ = campus::kMapHalfY;
constexpr float kMapCx = campus::kMapCx, kMapCz = -campus::kMapCy;  // raylib z = -gz y
inline bool onMap(float x, float z) { return std::fabs(x - kMapCx) <= kMapHalfX && std::fabs(z - kMapCz) <= kMapHalfZ; }

// Visual-only scenery for a sense of speed and height; gz has nothing here, so the trees have no collision.
// The trees are generated around the vehicle (`at`) and clipped to the map; they stay put in the world, so they show
// motion. The campus (above) is the main visual reference.
// The campus greybox (shared/scene/campus.hpp), drawn from the same primitives that become gz collision.
// gz (x, y, z) -> raylib (x, z, -y); a gz yaw is the same angle about raylib's y axis.
inline void drawCampus() {
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

inline void drawScenery(Vector3 at) {
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

}  // namespace astrosim
