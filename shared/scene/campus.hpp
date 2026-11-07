#pragma once
// Greybox of the Florida Polytechnic campus, as primitives in gz world coordinates (ENU, metres, z up, spawn pad
// at the origin). One description, two consumers: host/app/viewer.cpp draws it, and host/tools/gen_campus.cpp
// turns the solid parts into container/models/campus/model.sdf (collision only; gz runs headless).
//
// Sizes are the user's brief: published figures where there are any, otherwise estimates (see the notes beside
// each landmark). Positions and orientations are traced from the annotated aerial photo (north up): pixel
// coordinates below, converted with px(). Scale from the IST: ~126 px for its ~122 m long axis.
#include <cmath>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace astrosim::campus {

constexpr float kFt = 0.3048f;
constexpr float kPi = 3.14159265358979f;

constexpr float kMpp = 0.97f;                // metres per image pixel
constexpr float kPadPx[2] = {417, 438};      // the landing pad, in image pixels
constexpr float kLakeYaw = -46 * kPi / 180;  // the lake and the IST long axis run NW-SE
// Map extent (the ground box): 800 m east-west by 1100 m north-south, centred on the campus, not the pad.
constexpr float kMapCx = 130, kMapCy = 50, kMapHalfX = 400, kMapHalfY = 550;

struct Rgb { unsigned char r, g, b; };
enum class Kind { Box, Cylinder, Ellipse, Sphere };

// (x, y, z) is the centre. a, b, c by kind:
//   Box: full sizes along local x, y, z.   Cylinder: a = radius, c = length (axis z, or local x if horizontal).
//   Ellipse (extruded): a, b = semi-axes along local x, y; c = height.   Sphere: a = radius.
// yaw: radians about z (counter-clockwise from above). Collision exists only if `collide`.
struct Prim {
  Kind kind;
  float x, y, z, a, b, c, yaw;
  bool horizontal, collide, edges;
  Rgb col;
  const char* tag;
  float pitch = 0;  // radians about local y, applied after yaw (visual-only prims, e.g. the flags' saltire)
};

struct P2 { float x, y; };

// Image pixel (north up) to world metres, spawn pad at the origin.
inline P2 px(float x, float y) { return {(x - kPadPx[0]) * kMpp, -(y - kPadPx[1]) * kMpp}; }
// World yaw of a direction given in image pixels (dy grows southward).
inline float pxYaw(float dx, float dy) { return std::atan2(-dy, dx); }
constexpr float rad(float deg) { return deg * kPi / 180; }

// Draw heights of flat decals (m above the ground top). Ground-level z-fighting at distance forces >= 0.1 m steps.
constexpr float kRoadZ = 0.1f, kPathZ = 0.2f, kPlazaZ = 0.3f, kWaterZ = 0.4f, kArcadeZ = 0.5f;

// Ring road centreline, traced from the photo (image pixels, clockwise from the IST's NW tip).
constexpr float kRingPx[][2] = {
    {293, 153}, {333, 113}, {393, 90},  {460, 83},  {520, 93},  {580, 120}, {633, 157}, {687, 200}, {740, 267}, {773, 320},
    {797, 387}, {807, 440}, {813, 507}, {823, 573}, {830, 640}, {830, 683}, {793, 680}, {727, 667}, {633, 650}, {553, 627},
    {487, 593}, {420, 547}, {367, 493}, {327, 440}, {293, 373}, {277, 320}, {267, 260}, {270, 213}};
// The west-to-roundabout approach road along the south edge.
constexpr float kApproachPx[][2] = {{150, 600}, {220, 627}, {387, 643}, {507, 667}, {620, 673}, {793, 681}, {830, 683}};
constexpr float kRoadW = 10;
// Storeys, per the user: residence halls 5, IST and BARC 2, everything else 1.
constexpr float kOneStory = 5.f, kFiveStory = 17.f;

