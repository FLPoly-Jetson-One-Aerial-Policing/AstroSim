#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstring>

#include "campus.hpp"

using namespace astrosim::campus;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main() {
  const auto& s = scene();
  int louvers = 0;
  float maxTip = 0;
  for (const Prim& p : s) {
    // Everything stays on the 800 x 1100 m map, and nothing solid sits on the spawn pad or its clearing.
    const float reach = p.kind == Kind::Box ? std::hypot(p.a, p.b) / 2
                      : p.kind == Kind::Cylinder && p.horizontal ? p.c / 2 : std::fmax(p.a, p.b);
    if (std::strcmp(p.tag, "ring_road") && std::strcmp(p.tag, "approach") && std::strcmp(p.tag, "highway") && std::strcmp(p.tag, "path")) CHECK(std::fabs(p.x - kMapCx) + reach <= kMapHalfX && std::fabs(p.y - kMapCy) + reach <= kMapHalfY);
    if (p.collide) CHECK(std::hypot(p.x, p.y) - reach > 25);
    if (!std::strcmp(p.tag, "louver")) {
      ++louvers;
      maxTip = std::fmax(maxTip, p.z + p.c / 2);
    }
    if (std::strcmp(p.tag, "ring_road") && std::strcmp(p.tag, "approach") && std::strcmp(p.tag, "highway") && std::strcmp(p.tag, "admissions") && std::strcmp(p.tag, "police") && std::strcmp(p.tag, "roundabout") && std::strcmp(p.tag, "roundabout_island") && std::strcmp(p.tag, "path") && p.kind != Kind::Sphere &&
        std::strncmp(p.tag, "palm", 4))
      { bool ok = insideRing(p.x, p.y, kRoadW); if (!ok) std::fprintf(stderr, "outside ring: %s %.0f %.0f\n", p.tag, p.x, p.y); CHECK(ok); }
  }
  CHECK(louvers == 2 * kArmsPerBank);  // 94
  CHECK(std::fabs(maxTip - 130 * kFt) < 0.2f);  // upright tips near 130 ft
  CHECK(std::fabs(armLength(0) - 62 * kFt) < 1e-4f && armLength(125 * kFt) < 1e-3f);

  // The committed collision model is what the generator would write today.
  std::ifstream in(ASTROSIM_CAMPUS_SDF);
  std::stringstream ss;
  ss << in.rdbuf();
  CHECK(in.good() || in.eof());
  CHECK(ss.str() == toSdf(s));
  if (fails) std::fprintf(stderr, "campus sdf stale? run build/host/gen_campus\n");
  return fails ? 1 : 0;
}
