// SPIKE: OpenXR (XR_KHR_opengl_enable, Xlib/GLX) session bound to the GLX context that is current at init().
#include "xr_session.hpp"

#include <GL/glx.h>
#include <X11/Xlib.h>

#include <cstdio>
#include <cstring>
#include <vector>

#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace astrosim {

#define XR_TRY(call)                                                                  \
  do {                                                                                \
    XrResult r_ = (call);                                                             \
    if (XR_FAILED(r_)) { std::fprintf(stderr, "xr: %s failed (%d)\n", #call, (int)r_); return false; } \
  } while (0)

struct XrSession::Impl {
  XrInstance inst = XR_NULL_HANDLE;
  XrSystemId sys = XR_NULL_SYSTEM_ID;
  ::XrSession sess = XR_NULL_HANDLE;
  XrSpace space = XR_NULL_HANDLE;
  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  bool sessionRunning = false, over = false;
  int w = 0, h = 0;
  struct Eye { XrSwapchain sc = XR_NULL_HANDLE; std::vector<XrSwapchainImageOpenGLKHR> imgs; } eye[2];
  XrActionSet set = XR_NULL_HANDLE;
  XrAction leftStick = XR_NULL_HANDLE, rightStick = XR_NULL_HANDLE;
  int64_t colorFormat = 0;

  bool pollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
      if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
        std::fprintf(stderr, "xr: session state %d\n", (int)state);
        if (state == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
          bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          if (XR_SUCCEEDED(xrBeginSession(sess, &bi))) sessionRunning = true;
        } else if (state == XR_SESSION_STATE_STOPPING) {
          xrEndSession(sess);
          sessionRunning = false;
        } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
          over = true;
        }
      } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
        over = true;
      }
      ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
    return !over;
  }

  bool makeActions() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(si.actionSetName, "flight");
    std::strcpy(si.localizedActionSetName, "Flight");
    XR_TRY(xrCreateActionSet(inst, &si, &set));
    auto mk = [&](const char* name, XrAction& out) {
      XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
      ai.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
      std::strcpy(ai.actionName, name);
      std::strcpy(ai.localizedActionName, name);
      return XR_SUCCEEDED(xrCreateAction(set, &ai, &out));
    };
    if (!mk("left_stick", leftStick) || !mk("right_stick", rightStick)) return false;

    auto path = [&](const char* s) { XrPath p; xrStringToPath(inst, s, &p); return p; };
    std::vector<XrActionSuggestedBinding> b = {
        {leftStick, path("/user/hand/left/input/thumbstick")},
        {rightStick, path("/user/hand/right/input/thumbstick")},
    };
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = b.data();
    sb.countSuggestedBindings = (uint32_t)b.size();
    XR_TRY(xrSuggestInteractionProfileBindings(inst, &sb));

    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &set;
    XR_TRY(xrAttachSessionActionSets(sess, &at));
    return true;
  }
};

XrSession::XrSession() : p(new Impl) {}
XrSession::~XrSession() {
  if (!p) return;
  for (auto& e : p->eye) if (e.sc) xrDestroySwapchain(e.sc);
  if (p->space) xrDestroySpace(p->space);
  if (p->sess) xrDestroySession(p->sess);
  if (p->inst) xrDestroyInstance(p->inst);
}

bool XrSession::running() const { return p->sessionRunning; }
void XrSession::eyeSize(int& w, int& h) const { w = p->w; h = p->h; }