// Catmull-Rom through the points, `sub` samples per span.
inline std::vector<P2> smooth(const std::vector<P2>& p, bool closed, int sub) {
  std::vector<P2> o;
  const int n = static_cast<int>(p.size());
  const int spans = closed ? n : n - 1;
  auto at = [&](int i) { return closed ? p[(i % n + n) % n] : p[i < 0 ? 0 : (i >= n ? n - 1 : i)]; };
  for (int i = 0; i < spans; ++i)
    for (int k = 0; k < sub; ++k) {
      const float t = static_cast<float>(k) / sub, t2 = t * t, t3 = t2 * t;
      const P2 a = at(i - 1), b = at(i), c = at(i + 1), d = at(i + 2);
      auto f = [&](float pa, float pb, float pc, float pd) {
        return 0.5f * (2 * pb + (-pa + pc) * t + (2 * pa - 5 * pb + 4 * pc - pd) * t2 + (-pa + 3 * pb - 3 * pc + pd) * t3);
      };
      o.push_back({f(a.x, b.x, c.x, d.x), f(a.y, b.y, c.y, d.y)});
    }
  if (!closed) o.push_back(p.back());
  return o;
}
inline const std::vector<P2>& ringLine() {
  static const std::vector<P2> r = [] {
    std::vector<P2> pts;
    for (const auto& q : kRingPx) pts.push_back(px(q[0], q[1]));
    return smooth(pts, true, 6);
  }();
  return r;
}
inline P2 ringCentre() {
  static const P2 c = [] {
    P2 s = {0, 0};
    for (const auto& q : ringLine()) { s.x += q.x; s.y += q.y; }
    return P2{s.x / ringLine().size(), s.y / ringLine().size()};
  }();
  return c;
}
// Is (x, y) inside the ring road, shrunk by `margin` m (negative: grown) about the campus centre?
inline bool insideRing(float x, float y, float margin = 0) {
  const P2 c = ringCentre();
  const float s = 1 - margin / 290.f;
  const float qx = c.x + (x - c.x) / s, qy = c.y + (y - c.y) / s;
  bool in = false;
  const auto& r = ringLine();
  for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
    if ((r[i].y > qy) != (r[j].y > qy) && qx < (r[j].x - r[i].x) * (qy - r[i].y) / (r[j].y - r[i].y) + r[i].x) in = !in;
  return in;
}

class Builder {
 public:
  std::vector<Prim> out;

  void box(const char* tag, P2 p, float z0, float sx, float sy, float h, float yaw, Rgb col, bool collide = true,
           bool edges = true) {
    out.push_back({Kind::Box, p.x, p.y, z0 + h / 2, sx, sy, h, yaw, false, collide, edges, col, tag});
  }
  void cyl(const char* tag, P2 p, float z0, float r, float h, Rgb col, bool collide = true) {
    out.push_back({Kind::Cylinder, p.x, p.y, z0 + h / 2, r, 0, h, 0, false, collide, false, col, tag});
  }
  // Cylinder lying along local x at centre height z.
  void barrel(const char* tag, P2 p, float z, float r, float len, float yaw, Rgb col) {
    out.push_back({Kind::Cylinder, p.x, p.y, z, r, 0, len, yaw, true, true, false, col, tag});
  }
  void ell(const char* tag, P2 p, float z0, float a, float b, float h, float yaw, Rgb col, bool collide = true,
           bool edges = true) {
    out.push_back({Kind::Ellipse, p.x, p.y, z0 + h / 2, a, b, h, yaw, false, collide, edges, col, tag});
  }
  void sphere(const char* tag, P2 p, float z, float r, Rgb col) {
    out.push_back({Kind::Sphere, p.x, p.y, z, r, 0, 0, 0, false, false, false, col, tag});
  }
  // Flat decals: thin slabs whose top is at z.
  void flatBox(const char* tag, P2 p, float z, float sx, float sy, float yaw, Rgb col) {
    box(tag, p, 0, sx, sy, z, yaw, col, false, false);
  }
  void flatEll(const char* tag, P2 p, float z, float a, float b, float yaw, Rgb col) {
    ell(tag, p, 0, a, b, z, yaw, col, false, false);
  }
  // A straight path through `from` along unit direction d, kept to the parts inside the ring road.
  void path(const char* tag, P2 from, P2 d, float t0, float t1, float width, float z, Rgb col) {
    const float yaw = std::atan2(d.y, d.x);
    float start = 0;
    bool in = false;
    for (float t = t0; t <= t1 + 1e-3f; t += 2) {
      const bool now = t < t1 && insideRing(from.x + d.x * t, from.y + d.y * t, kRoadW);
      if (now && !in) { start = t; in = true; }
      if (!now && in) {
        in = false;
        const float mid = (start + t) / 2;
        flatBox(tag, {from.x + d.x * mid, from.y + d.y * mid}, z, t - start, width, yaw, col);
      }
    }
  }
};

