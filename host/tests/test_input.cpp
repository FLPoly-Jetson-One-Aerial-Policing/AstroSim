#include <cmath>
#include <cstdio>
#include <limits>

#include "input_map.hpp"

using namespace astrosim;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
static bool near(float a, float b, float e = 1e-5f) { return std::fabs(a - b) <= e; }

int main() {
  // centered / inside deadzone -> exact zero
  auto c = mapGamepad(0, 0, 0, 0);
  CHECK(c.roll == 0 && c.pitch == 0 && c.yaw == 0 && c.throttle == 0);
  c = mapGamepad(0.05f, -0.07f, 0.09f, 0.03f);
  CHECK(c.roll == 0 && c.pitch == 0 && c.yaw == 0 && c.throttle == 0);

  // full deflection reaches +/-1; stick up (negative) = forward / climb
  c = mapGamepad(0, -1, 0, -1);
  CHECK(near(c.throttle, 1) && near(c.pitch, 1) && c.yaw == 0 && c.roll == 0);
  c = mapGamepad(0, 1, 0, 1);
  CHECK(near(c.throttle, -1) && near(c.pitch, -1));
  c = mapGamepad(1, 0, 1, 0);
  CHECK(near(c.yaw, 1) && near(c.roll, 1) && c.throttle == 0 && c.pitch == 0);
  c = mapGamepad(-1, 0, -1, 0);
  CHECK(near(c.yaw, -1) && near(c.roll, -1));

  // sticks are independent
  c = mapGamepad(0.5f, 0, 0, -1);
  CHECK(c.yaw > 0.4f && c.yaw < 0.5f && near(c.pitch, 1) && c.roll == 0 && c.throttle == 0);

  // radial rescale: just past the deadzone is small and continuous; diagonal corner clamps to unit circle
  c = mapGamepad(0, -0.12f, 0, 0);
  CHECK(c.throttle > 0 && c.throttle < 0.05f);
  c = mapGamepad(1, 1, 0, 0);  // corner (mag 1.41) clamps
  CHECK(c.yaw > 0 && c.yaw <= 1 && c.throttle < 0 && c.throttle >= -1);
  CHECK(near(std::hypot(c.yaw, c.throttle), 1.f, 1e-4f));
  c = mapGamepad(5, -5, 0, 0);  // out-of-range input still bounded
  CHECK(std::fabs(c.yaw) <= 1 && std::fabs(c.throttle) <= 1);

  // monotonic along an axis
  float prev = -1;
  for (float v = 0; v <= 1.0f; v += 0.05f) { float t = mapGamepad(0, -v, 0, 0).throttle; CHECK(t >= prev); prev = t; }

  // NaN / inf
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  c = mapGamepad(nan, 0.5f, inf, -inf);
  CHECK(c.roll == 0 && c.pitch == 0 && c.yaw == 0 && c.throttle == 0);

  // keyboard: WASD = left stick (throttle/yaw), arrows = right stick (pitch/roll)
  Keys k;
  auto kc = mapKeys(k); CHECK(kc.roll == 0 && kc.pitch == 0 && kc.yaw == 0 && kc.throttle == 0);
  k.w = true; kc = mapKeys(k); CHECK(kc.throttle == 1 && kc.yaw == 0 && kc.pitch == 0 && kc.roll == 0);
  k = {}; k.s = true; kc = mapKeys(k); CHECK(kc.throttle == -1 && kc.pitch == 0);
  k = {}; k.d = true; kc = mapKeys(k); CHECK(kc.yaw == 1 && kc.roll == 0 && kc.throttle == 0);
  k = {}; k.a = true; kc = mapKeys(k); CHECK(kc.yaw == -1 && kc.roll == 0);
  k = {}; k.up = true; kc = mapKeys(k); CHECK(kc.pitch == 1 && kc.throttle == 0);
  k = {}; k.down = true; kc = mapKeys(k); CHECK(kc.pitch == -1 && kc.throttle == 0);
  k = {}; k.right = true; kc = mapKeys(k); CHECK(kc.roll == 1 && kc.yaw == 0);
  k = {}; k.left = true; kc = mapKeys(k); CHECK(kc.roll == -1 && kc.yaw == 0);
  k = {}; k.w = k.s = k.up = k.down = k.left = k.right = k.a = k.d = true;  // opposing keys cancel
  kc = mapKeys(k); CHECK(kc.roll == 0 && kc.pitch == 0 && kc.yaw == 0 && kc.throttle == 0);
  k = {}; k.w = k.d = k.up = k.right = true;  // all four axes at once
  kc = mapKeys(k); CHECK(kc.throttle == 1 && kc.yaw == 1 && kc.pitch == 1 && kc.roll == 1);

  if (fails) { std::fprintf(stderr, "%d failure(s)\n", fails); return 1; }
  std::puts("test_input OK");
  return 0;
}
