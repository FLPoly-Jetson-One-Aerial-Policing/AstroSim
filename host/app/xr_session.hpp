#pragma once
// SPIKE (poc/gzsim-openxr): minimal OpenXR session on top of the *current* GLX context (raylib's).
// Kept free of raylib/X11 headers so the two worlds' names never meet.
#include <functional>
#include <memory>

namespace astrosim {

struct EyeView {
  float pos[3];    // metres, XR local space (y up, -z forward)
  float quat[4];   // x y z w
  float fov[4];    // angleLeft, angleRight, angleUp, angleDown (radians, left/down negative)
  unsigned colorTex;
  int w, h;
};

struct XrSticks {
  float lx = 0, ly = 0, rx = 0, ry = 0;  // OpenXR convention: stick up = +y
  bool valid = false;                    // false until the session is focused
};

class XrSession {
 public:
  XrSession();
  ~XrSession();
  // Call with a current GLX context. Prints why it failed and returns false.
  bool init();
  bool running() const;
  // Pump events, run one frame. render() is called once per eye (left, right) when the runtime wants pixels.
  // Returns false once the session is over.
  bool frame(const std::function<void(const EyeView&, int eye)>& render, XrSticks& sticks);
  void eyeSize(int& w, int& h) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> p;
};

}  // namespace astrosim