constexpr Rgb kConcrete = {205, 205, 200}, kGlass = {170, 205, 225}, kWhite = {240, 240, 238}, kWater = {85, 140, 190},
              kAsphalt = {62, 62, 66}, kPathCol = {172, 170, 162}, kPlaza = {196, 182, 154},
              kTrunk = {110, 85, 55}, kPalmTop = {60, 120, 55}, kOakTop = {70, 125, 60}, kBridge = {188, 188, 184};

constexpr float kIstArm = 62 * kFt, kIstHalfLen = 125 * kFt, kIstHinge = 68 * kFt;
constexpr int kArmsPerBank = 47;
// Arm length at distance x from the skylight centre: 62 ft tapered to a point at the ends (the wing silhouette).
inline float armLength(float x) {
  const float r = x / kIstHalfLen;
  return kIstArm * std::sqrt(std::fmax(0.f, 1 - r * r));
}

inline void addIst(Builder& s) {
  // 162,000 sq ft over two floors, elliptical in plan: ~400 x 255 ft footprint (estimate), ~45 ft to the roof deck.
  // Long axis NW-SE, parallel to the lake, which wraps round it as a moat.
  const P2 c = px(377, 192);
  const float yaw = kLakeYaw;
  const float a = 200 * kFt, b = 127.5f * kFt, deck = 45 * kFt;
  s.flatEll("ist_moat", c, kWaterZ, 85, 68, yaw, kWater);
  s.flatEll("ist_arcade", c, kArcadeZ, a + 10, b + 10, yaw, kPlaza);  // shaded arcade ring
  s.ell("ist_base", c, 0, a, b, deck, yaw, kConcrete);
  // Pergola: thin ribs on an offset ellipse, ground to roof.
  for (int i = 0; i < 72; ++i) {
    const float t = 2 * kPi * i / 72, lx = (a + 3) * std::cos(t), ly = (b + 3) * std::sin(t);
    s.cyl("ist_pergola", {c.x + lx * std::cos(yaw) - ly * std::sin(yaw), c.y + lx * std::sin(yaw) + ly * std::cos(yaw)}, 0,
          0.12f, deck + 0.5f, kWhite);
  }
  // Skylight: 250 ft barrel vault, ~60 ft wide, crown at 68 ft (estimates), along the building's long axis.
  const float r = 30 * kFt, springZ = kIstHinge - r;
  s.barrel("ist_skylight", c, springZ, r, 2 * kIstHalfLen, yaw, kGlass);
  // 94 louvers: two mirrored banks of 47, hinged either side of the crown, upright (tips ~130 ft).
  const float cy = std::cos(yaw), sy = std::sin(yaw), hinge = 3.0f;
  for (int bank = 0; bank < 2; ++bank) {
    const float side = bank ? hinge : -hinge, surfaceZ = springZ + std::sqrt(r * r - hinge * hinge);
    for (int i = 0; i < kArmsPerBank; ++i) {
      const float x = -kIstHalfLen + 2 * kIstHalfLen * i / (kArmsPerBank - 1);
      const float len = std::fmax(armLength(x), 0.4f);
      const float top = kIstHinge + len, h = top - surfaceZ;
      s.box("louver", {c.x + x * cy - side * sy, c.y + x * sy + side * cy}, surfaceZ, 0.9f, 0.15f, h, yaw, kWhite, false, false);
    }
  }
}

