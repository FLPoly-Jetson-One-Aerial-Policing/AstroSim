#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#include "wire.hpp"

using namespace astrosim;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static ControlMsg goldenControl() { return {0.5f, -0.25f, 1.0f, -1.0f}; }
static StateMsg goldenState() {
  StateMsg s;
  s.sim_time = 12.345;
  s.pos[0] = 1.5f; s.pos[1] = -2.5f; s.pos[2] = 3.25f;
  s.quat[0] = 0.f; s.quat[1] = 0.f; s.quat[2] = 0.70710678f; s.quat[3] = 0.70710678f;
  s.vel[0] = 0.1f; s.vel[1] = 0.2f; s.vel[2] = -0.3f;
  s.ang_vel[0] = 0.01f; s.ang_vel[1] = 0.02f; s.ang_vel[2] = 0.03f;
  s.flags = kGzConnected; s.echo_seq = 4242;
  return s;
}

// Golden files are shared with the Go orchestrator tests. UPDATE_GOLDEN=1 rewrites them.
static void golden(const std::string& name, const std::vector<uint8_t>& bytes) {
  const std::string path = std::string(GOLDEN_DIR) + "/" + name;
  if (std::getenv("UPDATE_GOLDEN")) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return;
  }
  std::ifstream f(path, std::ios::binary);
  CHECK(f.good());
  std::vector<uint8_t> want((std::istreambuf_iterator<char>(f)), {});
  CHECK(want == bytes);
}

int main() {
  // sizes and round trips
  auto c = wire::encode(goldenControl(), 7);
  CHECK(c.size() == wire::kControlSize);
  auto dc = wire::decode(c.data(), c.size());
  CHECK(dc && dc->seq == 7);
  auto* cm = dc ? std::get_if<ControlMsg>(&dc->msg) : nullptr;
  CHECK(cm && cm->roll == 0.5f && cm->pitch == -0.25f && cm->yaw == 1.f && cm->throttle == -1.f);

  auto s = wire::encode(goldenState(), 0xFFFFFFFEu);
  CHECK(s.size() == wire::kStateSize);
  auto ds = wire::decode(s.data(), s.size());
  auto* sm = ds ? std::get_if<StateMsg>(&ds->msg) : nullptr;
  CHECK(sm && ds->seq == 0xFFFFFFFEu && sm->sim_time == 12.345 && sm->pos[1] == -2.5f &&
        sm->quat[3] == 0.70710678f && sm->ang_vel[2] == 0.03f && sm->flags == kGzConnected && sm->echo_seq == 4242);

  auto f = wire::encode(FaultMsg{3}, 1);
  CHECK(f.size() == wire::kFaultSize);
  auto df = wire::decode(f.data(), f.size());
  auto* fm = df ? std::get_if<FaultMsg>(&df->msg) : nullptr;
  CHECK(fm && fm->motor_index == 3);

  // little-endian layout spot checks
  CHECK(c[0] == 0x7A && c[1] == 0xA5 && c[2] == wire::kVersion && c[3] == 1 && c[4] == 7 && c[5] == 0);

  // rejection
  CHECK(!wire::decode(nullptr, 0));
  CHECK(!wire::decode(c.data(), 0));
  CHECK(!wire::decode(c.data(), wire::kHeaderSize - 1));
  for (size_t n = 0; n < c.size(); ++n) CHECK(!wire::decode(c.data(), n));            // truncated
  auto big = c; big.push_back(0); CHECK(!wire::decode(big.data(), big.size()));       // oversized
  auto bad = c; bad[0] ^= 0xff; CHECK(!wire::decode(bad.data(), bad.size()));         // magic
  bad = c; bad[2] = wire::kVersion + 1; CHECK(!wire::decode(bad.data(), bad.size())); // version
  bad = c; bad[3] = 0; CHECK(!wire::decode(bad.data(), bad.size()));                  // type 0
  bad = c; bad[3] = 99; CHECK(!wire::decode(bad.data(), bad.size()));                 // unknown type
  bad = c; bad[3] = 2; CHECK(!wire::decode(bad.data(), bad.size()));                  // type/size mismatch

  // sequence wraparound
  CHECK(wire::seqNewer(2, 1));
  CHECK(!wire::seqNewer(1, 2));
  CHECK(!wire::seqNewer(5, 5));
  CHECK(wire::seqNewer(0, 0xFFFFFFFFu));

  // sanitize
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  auto z = sanitize({nan, inf, -inf, 5.f});
  CHECK(z.roll == 0.f && z.pitch == 0.f && z.yaw == 0.f && z.throttle == 1.f);
  auto z2 = sanitize({-3.f, 0.3f, 1.f, -1.f});
  CHECK(z2.roll == -1.f && z2.pitch == 0.3f && z2.yaw == 1.f && z2.throttle == -1.f);

  golden("control.bin", wire::encode(goldenControl(), 7));
  golden("state.bin", wire::encode(goldenState(), 0xFFFFFFFEu));
  golden("fault.bin", wire::encode(FaultMsg{3}, 1));

  if (fails) { std::fprintf(stderr, "%d failure(s)\n", fails); return 1; }
  std::puts("test_wire OK");
  return 0;
}