bool XrSession::init() {
  Impl& s = *p;
  const char* exts[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strcpy(ci.applicationInfo.applicationName, "AstroSim XR spike");
  ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = exts;
  XR_TRY(xrCreateInstance(&ci, &s.inst));

  XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
  gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XR_TRY(xrGetSystem(s.inst, &gi, &s.sys));

  PFN_xrGetOpenGLGraphicsRequirementsKHR getReq = nullptr;
  XR_TRY(xrGetInstanceProcAddr(s.inst, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
  XrGraphicsRequirementsOpenGLKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
  XR_TRY(getReq(s.inst, s.sys, &req));  // mandatory call before xrCreateSession

  // Describe the GLX context raylib/GLFW made current.
  Display* dpy = glXGetCurrentDisplay();
  GLXContext ctx = glXGetCurrentContext();
  GLXDrawable draw = glXGetCurrentDrawable();
  if (!dpy || !ctx) { std::fprintf(stderr, "xr: no current GLX context (is the window running on X11/XWayland?)\n"); return false; }
  int fbid = 0, scr = 0;
  glXQueryContext(dpy, ctx, GLX_FBCONFIG_ID, &fbid);
  glXQueryContext(dpy, ctx, GLX_SCREEN, &scr);
  int attr[] = {GLX_FBCONFIG_ID, fbid, None};
  int n = 0;
  GLXFBConfig* fbc = glXChooseFBConfig(dpy, scr, attr, &n);
  if (!fbc || n < 1) { std::fprintf(stderr, "xr: cannot resolve GLXFBConfig %d\n", fbid); return false; }
  XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, fbc[0]);

  XrGraphicsBindingOpenGLXlibKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
  gb.xDisplay = dpy;
  gb.visualid = vi ? (uint32_t)vi->visualid : 0;
  gb.glxFBConfig = fbc[0];
  gb.glxDrawable = draw;
  gb.glxContext = ctx;
  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &gb;
  sci.systemId = s.sys;
  XR_TRY(xrCreateSession(s.inst, &sci, &s.sess));

  XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;  // seated: origin = where the user started
  rs.poseInReferenceSpace.orientation.w = 1;
  XR_TRY(xrCreateReferenceSpace(s.sess, &rs, &s.space));

  uint32_t nv = 0;
  XR_TRY(xrEnumerateViewConfigurationViews(s.inst, s.sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr));
  std::vector<XrViewConfigurationView> vcv(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  XR_TRY(xrEnumerateViewConfigurationViews(s.inst, s.sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv, vcv.data()));
  if (nv != 2) { std::fprintf(stderr, "xr: expected 2 views, got %u\n", nv); return false; }
  s.w = (int)vcv[0].recommendedImageRectWidth;
  s.h = (int)vcv[0].recommendedImageRectHeight;

  uint32_t nf = 0;
  XR_TRY(xrEnumerateSwapchainFormats(s.sess, 0, &nf, nullptr));
  std::vector<int64_t> fmts(nf);
  XR_TRY(xrEnumerateSwapchainFormats(s.sess, nf, &nf, fmts.data()));
  s.colorFormat = fmts.empty() ? 0 : fmts[0];
  for (int64_t f : fmts) if (f == 0x8058 /*GL_RGBA8*/) s.colorFormat = f;  // avoid sRGB: raylib shades in gamma space
  std::fprintf(stderr, "xr: eye %dx%d, swapchain format 0x%llx (%u offered)\n", s.w, s.h, (unsigned long long)s.colorFormat, nf);

  for (auto& e : s.eye) {
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sc.format = s.colorFormat;
    sc.sampleCount = 1;
    sc.width = (uint32_t)s.w;
    sc.height = (uint32_t)s.h;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    XR_TRY(xrCreateSwapchain(s.sess, &sc, &e.sc));
    uint32_t ni = 0;
    XR_TRY(xrEnumerateSwapchainImages(e.sc, 0, &ni, nullptr));
    e.imgs.assign(ni, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    XR_TRY(xrEnumerateSwapchainImages(e.sc, ni, &ni, (XrSwapchainImageBaseHeader*)e.imgs.data()));
  }
  return s.makeActions();
}

bool XrSession::frame(const std::function<void(const EyeView&, int)>& render, XrSticks& sticks) {
  Impl& s = *p;
  if (!s.pollEvents()) return false;
  if (!s.sessionRunning) return true;  // waiting for READY; caller should sleep a little

  XrFrameState fs{XR_TYPE_FRAME_STATE};
  XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
  if (XR_FAILED(xrWaitFrame(s.sess, &wi, &fs))) return false;
  XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
  if (XR_FAILED(xrBeginFrame(s.sess, &bi))) return false;

  // Sticks (only valid once focused).
  sticks.valid = false;
  if (s.state == XR_SESSION_STATE_FOCUSED) {
    XrActiveActionSet as{s.set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    if (XR_SUCCEEDED(xrSyncActions(s.sess, &si))) {
      auto get = [&](XrAction a, float& x, float& y) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(s.sess, &gi, &st)) && st.isActive) { x = st.currentState.x; y = st.currentState.y; }
      };
      get(s.leftStick, sticks.lx, sticks.ly);
      get(s.rightStick, sticks.rx, sticks.ry);
      sticks.valid = true;
    }
  }

  std::vector<XrCompositionLayerProjectionView> pv(2, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
  XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  const XrCompositionLayerBaseHeader* layers[1] = {nullptr};
  uint32_t nlayers = 0;

  if (fs.shouldRender) {
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = s.space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t nv = 0;
    bool located = XR_SUCCEEDED(xrLocateViews(s.sess, &li, &vs, 2, &nv, views)) &&
                   (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    if (located) {
      for (int i = 0; i < 2; ++i) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        xrAcquireSwapchainImage(s.eye[i].sc, &ai, &idx);
        XrSwapchainImageWaitInfo wt{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wt.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(s.eye[i].sc, &wt);

        const XrView& v = views[i];
        EyeView ev{{v.pose.position.x, v.pose.position.y, v.pose.position.z},
                   {v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w},
                   {v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown},
                   s.eye[i].imgs[idx].image, s.w, s.h};
        render(ev, i);

        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(s.eye[i].sc, &ri);
        pv[i].pose = v.pose;
        pv[i].fov = v.fov;
        pv[i].subImage.swapchain = s.eye[i].sc;
        pv[i].subImage.imageRect = {{0, 0}, {s.w, s.h}};
      }
      layer.space = s.space;
      layer.viewCount = 2;
      layer.views = pv.data();
      layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);
      nlayers = 1;
    }
  }

  XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
  ei.displayTime = fs.predictedDisplayTime;
  ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  ei.layerCount = nlayers;
  ei.layers = layers;
  return XR_SUCCEEDED(xrEndFrame(s.sess, &ei));
}

}  // namespace astrosim