// BARC (40 ft) and the IST deck (45 ft) are the two-storey buildings.
inline void addCampusBuildings(Builder& s) {
  const Rgb kGrey = {196, 196, 190};
  // BARC: ~90,000 sq ft, two stories; 300 x 150 x 40 ft (estimate). Long axis NE-SW, west of the lake.
  s.box("barc", px(330, 308), 0, 300 * kFt, 150 * kFt, 40 * kFt, rad(44), kConcrete);
  // Wendt Engineering Center: 40,000 sq ft; 200 x 100 ft footprint (estimate), tilt-wall panels. One story per the user's correction.
  s.box("wendt", px(355, 345), 0, 200 * kFt, 100 * kFt, kOneStory, rad(-40), kGrey);
  // Two small buildings by the pad: temporary IT / auxiliary buildings (user). Sizes are guesses.
  s.box("temp_it_a", px(407, 396), 0, 14, 10, kOneStory, rad(45), kGrey);
  s.box("temp_it_b", px(460, 470), 0, 40, 30, kOneStory, rad(-46), kGrey);
  // Police station (the white block in the upper circle) and Admissions (lower circle), both across the ring road to the
  // east, one story; positions from the user's blown-up photo, sizes from it too (no published figures).
  s.box("police", px(713, 182), 0, 25, 20, kOneStory, rad(-30), kGrey);
  s.box("admissions", px(745, 233), 0, 22, 12, kOneStory, rad(75), kGrey);
  // Residence halls, five floors each (user). Placed along the centrelines the user drew on the photo, widths from the photo.
  // Phase I (north) and Phase III (west) are straight bars with a sidewalk's gap between them; Phase II (east) is
  // L-shaped: a bar, then a leg turning south-west at its south-east end.
  // Phase III's published 96,000 sq ft over five floors is ~1,780 m2 per floor; the 113 x 15 m bar here is ~1,700 m2.
  s.box("res1", px(550, 186), 0, 70, 15, kFiveStory, rad(-24), kConcrete);
  s.box("res3", px(565, 237), 0, 113, 15, kFiveStory, rad(-45.7f), kConcrete);
  s.box("res2", px(615, 222), 0, 58, 17, kFiveStory, rad(-39.6f), kConcrete);
  s.box("res2_leg", px(624, 251.5f), 0, 38, 15, kFiveStory, rad(-127.7f), kConcrete);
  // Wellness Center: size unpublished, so the photo's footprint, one story.
  s.box("wellness", px(671, 297), 0, 55, 42, kOneStory + 1, rad(-38), kConcrete);
  // Student Development Center (contains the gym): 8,600 sq ft, one story, phoenix shape: a central box plus two swept
  // boxes (the brief's wedge prisms), an 8-lane 25 yd pool and a sports field.
  const P2 ph = px(756, 405);
  s.box("sdc_core", ph, 0, 14, 7, kOneStory, rad(44), kConcrete);
  for (int w = -1; w <= 1; w += 2) {
    const float yaw = rad(44) + (w > 0 ? 0.5f : -0.5f);
    s.box("sdc_wing", {ph.x + 8.f * w * std::cos(rad(44)), ph.y + 8.f * w * std::sin(rad(44))}, 0, 17, 6, kOneStory - 1, yaw, kGrey);
  }
  s.flatBox("pool", px(758, 431), kWaterZ, 25 * 0.9144f, 18, rad(-30), kWater);
  s.flatBox("field", px(748, 466), kPlazaZ, 80, 62, rad(-22), {110, 150, 85});
  // Oak Grove (opened 2019) between the Wellness Center and the SDC: a plaza with live oaks (spheres on cylinders).
  const P2 grove = px(715, 350);
  s.flatBox("oak_grove", grove, kPlazaZ, 50, 45, 0, kPlaza);
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      const P2 p = {grove.x + (i - 1.5f) * 11 + 1.5f * std::sin(i * 12.9f + j * 78.2f),
                    grove.y + (j - 1.5f) * 10 + 1.5f * std::sin(i * 39.3f + j * 11.1f)};
      s.cyl("oak_trunk", p, 0, 0.45f, 4.5f, kTrunk);
      s.sphere("oak_canopy", p, 8, 5.5f, kOakTop);
    }
}

// Three large flagpoles at the south-east end of the lake, in a row perpendicular to the lake axis, flags facing the lake:
// Florida, American (centre), Florida Poly, left to right facing up the lake. By the US flag code the US flag flies highest
// when flown with others, so its pole is 65 ft against 60 ft; all flags are 12 x 8 ft and stream along the row. Flags are
// greybox stand-ins: stripes and canton for the US flag, a red saltire and a plain gold disc for the seal on Florida, and a flat #501D83 purple
// field for Florida Poly (no logo).
inline void addFlagpoles(Builder& s) {
  const float fw = 12 * kFt, fh = 8 * kFt, spacing = 10.5f;
  // The US pole sits on the lake's centre line, just beyond its south-east end; the others follow along the row.
  const P2 lake = px(565, 407), U = {std::cos(kLakeYaw), std::sin(kLakeYaw)};
  const float endU = 232 * kMpp + 7;  // lake semi-length plus a margin
  const P2 mid = {lake.x + U.x * endU, lake.y + U.y * endU};
  const P2 V = {-std::sin(kLakeYaw), std::cos(kLakeYaw)};  // perpendicular to the lake axis
  const float yaw = kLakeYaw + kPi / 2;                     // flag plane contains the row; local y is the lake axis
  for (int k = -1; k <= 1; ++k) {
    const P2 p = {mid.x + V.x * spacing * k, mid.y + V.y * spacing * k};
    const float poleH = (k == 0 ? 65 : 60) * kFt;
    s.cyl("flagpole", p, 0, 0.15f, poleH, {220, 220, 215});
    s.sphere("flagpole_finial", p, poleH + 0.2f, 0.3f, {212, 175, 55});
    const float top = poleH - 0.4f;
    // A flag layer: `t` is its centre's distance from the pole along the row, towards -V so the hoist is on the left seen from the lake, w x h its size.
    auto layer = [&](float t, float zTop, float w, float h, float thick, Rgb c) {
      s.box("flag", {p.x - V.x * t, p.y - V.y * t}, zTop - h, w, thick, h, yaw, c, false, false);
    };
    const float centre = 0.15f + fw / 2;
    if (k == 0) {  // American: 13 stripes, blue canton over the top 7
      const float sh = fh / 13;
      const float cantonW = fw * 0.4f, flyW = fw - cantonW;  // stripes stop at the canton, so nothing overlaps (no z-fighting)
      for (int i = 0; i < 13; ++i) {
        const Rgb c = i % 2 ? Rgb{240, 240, 240} : Rgb{178, 34, 52};
        if (i < 7) layer(0.15f + cantonW + flyW / 2, top - i * sh, flyW, sh, 0.04f, c);
        else layer(centre, top - i * sh, fw, sh, 0.04f, c);
      }
      layer(0.15f + cantonW / 2, top, cantonW, sh * 7, 0.04f, {60, 59, 110});
    } else if (k == -1) {  // Florida: white field, red saltire
      layer(centre, top, fw, fh, 0.04f, {245, 245, 245});
      const float len = std::hypot(fw, fh), ang = std::atan2(fh, fw);
      for (int d = -1; d <= 1; d += 2) {
        s.box("flag_saltire", {p.x - V.x * centre, p.y - V.y * centre}, top - fh / 2 - 0.15f, len, 0.1f, 0.3f, yaw,
              {200, 30, 40}, false, false);
        s.out.back().pitch = d * ang;
      }
      // The state seal, as a flat disc standing on the flag's face (a horizontal cylinder along the lake axis).
      s.barrel("flag_seal", {p.x - V.x * centre, p.y - V.y * centre}, top - fh / 2, fh * 0.22f, 0.16f, kLakeYaw, {230, 180, 40});
      s.out.back().collide = false;
    } else {  // Florida Poly: flat purple
      layer(centre, top, fw, fh, 0.04f, {0x50, 0x1D, 0x83});
    }
  }
}

inline void addSite(Builder& s) {
  // Main lake, NW-SE (placeholder-sized, here fitted to the photo: ~470 x 90 m).
  s.flatEll("lake", px(565, 407), kWaterZ, 232 * kMpp, 44 * kMpp, kLakeYaw, kWater);
  // Ring road (smoothed trace) with tall palms along its inner edge, every ~25 m.
  const auto& ring = ringLine();
  const P2 ctr = ringCentre();
  float carry = 0;
  for (size_t i = 0; i < ring.size(); ++i) {
    const P2 a = ring[i], b = ring[(i + 1) % ring.size()];
    const float len = std::hypot(b.x - a.x, b.y - a.y), yaw = std::atan2(b.y - a.y, b.x - a.x);
    s.flatBox("ring_road", {(a.x + b.x) / 2, (a.y + b.y) / 2}, kRoadZ, len * 1.2f, kRoadW, yaw, kAsphalt);
    for (carry += len; carry >= 25; carry -= 25) {
      float nx = -(b.y - a.y) / len, ny = (b.x - a.x) / len;  // inward-pointing normal
      if (nx * (ctr.x - a.x) + ny * (ctr.y - a.y) < 0) { nx = -nx; ny = -ny; }
      const P2 p = {a.x + nx * (kRoadW + 2), a.y + ny * (kRoadW + 2)};
      s.cyl("palm_trunk", p, 0, 0.3f, 14, kTrunk);
      s.sphere("palm_crown", p, 14.5f, 3, kPalmTop);
    }
  }
  // Approach road from the west into the south-east roundabout.
  std::vector<P2> app;
  for (const auto& q : kApproachPx) app.push_back(px(q[0], q[1]));
  app = smooth(app, false, 6);
  for (size_t i = 0; i + 1 < app.size(); ++i) {
    const P2 a = app[i], b = app[i + 1];
    s.flatBox("approach", {(a.x + b.x) / 2, (a.y + b.y) / 2}, kRoadZ, std::hypot(b.x - a.x, b.y - a.y) * 1.2f, kRoadW,
              std::atan2(b.y - a.y, b.x - a.x), kAsphalt);
  }
  s.flatEll("roundabout", px(830, 683), kRoadZ, 22, 22, 0, kAsphalt);
  s.flatEll("roundabout_island", px(830, 683), kPathZ, 9, 9, 0, {95, 125, 80});
  // Bridges: low solid decks 1-3 ft above the water surface (collision, so they can be landed on). The large road-width one curves round
  // the IST's south-east end from the east bank to the south-west bank (the user's red line in the IST close-up);
  // four footbridges join the sidewalks either side of the lake. All traced in image pixels.
  auto bridge = [&](const char* tag, std::initializer_list<std::array<float, 2>> pts, float width, float deck) {
    std::vector<P2> line;
    for (const auto& q : pts) line.push_back(px(q[0], q[1]));
    line = smooth(line, false, 4);
    for (size_t i = 0; i + 1 < line.size(); ++i) {
      const P2 a = line[i], b = line[i + 1];
      s.box(tag, {(a.x + b.x) / 2, (a.y + b.y) / 2}, 0, std::hypot(b.x - a.x, b.y - a.y) * 1.2f, width, deck,
            std::atan2(b.y - a.y, b.x - a.x), kBridge, true, false);
    }
  };
  bridge("bridge_main", {{477, 216}, {452, 219}, {429, 228}, {413.5f, 249}, {406.5f, 262}, {400, 287}}, 12, kWaterZ + 3 * kFt);
  bridge("footbridge", {{529, 278}, {511, 303}, {493, 321}, {456.5f, 346.5f}}, 3.5f, kWaterZ + 1.5f * kFt);
  bridge("footbridge", {{572, 337}, {540, 367}, {507, 397}}, 3.5f, kWaterZ + 1.5f * kFt);
  bridge("footbridge", {{627, 400}, {595, 431}, {565, 462}}, 3.5f, kWaterZ + 1.5f * kFt);
  bridge("footbridge", {{685, 465}, {650, 495}, {620, 525}}, 3.5f, kWaterZ + 1.5f * kFt);
  // I-4 along the north-west side (from the photo's upper left): two carriageways with a grass median, running
  // west-south-west to east-north-east, clipped to the map.
  {
    const P2 a = px(0, 110), b = px(350, -5);
    const float len = std::hypot(b.x - a.x, b.y - a.y), yaw = std::atan2(b.y - a.y, b.x - a.x);
    const P2 d = {(b.x - a.x) / len, (b.y - a.y) / len}, n = {-d.y, d.x};
    float t0 = 0, t1 = -1;
    bool seen = false;
    for (float u = -2000; u <= 2000; u += 4)
      if (std::fabs(a.x + d.x * u - kMapCx) <= kMapHalfX - 8 && std::fabs(a.y + d.y * u - kMapCy) <= kMapHalfY - 8) {
        if (!seen) t0 = u;
        t1 = u;
        seen = true;
      }
    if (seen)
      for (int side = -1; side <= 1; side += 2) {
        const float mid = (t0 + t1) / 2, off = side * 15.f;  // carriageway centres 30 m apart
        s.flatBox("highway", {a.x + d.x * mid + n.x * off, a.y + d.y * mid + n.y * off}, kRoadZ, t1 - t0, 16, yaw, {52, 52, 56});
      }
  }
  // Pedestrian grid parallel and perpendicular to the lake axis, 90 m pitch, through the lake centre.
  const P2 L = px(565, 407);
  const P2 U = {std::cos(kLakeYaw), std::sin(kLakeYaw)}, V = {-U.y, U.x};
  for (int k = -6; k <= 6; ++k) {
    s.path("path", {L.x + V.x * 90 * k, L.y + V.y * 90 * k}, U, -700, 700, 3, kPathZ, kPathCol);
    s.path("path", {L.x + U.x * 90 * k, L.y + U.y * 90 * k}, V, -700, 700, 3, kPathZ, kPathCol);
  }
}

inline std::vector<Prim> build() {
  Builder s;
  addSite(s);
  addFlagpoles(s);
  addIst(s);
  addCampusBuildings(s);
  return s.out;
}
inline const std::vector<Prim>& scene() {
  static const std::vector<Prim> s = build();
  return s;
}

// ---- gz collision model ----

// Ellipses have no SDF primitive: slice along the local x axis into boxes sized at each slice's centre.
constexpr int kEllipseSlices = 24;

inline std::string sdfBox(int& n, const char* tag, float x, float y, float z, float yaw, float sx, float sy, float sz) {
  char buf[400];
  std::snprintf(buf, sizeof buf,
                "      <collision name=\"c%03d_%s\"><pose>%.3f %.3f %.3f 0 0 %.4f</pose><geometry><box><size>%.3f %.3f %.3f</size></box></geometry></collision>\n",
                n++, tag, x, y, z, yaw, sx, sy, sz);
  return buf;
}

inline std::string toSdf(const std::vector<Prim>& scene) {
  std::string o =
      "<?xml version=\"1.0\" ?>\n"
      "<!-- GENERATED by build/host/gen_campus from shared/scene/campus.hpp; edit that, not this. Collision only: the viewer\n"
      "     draws the campus. Thin or soft parts (louvers, flat decals, canopies) are visual-only and absent here. -->\n"
      "<sdf version=\"1.6\">\n  <model name=\"campus\">\n    <static>true</static>\n    <link name=\"link\">\n";
  int n = 0;
  char buf[400];
  for (const Prim& p : scene) {
    if (!p.collide) continue;
    switch (p.kind) {
      case Kind::Box: o += sdfBox(n, p.tag, p.x, p.y, p.z, p.yaw, p.a, p.b, p.c); break;
      case Kind::Cylinder:
        std::snprintf(buf, sizeof buf,
                      "      <collision name=\"c%03d_%s\"><pose>%.3f %.3f %.3f 0 %.4f %.4f</pose><geometry><cylinder><radius>%.3f</radius><length>%.3f</length></cylinder></geometry></collision>\n",
                      n++, p.tag, p.x, p.y, p.z, p.horizontal ? kPi / 2 : 0.f, p.yaw, p.a, p.c);
        o += buf;
        break;
      case Kind::Ellipse: {
        const float w = 2 * p.a / kEllipseSlices, cy = std::cos(p.yaw), sy = std::sin(p.yaw);
        for (int i = 0; i < kEllipseSlices; ++i) {
          const float lx = -p.a + (i + 0.5f) * w, ly = p.b * std::sqrt(1 - lx * lx / (p.a * p.a));
          o += sdfBox(n, p.tag, p.x + lx * cy, p.y + lx * sy, p.z, p.yaw, w, 2 * ly, p.c);
        }
        break;
      }
      case Kind::Sphere: break;
    }
  }
  o += "    </link>\n  </model>\n</sdf>\n";
  return o;
}

}  // namespace astrosim::campus
