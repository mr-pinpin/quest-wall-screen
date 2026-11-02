// Native room lab: passthrough, hands, Meta scene walls/volumes, environment depth occlusion,
// and pinch placement of a desktop-sized panel onto a validated wall area.
// Reports live state to files/status.json; placement persists by wall anchor UUID.
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include "moonlight.h"
#include "voice.h"
#include "ui_grid.h"
#include "spell.h"
#include "gamestream.h"
#include "panel_layer.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PinPinRoomLab", __VA_ARGS__)

static JavaVM* gVm = nullptr;
static jobject gActivity = nullptr;
static std::string gDir;
static bool gRequestCapture = false;
static std::atomic<bool> gQuit{false};
static std::atomic<bool> gJavaStop{false};
static std::thread gThread;

// ---------------------------------------------------------------- math
struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float len(V3 a) { return std::sqrt(dot(a, a)); }
static V3 norm(V3 a) { float l = len(a); return l > 1e-6f ? a * (1.0f / l) : V3{0, 0, 0}; }
static V3 v3(XrVector3f v) { return {v.x, v.y, v.z}; }
static XrQuaternionf qinv(XrQuaternionf q) { return {-q.x, -q.y, -q.z, q.w}; }
static XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static V3 rot(XrQuaternionf q, V3 v) {
    V3 u{q.x, q.y, q.z};
    V3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
static V3 xform(const XrPosef& p, V3 v) { return rot(p.orientation, v) + v3(p.position); }
static V3 untransform(const XrPosef& p, V3 v) { return rot(qinv(p.orientation), v - v3(p.position)); }

struct M4 { float m[16]; };
static M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
        r.m[c * 4 + rr] = s;
    }
    return r;
}
static M4 viewFromPose(const XrPosef& p) {
    XrQuaternionf qi = qinv(p.orientation);
    V3 c0 = rot(qi, {1, 0, 0}), c1 = rot(qi, {0, 1, 0}), c2 = rot(qi, {0, 0, 1});
    V3 t = rot(qi, v3(p.position)) * -1.0f;
    return M4{{c0.x, c0.y, c0.z, 0, c1.x, c1.y, c1.z, 0, c2.x, c2.y, c2.z, 0, t.x, t.y, t.z, 1}};
}
static M4 projection(const XrFovf& f, float n, float far) {
    float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), u = std::tan(f.angleUp), d = std::tan(f.angleDown);
    float w = r - l, h = u - d;
    M4 m{};
    m.m[0] = 2 / w; m.m[8] = (r + l) / w;
    m.m[5] = 2 / h; m.m[9] = (u + d) / h;
    if (std::isinf(far) || far <= n) { m.m[10] = -1; m.m[14] = -2 * n; }
    else { m.m[10] = -(far + n) / (far - n); m.m[14] = -2 * far * n / (far - n); }
    m.m[11] = -1;
    return m;
}

// ---------------------------------------------------------------- geometry
struct Vtx { float p[3]; float c[4]; float uv[2]; };
struct Batch {
    std::vector<Vtx> v;
    void tri(V3 a, V3 b, V3 c, const float* col, const float* ua = nullptr, const float* ub = nullptr, const float* uc = nullptr) {
        static const float none[2] = {-1, -1};
        v.push_back({{a.x, a.y, a.z}, {col[0], col[1], col[2], col[3]}, {(ua ? ua : none)[0], (ua ? ua : none)[1]}});
        v.push_back({{b.x, b.y, b.z}, {col[0], col[1], col[2], col[3]}, {(ub ? ub : none)[0], (ub ? ub : none)[1]}});
        v.push_back({{c.x, c.y, c.z}, {col[0], col[1], col[2], col[3]}, {(uc ? uc : none)[0], (uc ? uc : none)[1]}});
    }
    void quad(V3 a, V3 b, V3 c, V3 d, const float* col, bool screen = false) {
        if (screen) {
            const float ua[2] = {0, 0}, ub[2] = {1, 0}, uc[2] = {1, 1}, ud[2] = {0, 1};
            tri(a, b, c, col, ua, ub, uc); tri(a, c, d, col, ua, uc, ud);
        } else { tri(a, b, c, col); tri(a, c, d, col); }
    }
    void line(V3 a, V3 b, V3 n, float w, const float* col) {
        V3 side = norm(cross(b - a, n)) * (w * 0.5f);
        quad(a - side, b - side, b + side, a + side, col);
    }
    void ray(V3 a, V3 b, float w, const float* col) {
        V3 d = norm(b - a);
        V3 up = std::fabs(d.y) > 0.9f ? V3{1, 0, 0} : V3{0, 1, 0};
        V3 p1 = norm(cross(d, up)), p2 = cross(d, p1);
        line(a, b, p1, w, col); line(a, b, p2, w, col);
    }
    void dashed(V3 a, V3 b, V3 n, float w, const float* col, float dash = 0.06f, float gap = 0.04f) {
        float L = len(b - a); V3 d = norm(b - a);
        for (float s = 0; s < L; s += dash + gap) line(a + d * s, a + d * std::fmin(s + dash, L), n, w, col);
    }
    void cube(V3 c, float s, const float* col) {
        float h = s * 0.5f;
        V3 p[8];
        for (int i = 0; i < 8; i++) p[i] = {c.x + ((i & 1) ? h : -h), c.y + ((i & 2) ? h : -h), c.z + ((i & 4) ? h : -h)};
        int f[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
        for (auto& q : f) quad(p[q[0]], p[q[1]], p[q[2]], p[q[3]], col);
    }
};

// ---------------------------------------------------------------- scene model
struct Entity {
    XrSpace space = XR_NULL_HANDLE;
    std::string uuid, label;
    bool has2d = false, has3d = false, located = false;
    XrRect2Df r2{};
    XrRect3DfFB r3{};
    XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
};
static bool isWall(const Entity& e) { return e.has2d && e.label.find("WALL_FACE") != std::string::npos && e.label.find("INVISIBLE") == std::string::npos; }

struct Placement { bool active = false; std::string wall; float x = 0, y = 0, w = 1.44f, h = 0.9f; bool valid = false; std::string reason; };

static std::string hexUuid(const XrUuidEXT& u) {
    char b[33]; for (int i = 0; i < 16; i++) snprintf(b + i * 2, 3, "%02x", u.data[i]); return std::string(b, 32);
}
static std::string jsonEscape(const std::string& s) {
    std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; } return o;
}

// ---------------------------------------------------------------- app
struct App {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    std::set<std::string> enabledExt;
    std::vector<std::string> errors;

    EGLDisplay display = EGL_NO_DISPLAY; EGLConfig config = nullptr; EGLContext context = EGL_NO_CONTEXT; EGLSurface surface = EGL_NO_SURFACE;
    struct Eye { XrSwapchain chain; int w, h; std::vector<GLuint> fbo; GLuint depth; };
    Eye eyes[2]{};
    int msaa = 0;
    GLuint program = 0, vao = 0, vbo = 0;
    GLint uViewProj, uDepthViewProj, uDepth, uOcclude, uLayer, uDepthParams, uVideo, uHasVideo;
    VideoStream video;
    PanelLayer panelLayer;  // QUAD: the panel as a compositor quad layer
    bool bezel = true;   // BEZEL flag (default: it removed the line completely): a frame over the panel edge, drawn in the eye
                         // buffers above the layer with 4x MSAA, hides the compositor's aliased edge line
    bool bezelFade = false, bezelGlass = false, bezelAmbi = true;   // FADE / GLASS / AMBI bezel styles (grey when all off); AMBI is the user pick
    bool nativeRes = false;  // NATIVE: stream the laptop at its panel's real pixels (2880x1800) instead of 2560x1600
    float currentHz() { float hz = 0; if (xrGetDisplayRefreshRateFB_ && session) xrGetDisplayRefreshRateFB_(session, &hz); return hz; }
    GameStreamHost hostFor(int i) const {
        GameStreamHost h = hosts[i];
        if (nativeRes && i == 1) { h.width = 2880; h.height = 1800; h.kbps = 100000; }
        return h;
    }
    bool quadShown = false, quadFile = false, mipqMenu = false, mipqFile = false; long quadTick = 0;
    MoonlightSource moonlight;
    // Raise a fist (above the chest) and hold to dictate; open the hand to finish. Text is typed into the
    // streamed Mac through Moonlight and shown as a caption.
    Dictation dictation;
    static constexpr float kFistHold = 0.25f, kFistRelease = 0.3f;
    float fistTime = 0, openTime = 0; std::string caption; std::chrono::steady_clock::time_point captionAt{};
    // Throw to send: from a fist, push the arm forward and open the hand -> stop dictating and press Enter
    // once the last words are typed. Requiring a fist just before keeps ordinary gestures from sending.
    static constexpr int kTrail = 24;  // ~0.33 s of palm positions
    V3 palmTrail[2][kTrail]; int trailN[2] = {0, 0}; float sinceFist[2] = {9, 9};
    bool pendingEnter = false; float pendingFor = 0; long throws = 0, enters = 0;
    // Swipe to undo: an open hand swept left within kUndoWindow of dictating erases the last typed sentence
    // (one per swipe, newest first). Enter clears the history, so a sent message is never touched.
    static constexpr float kUndoWindow = 15.0f;
    std::vector<int> typed; std::chrono::steady_clock::time_point typedAt{}; long undos = 0;
    std::mutex keyLock;  // key sequences from helper threads never interleave
    static int codepoints(const std::string& t) { int n = 0; for (unsigned char c : t) n += (c & 0xC0) != 0x80; return n; }
    void backspaces(int n) {
        std::thread([this, n] {
            std::lock_guard<std::mutex> g(keyLock);
            for (int i = 0; i < n && streamInput(); i++) {  // 20 ms held: the virtual HID keyboard drops shorter presses
                LiSendKeyboardEvent((short)0x8008, KEY_ACTION_DOWN, 0); std::this_thread::sleep_for(std::chrono::milliseconds(20));
                LiSendKeyboardEvent((short)0x8008, KEY_ACTION_UP, 0); std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }).detach();
    }
    bool handOpen(int h) {
        auto& pj = joints[h][XR_HAND_JOINT_PALM_EXT]; int open = 0;
        for (int tip : {XR_HAND_JOINT_INDEX_TIP_EXT, XR_HAND_JOINT_MIDDLE_TIP_EXT, XR_HAND_JOINT_RING_TIP_EXT, XR_HAND_JOINT_LITTLE_TIP_EXT})
            if ((joints[h][tip].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && len(v3(joints[h][tip].pose.position) - v3(pj.pose.position)) > 0.085f) open++;
        return open >= 3;
    }
    void pressEnter() {
        // Hold it like a finger would: Sunshine on the Macs types through a virtual HID keyboard, and a down/up pair
        // sent 1 ms apart can collapse into no press at all (the Unicode text path is unaffected).
        if (streamInput()) {
            std::thread([this] {
                std::lock_guard<std::mutex> g(keyLock);
                LiSendKeyboardEvent((short)0x800D, KEY_ACTION_DOWN, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(60)); LiSendKeyboardEvent((short)0x800D, KEY_ACTION_UP, 0);
            }).detach();
        }
        typed.clear(); enters++; caption = "SENT"; captionAt = std::chrono::steady_clock::now(); event("enter sent");
    }
    GameStreamLauncher launcher;
    // Pinch a button above the panel to switch sources. The Air is reached through lan_relay.py on the Mini.
    GameStreamHost hosts[2] = {{"MINI", "192.168.2.1", 47984, "server.pem", 2560, 1440, 72, 60000},
                               {"LAPTOP", "192.168.2.1", 57984, "server-air.pem", 2560, 1600, 60, 80000}};
    int hoverButton = -1;

    PFN_xrCreatePassthroughFB xrCreatePassthroughFB_ = nullptr;
    PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB_ = nullptr; PFN_xrGetDisplayRefreshRateFB xrGetDisplayRefreshRateFB_ = nullptr;
    PFN_xrPerfSettingsSetPerformanceLevelEXT xrPerfSettingsSetPerformanceLevelEXT_ = nullptr;
    bool hz120 = false, boost = false; float baseHz = 0;  // 120HZ: headset display at 120 Hz; BOOST: GPU/CPU boost level
    PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB_ = nullptr;
    XrPassthroughFB passthrough = XR_NULL_HANDLE; XrPassthroughLayerFB ptLayer = XR_NULL_HANDLE;

    PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT_ = nullptr;
    PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT_ = nullptr;
    XrHandTrackerEXT hands[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrHandJointLocationEXT joints[2][XR_HAND_JOINT_COUNT_EXT];
    XrHandTrackingAimStateFB aim[2];
    bool handActive[2] = {false, false}, pinchPrev[2] = {false, false};
    int handResult[2] = {0, 0};
    // Ray and press derived from joints. The ray runs shoulder -> index/middle knuckles, joints that stay still
    // while the thumb moves. Press is the "finger gun" (thumb tip down onto the side of the middle finger), with
    // pinch kept as a fallback. A press rewinds the ray ~0.1 s (to before the gesture began) and locks it there
    // until the hand moves more than kLockDeg, so a press-release in place is a clean click and only a real
    // movement becomes a drag/selection.
    static constexpr int kHist = 16, kRewind = 7; static constexpr float kLockDeg = 3.5f;
    struct HandRay {
        bool valid = false, pinch = false, thumbDown = false, pinchDown = false, locked = false, wasPressed = false, fist = false;
        V3 o{0, 0, 0}, d{0, 0, 0}, tip{0, 0, 0}, lockO{0, 0, 0}, lockD{0, 0, 0};
        V3 histO[kHist], histD[kHist]; int histN = 0;
        float gap = 1, thumb = 1, thumbMin = 1, thumbMax = 0; long thumbPresses = 0, pinchPresses = 0, lockBreaks = 0;
    };
    HandRay ray[2];

    PFN_xrQuerySpacesFB xrQuerySpacesFB_ = nullptr;
    PFN_xrRetrieveSpaceQueryResultsFB xrRetrieveSpaceQueryResultsFB_ = nullptr;
    PFN_xrGetSpaceComponentStatusFB xrGetSpaceComponentStatusFB_ = nullptr;
    PFN_xrSetSpaceComponentStatusFB xrSetSpaceComponentStatusFB_ = nullptr;
    PFN_xrGetSpaceBoundingBox2DFB xrGetSpaceBoundingBox2DFB_ = nullptr;
    PFN_xrGetSpaceBoundingBox3DFB xrGetSpaceBoundingBox3DFB_ = nullptr;
    PFN_xrGetSpaceSemanticLabelsFB xrGetSpaceSemanticLabelsFB_ = nullptr;
    PFN_xrRequestSceneCaptureFB xrRequestSceneCaptureFB_ = nullptr;
    std::vector<Entity> entities;
    std::string sceneState = "idle";
    XrAsyncRequestIdFB queryId = 0;

    PFN_xrCreateEnvironmentDepthProviderMETA xrCreateEnvironmentDepthProviderMETA_ = nullptr;
    PFN_xrStartEnvironmentDepthProviderMETA xrStartEnvironmentDepthProviderMETA_ = nullptr;
    PFN_xrCreateEnvironmentDepthSwapchainMETA xrCreateEnvironmentDepthSwapchainMETA_ = nullptr;
    PFN_xrGetEnvironmentDepthSwapchainStateMETA xrGetEnvironmentDepthSwapchainStateMETA_ = nullptr;
    PFN_xrEnumerateEnvironmentDepthSwapchainImagesMETA xrEnumerateEnvironmentDepthSwapchainImagesMETA_ = nullptr;
    PFN_xrAcquireEnvironmentDepthImageMETA xrAcquireEnvironmentDepthImageMETA_ = nullptr;
    XrEnvironmentDepthProviderMETA depthProvider = XR_NULL_HANDLE;
    XrEnvironmentDepthSwapchainMETA depthChain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> depthImages;
    std::set<GLuint> depthConfigured;
    uint32_t depthW = 0, depthH = 0;
    bool depthStarted = false, depthValid = false;
    XrEnvironmentDepthImageMETA depthImage{};
    long depthAcquired = 0, depthAcquiredSec = 0, depthAcquiredLastSec = 0;
    int depthLastResult = 0;

    Placement candidate, placed;
    int sizeIndex = 1;
    // Screens stay put. Moving one is deliberate: pinch and hold the grab bar under it, drag, release to drop.
    static constexpr float kHoldToMove = 0.5f, kBarW = 0.36f, kBarH = 0.035f, kBarGap = 0.045f;
    bool moving = false, barHover = false, barHolding = false; float barHold = 0;
    std::chrono::steady_clock::time_point barPinchStart{};
    // Resize: pinch with both hands while pointing at the screen and pull apart, or drag a corner.
    static constexpr float kMinW = 0.5f, kCorner = 0.08f;
    bool resizing2 = false; float resizeStartW = 0, resizeStartD = 1;
    // Settings: hold the back of the left wrist toward your face for kPalmHold to open (or close) a floating panel of big buttons.
    static constexpr float kPalmHold = 0.5f;
    // LOCK pins the screen and the settings panel: the panel is always shown where it was left, and is saved in
    // the frame of a scene wall so it comes back in the same spot. Unlocked, the watch gesture re-places it.
    bool menuOpen = false, editLocked = true, palmArmed = true; float palmHold = 0; int menuHover = -1;
    bool settingsLoaded = false, menuSaved = false; std::string menuWall; XrPosef menuRel{{0, 0, 0, 1}, {0, 0, 0}};
    // Unlocked, the panel has a grab bar like the screen: hold, drag, and it sticks flat to the wall you point at.
    bool menuRestored = false, menuMoving = false, menuBarHover = false; float menuBarHold = 0; std::string menuStickWall;
    std::chrono::steady_clock::time_point menuBarStart{};
    float menuHalfW() const { return menuGrid.halfW; }
    float menuHalfH() const { return menuGrid.halfH; }
    float menuBarY() const { return menuGrid.barY; }
    // Real walls hide what is behind them: something on a wall shows only from the side it faces, and only
    // while no other scanned wall stands between the eye and it. The compositor layer has no depth, so this
    // check is what keeps the kitchen screen from showing through the wall in the next room.
    bool seenFrom(V3 eye, V3 c, V3 n, const std::string& ownWall) {
        if (dot(eye - c, n) < 0.02f) return false;  // behind its wall
        V3 d = eye - c; float L = len(d); if (L < 1e-3f) return true; d = d * (1.0f / L);
        for (auto& e : entities) {
            if (!isWall(e) || !e.located || e.uuid == ownWall) continue;
            V3 lo = untransform(e.pose, c), ld = rot(qinv(e.pose.orientation), d);
            if (std::fabs(ld.z) < 1e-4f) continue;
            float t = -lo.z / ld.z; if (t < 0.05f || t > L - 0.05f) continue;
            float x = lo.x + ld.x * t, y = lo.y + ld.y * t;
            if (x > e.r2.offset.x && y > e.r2.offset.y && x < e.r2.offset.x + e.r2.extent.width && y < e.r2.offset.y + e.r2.extent.height) return false;
        }
        return true;
    }
    bool panelSeen = true, menuSeen = true;
    void updateVisibility() {
        V3 eye = v3(head.position);
        panelSeen = true;
        if (placed.active) if (auto* w = wallById(placed.wall))
            panelSeen = seenFrom(eye, xform(w->pose, {placed.x, placed.y, 0.02f}), rot(w->pose.orientation, {0, 0, 1}), placed.wall);
        menuSeen = !menuOpen || menuMoving || seenFrom(eye, v3(menu.pose.position), rot(menu.pose.orientation, {0, 0, 1}), menuStickWall);
    }
    Entity* rayWall(V3 o, V3 d, float& hx, float& hy) {
        float best = 1e9f; Entity* hit = nullptr;
        for (auto& e : entities) {
            if (!isWall(e) || !e.located) continue;
            V3 lo = untransform(e.pose, o), ld = rot(qinv(e.pose.orientation), d);
            if (std::fabs(ld.z) < 1e-4f) continue;
            float t = -lo.z / ld.z; if (t <= 0 || t > best) continue;
            float x = lo.x + ld.x * t, y = lo.y + ld.y * t;
            if (x < e.r2.offset.x || y < e.r2.offset.y || x > e.r2.offset.x + e.r2.extent.width || y > e.r2.offset.y + e.r2.extent.height) continue;
            best = t; hit = &e; hx = x; hy = y;
        }
        return hit;
    }
    std::chrono::steady_clock::time_point palmStart{};
    Entity menu;  // only its pose is used: the panel's plane, facing the user
    // Settings items, in order. The panel lays itself out from this list (ui_grid.h): items flow into rows of three,
    // each section starts a new row under its heading, and the panel grows to fit. To add a flag: one line here,
    // its action in choose() and, for a toggle, its state in itemOn().
    struct MenuItem { const char* label; const char* section; };
    static constexpr int kMenuItems = 16;
    const MenuItem menuItems[kMenuItems] = {{"MINI", "SOURCE"}, {"LAPTOP", "SOURCE"},
                                            {"SMALL", "SIZE"}, {"MEDIUM", "SIZE"}, {"LARGE", "SIZE"},
                                            {"LOCK", "PANEL"}, {"QUAD", "PANEL"}, {"CLOSE", "PANEL"},
                                            {"CURVE", "DISPLAY"}, {"MARGIN", "BORDER"}, {"SUPER", "BORDER"}, {"BEZEL", "BORDER"},
                                            {"FADE", "BEZEL"}, {"GLASS", "BEZEL"}, {"AMBI", "BEZEL"}, {"SPELL", "GESTURES"}};
    UiGrid menuGrid; std::vector<int> menuShown;  // grid cell k shows menuItems[menuShown[k]]
    bool itemVisible(const std::string& l) const { return !(l == "CLOSE" && editLocked); }  // pinned: nothing to close
    // Highlight state: the active source, or a toggle that is on. -1 means not a toggle.
    int itemOn(const std::string& l) {
        if (l == "MINI" || l == "LAPTOP") return video.source == l;
        if (l == "LOCK") return editLocked;
        if (l == "QUAD") return panelLayer.enabled;
        if (l == "CURVE") return panelLayer.curve;
        if (l == "SPELL") return spellOn;
        if (l == "MARGIN") return panelLayer.margin;
        if (l == "SUPER") return panelLayer.super;
        if (l == "BEZEL") return bezel;
        if (l == "FADE") return bezelFade;
        if (l == "GLASS") return bezelGlass;
        if (l == "AMBI") return bezelAmbi;
        return -1;
    }
    void layoutMenu() {
        std::vector<UiGrid::Item> items; menuShown.clear();
        for (int i = 0; i < kMenuItems; i++) if (itemVisible(menuItems[i].label)) { items.push_back({menuItems[i].label, menuItems[i].section}); menuShown.push_back(i); }
        menuGrid.layout(items);
    }
    bool palmFacing(V3& at) {
        if (!handActive[0]) return false;
        auto& pj = joints[0][XR_HAND_JOINT_PALM_EXT];
        if (!(pj.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) || !(pj.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return false;
        at = v3(pj.pose.position);
        // The back of the wrist toward the face ("check your watch"): the palm-toward-face pose is Meta's system menu gesture.
        V3 out = rot(pj.pose.orientation, {0, 1, 0});  // OpenXR hand joints: +Y is the back of the hand
        return dot(out, norm(v3(head.position) - at)) > 0.75f && len(v3(head.position) - at) < 0.7f;
    }
    void openMenu() {
        V3 f = rot(head.orientation, {0, 0, -1}); f.y = 0; f = norm(f);
        if (len(f) < 0.5f) f = {0, 0, -1};
        float yaw = std::atan2(-f.x, -f.z);
        menu.pose.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
        V3 c = v3(head.position) + f * 0.6f; c.y -= 0.12f;
        menu.pose.position = {c.x, c.y, c.z};
        menuOpen = true; menuHover = -1; event("settings opened");
        if (!menuSaved) editLocked = true;  // first placement: pin it right away
        saveSettings();
    }
    Entity* anyWall() {
        if (auto* w = placed.active ? wallById(placed.wall) : nullptr) if (w->located) return w;
        for (auto& e : entities) if (isWall(e) && e.located) return &e;
        return nullptr;
    }
    void saveSettings() {
        Entity* w = menuStickWall.empty() ? nullptr : wallById(menuStickWall);
        if (!w || !w->located) w = anyWall();
        if (w && menuOpen) {
            menuWall = w->uuid; menuSaved = true;
            V3 r = untransform(w->pose, v3(menu.pose.position));
            menuRel.position = {r.x, r.y, r.z}; menuRel.orientation = qmul(qinv(w->pose.orientation), menu.pose.orientation);
        }
        std::ofstream f(gDir + "/settings.json");
        f << "{\"locked\":" << (editLocked ? 1 : 0) << ",\"menuSaved\":" << (menuSaved ? 1 : 0) << ",\"menuWall\":\"" << menuWall << "\",\"menu\":["
          << menuRel.position.x << "," << menuRel.position.y << "," << menuRel.position.z << "," << menuRel.orientation.x << ","
          << menuRel.orientation.y << "," << menuRel.orientation.z << "," << menuRel.orientation.w << "]}";
    }
    void loadSettings() {
        std::ifstream f(gDir + "/settings.json"); if (!f) return;
        std::stringstream ss; ss << f.rdbuf(); std::string j = ss.str();
        editLocked = j.find("\"locked\":0") == std::string::npos;
        menuSaved = j.find("\"menuSaved\":1") != std::string::npos;
        size_t a = j.find("\"menuWall\":\""); if (a != std::string::npos) { a += 12; menuWall = j.substr(a, j.find('"', a) - a); }
        float v[7] = {0, 0, 0, 0, 0, 0, 1}; size_t b = j.find("\"menu\":[");
        menuStickWall = menuWall;
        if (b != std::string::npos) sscanf(j.c_str() + b + 8, "%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
        menuRel.position = {v[0], v[1], v[2]}; menuRel.orientation = {v[3], v[4], v[5], v[6]};
        event(std::string("settings loaded, locked ") + (editLocked ? "on" : "off"));
    }
    void choose(int i) {
        std::string l = menuItems[i].label;
        auto* w = placed.active ? wallById(placed.wall) : nullptr;
        if (l == "MINI") launcher.launch(hostFor(0));
        else if (l == "LAPTOP") launcher.launch(hostFor(1));
        else if (w && (l == "SMALL" || l == "MEDIUM" || l == "LARGE")) {
            resizeTo(l == "SMALL" ? 1.0f : l == "MEDIUM" ? 1.6f : 2.4f, *w); validate(placed, *w); savePlacement();
        } else if (l == "LOCK") editLocked = !editLocked;
        else if (l == "SPELL") { spellOn = !spellOn; spell.clear(); }
        else if (l == "CLOSE" && !editLocked) menuOpen = false;
        else if (l == "QUAD") panelLayer.enabled = !panelLayer.enabled;     // A/B the compositor quad layer
        else if (l == "MARGIN") panelLayer.margin = !panelLayer.margin;  // transparent margin around the layer image
        else if (l == "SUPER") panelLayer.super = !panelLayer.super;     // compositor super-sampling for the layer
        else if (l == "BEZEL") bezel = !bezel;                            // dark frame over the layer edge, drawn in the eye buffers
        else if (l == "FADE") bezelFade = !bezelFade;                     // bezel styles; precedence AMBI > GLASS > FADE > grey
        else if (l == "GLASS") bezelGlass = !bezelGlass;
        else if (l == "AMBI") bezelAmbi = !bezelAmbi;
        else if (l == "CURVE") {  // curved panel (with QUAD), centred on where the user stands now
            panelLayer.curve = !panelLayer.curve;
            if (auto* w = placed.active ? wallById(placed.wall) : nullptr)
                panelLayer.radius = std::fmin(4.0f, std::fmax(1.0f, len(v3(head.position) - xform(w->pose, {placed.x, placed.y, 0}))));
        }
        if (l == "LOCK") saveSettings();
        int on = itemOn(l);
        event("settings: " + l + (on < 0 || l == "MINI" || l == "LAPTOP" ? "" : on ? " on" : " off"));
    }
    // Returns true when the settings panel takes the right hand this frame.
    bool updateMenu() {
        if (!settingsLoaded) { settingsLoaded = true; loadSettings(); loadScreens(); }
        // Bring the saved panel back once its wall is located (the scene loads a moment after start).
        if (menuSaved && !menuRestored) if (auto* w = wallById(menuWall)) if (w->located) {
            menuRestored = true;
            V3 c = xform(w->pose, v3(menuRel.position));
            menu.pose.position = {c.x, c.y, c.z}; menu.pose.orientation = qmul(w->pose.orientation, menuRel.orientation);
            menuOpen = true;
        }
        V3 palm;
        if (palmFacing(palm)) {
            if (palmArmed && palmHold == 0) palmStart = std::chrono::steady_clock::now();
            if (palmArmed) palmHold = std::fmax(std::chrono::duration<float>(std::chrono::steady_clock::now() - palmStart).count() / kPalmHold, 1e-3f);
            if (palmArmed && palmHold >= 1) {
                palmArmed = false; palmHold = 0;
                if (editLocked && menuSaved) event("settings pinned: unlock to move");
                else if (!menuOpen) openMenu();  // the grab bar moves it; the gesture only brings back a closed panel
            }
        } else { palmArmed = true; palmHold = 0; }
        menuHover = -1; menuBarHover = false;
        layoutMenu();
        if (!menuOpen || !menuSeen) return false;
        if (menuMoving) {
            if (handActive[1] && ray[1].valid) {
                float hx, hy; Entity* w = rayWall(ray[1].o, ray[1].d, hx, hy);
                if (w) {  // flat on the wall, the bar under the hand
                    V3 c = xform(w->pose, {hx, hy - menuBarY(), 0.03f});
                    menu.pose.position = {c.x, c.y, c.z}; menu.pose.orientation = w->pose.orientation; menuStickWall = w->uuid;
                } else {  // no wall: float 1.2 m out, facing you
                    V3 c = ray[1].o + ray[1].d * 1.2f, f = norm(c - v3(head.position)); f.y = 0; f = norm(f);
                    float yaw = std::atan2(-f.x, -f.z); c.y -= menuBarY();
                    menu.pose.position = {c.x, c.y, c.z}; menu.pose.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}; menuStickWall.clear();
                }
            }
            if (!(handActive[1] && ray[1].pinch)) { menuMoving = false; saveSettings(); event("settings moved" + std::string(menuStickWall.empty() ? "" : " onto wall " + menuStickWall)); }
            return true;
        }
        if (!editLocked && len(v3(head.position) - v3(menu.pose.position)) > 3.0f) { menuOpen = false; return false; }  // walked away
        if (!(handActive[1] && ray[1].valid)) return false;
        V3 lo = untransform(menu.pose, ray[1].o), ld = rot(qinv(menu.pose.orientation), ray[1].d);
        if (std::fabs(ld.z) < 1e-4f) return false;
        float t = -lo.z / ld.z; if (t <= 0) return false;
        float x = lo.x + ld.x * t, y = lo.y + ld.y * t;
        float hw = menuHalfW(), hh = menuHalfH();
        if (std::fabs(x) > hw || std::fabs(y) > hh) return false;
        if (!editLocked && std::fabs(x) < 0.18f && std::fabs(y - menuBarY()) < 0.035f) {
            menuBarHover = true;
            if (ray[1].pinch && !pinchPrev[1]) menuBarStart = std::chrono::steady_clock::now();
            if (ray[1].pinch) {
                menuBarHold = std::chrono::duration<float>(std::chrono::steady_clock::now() - menuBarStart).count() / kHoldToMove;
                if (menuBarHold >= 1) { menuMoving = true; menuBarHold = 0; event("settings move started"); }
            } else menuBarHold = 0;
            return true;
        }
        menuBarHold = 0;
        int cell = menuGrid.hit(x, y);
        if (cell >= 0) menuHover = menuShown[cell];
        if (menuHover >= 0 && ray[1].pinch && !pinchPrev[1]) choose(menuHover);
        return true;
    }
    int cornerHover = -1, cornerDrag = -1; float anchorX = 0, anchorY = 0;
    void resizeTo(float w, const Entity& wall) {
        float aspect = placed.h / placed.w, m = 0.03f;
        w = std::fmax(w, kMinW);
        w = std::fmin(w, wall.r2.extent.width - 2 * m);
        w = std::fmin(w, (wall.r2.extent.height - 2 * m) / aspect);
        placed.w = w; placed.h = w * aspect;
    }
    // Pointer on the placed panel: hand ray -> stream pixel, pinch = left button (hold to drag), like the TV remote.
    struct Pointer { bool on = false, pressed = false; float u = 0.5f, v = 0.5f; int sentX = -1, sentY = -1; long moves = 0, clicks = 0; } pointer;
    long aimFrames = 0, hitFrames = 0, pinchEdges = 0; V3 lastAimO{0, 0, 0}, lastAimD{0, 0, 0}; float lastNearest = -1; std::string lastNearestWall;
    long frames = 0, framesSec = 0, fps = 0;
    XrPosef head{{0, 0, 0, 1}, {0, 0, 0}};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now(), lastStatus = start;

    void error(const std::string& s) { LOGI("ERROR %s", s.c_str()); if (errors.size() < 40) errors.push_back(s); }
    void event(const std::string& s) {
        LOGI("%s", s.c_str());
        std::ofstream f(gDir + "/events.log", std::ios::app);
        f << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " " << s << "\n";
    }
    bool ok(XrResult r, const char* what) {
        if (XR_SUCCEEDED(r)) return true;
        char b[160]; snprintf(b, sizeof b, "%s failed: %d", what, (int)r); error(b); return false;
    }
    template <class T> void load(const char* name, T& fn) {
        if (XR_FAILED(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn)))) fn = nullptr;
    }
    bool has(const char* e) const { return enabledExt.count(e) > 0; }

    bool createInstance(JNIEnv*) {
        PFN_xrInitializeLoaderKHR init = nullptr;
        xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&init));
        if (init) {
            XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
            li.applicationVM = gVm; li.applicationContext = gActivity;
            init(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&li));
        }
        uint32_t n = 0; xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
        const char* wanted[] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
            XR_FB_PASSTHROUGH_EXTENSION_NAME, XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME,
            XR_FB_SPATIAL_ENTITY_EXTENSION_NAME, XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME, XR_FB_SPATIAL_ENTITY_STORAGE_EXTENSION_NAME,
            XR_FB_SPATIAL_ENTITY_CONTAINER_EXTENSION_NAME, XR_FB_SCENE_EXTENSION_NAME, XR_FB_SCENE_CAPTURE_EXTENSION_NAME,
            XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME,
            XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME};
        std::vector<const char*> enable;
        for (auto w : wanted) for (auto& p : props) if (!strcmp(p.extensionName, w)) { enable.push_back(w); enabledExt.insert(w); }
        XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        android.applicationVM = gVm; android.applicationActivity = gActivity;
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        ci.next = &android;
        strcpy(ci.applicationInfo.applicationName, "PinPin Wall Screen Room Lab");
        ci.applicationInfo.applicationVersion = 1;
        strcpy(ci.applicationInfo.engineName, "pinpin");
        ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
        ci.enabledExtensionCount = (uint32_t)enable.size(); ci.enabledExtensionNames = enable.data();
        if (!ok(xrCreateInstance(&ci, &instance), "xrCreateInstance")) return false;
        XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO}; sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (!ok(xrGetSystem(instance, &sg, &system), "xrGetSystem")) return false;
        panelLayer.superSample = has(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
        panelLayer.cylinder = has(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
        if (has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) { load("xrRequestDisplayRefreshRateFB", xrRequestDisplayRefreshRateFB_); load("xrGetDisplayRefreshRateFB", xrGetDisplayRefreshRateFB_); }
        if (has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME)) load("xrPerfSettingsSetPerformanceLevelEXT", xrPerfSettingsSetPerformanceLevelEXT_);
        load("xrCreatePassthroughFB", xrCreatePassthroughFB_); load("xrCreatePassthroughLayerFB", xrCreatePassthroughLayerFB_);
        load("xrCreateHandTrackerEXT", xrCreateHandTrackerEXT_); load("xrLocateHandJointsEXT", xrLocateHandJointsEXT_);
        load("xrQuerySpacesFB", xrQuerySpacesFB_); load("xrRetrieveSpaceQueryResultsFB", xrRetrieveSpaceQueryResultsFB_);
        load("xrGetSpaceComponentStatusFB", xrGetSpaceComponentStatusFB_); load("xrSetSpaceComponentStatusFB", xrSetSpaceComponentStatusFB_);
        load("xrGetSpaceBoundingBox2DFB", xrGetSpaceBoundingBox2DFB_); load("xrGetSpaceBoundingBox3DFB", xrGetSpaceBoundingBox3DFB_);
        load("xrGetSpaceSemanticLabelsFB", xrGetSpaceSemanticLabelsFB_); load("xrRequestSceneCaptureFB", xrRequestSceneCaptureFB_);
        load("xrCreateEnvironmentDepthProviderMETA", xrCreateEnvironmentDepthProviderMETA_);
        load("xrStartEnvironmentDepthProviderMETA", xrStartEnvironmentDepthProviderMETA_);
        load("xrCreateEnvironmentDepthSwapchainMETA", xrCreateEnvironmentDepthSwapchainMETA_);
        load("xrGetEnvironmentDepthSwapchainStateMETA", xrGetEnvironmentDepthSwapchainStateMETA_);
        load("xrEnumerateEnvironmentDepthSwapchainImagesMETA", xrEnumerateEnvironmentDepthSwapchainImagesMETA_);
        load("xrAcquireEnvironmentDepthImageMETA", xrAcquireEnvironmentDepthImageMETA_);
        event("instance created with " + std::to_string(enable.size()) + " extensions");
        return true;
    }

    bool createEgl() {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(display, nullptr, nullptr);
        const EGLint attrs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0,
            EGL_RENDERABLE_TYPE, 0x40, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
        EGLint n = 0;
        if (!eglChooseConfig(display, attrs, &config, 1, &n) || n < 1) { error("eglChooseConfig"); return false; }
        const EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx);
        const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, pb);
        if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) { error("egl context"); return false; }
        return true;
    }

    GLuint compile(GLenum type, const char* src) {
        GLuint s = glCreateShader(type); glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
        GLint okc = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &okc);
        if (!okc) { char log[1024]; glGetShaderInfoLog(s, sizeof log, nullptr, log); error(std::string("shader: ") + log); }
        return s;
    }
    void createGl() {
        const char* vs = R"(#version 300 es
layout(location=0) in vec3 aPos; layout(location=1) in vec4 aCol; layout(location=2) in vec2 aUv;
uniform mat4 uViewProj; uniform mat4 uDepthViewProj;
out vec4 vCol; out vec2 vUv; out vec4 vDepthClip;
void main(){ vCol=aCol; vUv=aUv; vDepthClip=uDepthViewProj*vec4(aPos,1.0); gl_Position=uViewProj*vec4(aPos,1.0); })";
        const char* fs = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float; precision highp sampler2DArray;
uniform sampler2D uVideo; uniform int uHasVideo;
in vec4 vCol; in vec2 vUv; in vec4 vDepthClip;
uniform sampler2DArray uDepth; uniform int uOcclude; uniform float uLayer; uniform vec3 uDepthParams;
out vec4 oColor;
void main(){
  vec4 c=vCol;
  if(vUv.x<-5.0){  // AMBI bezel: colour from the screen's nearest edge (blurred by a high mip), alpha from the vertex
    vec3 e=uHasVideo>0 ? textureLod(uVideo,clamp(vec2(vUv.x+10.0,1.0-vUv.y),0.002,0.998),6.0).rgb : vec3(0.06);
    c=vec4(e*0.85,vCol.a);
  }
  if(vUv.x>=0.0){
    if(uHasVideo==2) discard;  // QUAD: the compositor quad layer underneath shows the panel
    vec2 g=fract(vUv*vec2(16.0,10.0)); float grid=step(0.965,max(g.x,g.y));
    vec3 base=mix(vec3(0.05,0.08,0.16),vec3(0.16,0.30,0.52),vUv.y);
    c=vec4(mix(base,vec3(0.8,0.88,1.0),grid*0.5),0.94);
    float e=min(min(vUv.x,1.0-vUv.x),min(vUv.y,1.0-vUv.y)); if(e<0.01) c=vec4(0.9,0.95,1.0,1.0);
    if(uHasVideo==1){
      // Slightly sharper mip choice plus contrast-adaptive sharpening over the pixel footprint (CAS-style):
      // neighbours one screen pixel apart, boost the centre against their mean, clamped to their range.
      vec2 uv=vec2(vUv.x,1.0-vUv.y); vec2 dx=dFdx(uv), dy=dFdy(uv);
      vec3 m=texture(uVideo,uv,-0.4).rgb;
      vec3 a=texture(uVideo,uv+dx,-0.4).rgb, b=texture(uVideo,uv-dx,-0.4).rgb, e=texture(uVideo,uv+dy,-0.4).rgb, f=texture(uVideo,uv-dy,-0.4).rgb;
      vec3 lo=min(m,min(min(a,b),min(e,f))), hi=max(m,max(max(a,b),max(e,f)));
      vec3 amt=clamp(min(lo,1.0-hi)/max(hi,1e-3),0.0,1.0); amt=sqrt(amt)*0.6;
      vec3 s=clamp(m+(m-(a+b+e+f)*0.25)*amt*2.0,lo,hi);
      c=vec4(s,1.0);
    }
  }
  if(uOcclude==1 && vDepthClip.w>0.0){
    vec2 uv=(vDepthClip.xy/vDepthClip.w)*0.5+0.5;
    if(all(greaterThan(uv,vec2(0.0))) && all(lessThan(uv,vec2(1.0)))){
      float d=texture(uDepth,vec3(uv,uLayer)).r; float n=uDepthParams.x, f=uDepthParams.y; float zn=d*2.0-1.0;
      float env = f<=0.0 ? 2.0*n/max(1.0-zn,1e-6) : 2.0*f*n/((f+n)-zn*(f-n));
      c.a*=1.0-clamp((vDepthClip.w-env-uDepthParams.z)/0.04,0.0,1.0);
    }
  }
  if(c.a<0.01) discard;
  oColor=c;
})";
        program = glCreateProgram();
        glAttachShader(program, compile(GL_VERTEX_SHADER, vs)); glAttachShader(program, compile(GL_FRAGMENT_SHADER, fs));
        glLinkProgram(program);
        GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) { char log[1024]; glGetProgramInfoLog(program, sizeof log, nullptr, log); error(std::string("link: ") + log); }
        uViewProj = glGetUniformLocation(program, "uViewProj"); uDepthViewProj = glGetUniformLocation(program, "uDepthViewProj");
        uDepth = glGetUniformLocation(program, "uDepth"); uOcclude = glGetUniformLocation(program, "uOcclude");
        uLayer = glGetUniformLocation(program, "uLayer"); uDepthParams = glGetUniformLocation(program, "uDepthParams");
        uVideo = glGetUniformLocation(program, "uVideo"); uHasVideo = glGetUniformLocation(program, "uHasVideo");
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
        glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, p));
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, c));
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, uv));
        glBindVertexArray(0);
    }

    bool createSession() {
        PFN_xrGetOpenGLESGraphicsRequirementsKHR req = nullptr; load("xrGetOpenGLESGraphicsRequirementsKHR", req);
        XrGraphicsRequirementsOpenGLESKHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        if (req) req(instance, system, &gr);
        XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        gb.display = display; gb.config = config; gb.context = context;
        XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO}; si.next = &gb; si.systemId = system;
        if (!ok(xrCreateSession(instance, &si, &session), "xrCreateSession")) return false;
        XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; rs.poseInReferenceSpace.orientation.w = 1;
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; ok(xrCreateReferenceSpace(session, &rs, &local), "local space");
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; ok(xrCreateReferenceSpace(session, &rs, &view), "view space");

        uint32_t vc = 0; XrViewConfigurationView cv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &vc, cv);
        uint32_t fc = 0; xrEnumerateSwapchainFormats(session, 0, &fc, nullptr);
        std::vector<int64_t> formats(fc); xrEnumerateSwapchainFormats(session, fc, &fc, formats.data());
        int64_t format = GL_RGBA8;
        for (auto f : formats) if (f == GL_SRGB8_ALPHA8) format = f;
        for (int i = 0; i < 2; i++) {
            Eye& e = eyes[i];
            e.w = (int)std::fmin(cv[i].recommendedImageRectWidth * 1.3f, (float)cv[i].maxImageRectWidth);
            e.h = (int)std::fmin(cv[i].recommendedImageRectHeight * 1.3f, (float)cv[i].maxImageRectHeight);
            XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            sc.format = format; sc.sampleCount = 1; sc.width = e.w; sc.height = e.h; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
            if (!ok(xrCreateSwapchain(session, &sc, &e.chain), "xrCreateSwapchain")) return false;
            uint32_t ic = 0; xrEnumerateSwapchainImages(e.chain, 0, &ic, nullptr);
            std::vector<XrSwapchainImageOpenGLESKHR> imgs(ic, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
            xrEnumerateSwapchainImages(e.chain, ic, &ic, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
            // 4x MSAA resolved on-tile (EXT_multisampled_render_to_texture), falling back to plain targets.
            auto rbMs = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
            auto texMs = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
            msaa = (rbMs && texMs) ? 4 : 0;
            glGenRenderbuffers(1, &e.depth); glBindRenderbuffer(GL_RENDERBUFFER, e.depth);
            if (msaa) rbMs(GL_RENDERBUFFER, msaa, GL_DEPTH_COMPONENT24, e.w, e.h);
            else glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, e.w, e.h);
            e.fbo.resize(ic); glGenFramebuffers(ic, e.fbo.data());
            for (uint32_t k = 0; k < ic; k++) {
                glBindFramebuffer(GL_FRAMEBUFFER, e.fbo[k]);
                if (msaa) texMs(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, imgs[k].image, 0, msaa);
                else glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, imgs[k].image, 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, e.depth);
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        char b[128]; snprintf(b, sizeof b, "session created, eye %dx%d msaa %d format 0x%llx", eyes[0].w, eyes[0].h, msaa, (long long)format); event(b);

        if (xrCreatePassthroughFB_ && xrCreatePassthroughLayerFB_) {
            XrPassthroughCreateInfoFB pc{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB}; pc.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
            if (ok(xrCreatePassthroughFB_(session, &pc, &passthrough), "xrCreatePassthroughFB")) {
                XrPassthroughLayerCreateInfoFB lc{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
                lc.passthrough = passthrough; lc.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
                lc.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
                ok(xrCreatePassthroughLayerFB_(session, &lc, &ptLayer), "xrCreatePassthroughLayerFB");
            }
        }
        if (xrCreateHandTrackerEXT_) for (int h = 0; h < 2; h++) {
            XrHandTrackerCreateInfoEXT hc{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
            hc.hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT; hc.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            ok(xrCreateHandTrackerEXT_(session, &hc, &hands[h]), "xrCreateHandTrackerEXT");
        }
        createDepth();
        return true;
    }

    void createDepth() {
        if (!xrCreateEnvironmentDepthProviderMETA_) { error("environment depth unavailable"); return; }
        XrEnvironmentDepthProviderCreateInfoMETA pc{XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META};
        if (!ok(xrCreateEnvironmentDepthProviderMETA_(session, &pc, &depthProvider), "create depth provider")) return;
        XrEnvironmentDepthSwapchainCreateInfoMETA sc{XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META};
        if (!ok(xrCreateEnvironmentDepthSwapchainMETA_(depthProvider, &sc, &depthChain), "create depth swapchain")) return;
        XrEnvironmentDepthSwapchainStateMETA st{XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META};
        xrGetEnvironmentDepthSwapchainStateMETA_(depthChain, &st); depthW = st.width; depthH = st.height;
        uint32_t n = 0; xrEnumerateEnvironmentDepthSwapchainImagesMETA_(depthChain, 0, &n, nullptr);
        depthImages.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateEnvironmentDepthSwapchainImagesMETA_(depthChain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(depthImages.data()));
        depthStarted = ok(xrStartEnvironmentDepthProviderMETA_(depthProvider), "start depth provider");
        char b[96]; snprintf(b, sizeof b, "depth provider %s, %ux%u, %u images", depthStarted ? "started" : "failed", depthW, depthH, n); event(b);
    }

    void querySpaces() {
        if (!xrQuerySpacesFB_) { sceneState = "unsupported"; return; }
        // Runtimes differ in which filter forms they accept; try each and record the outcome.
        XrSpaceStorageLocationFilterInfoFB storage{XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB, nullptr, XR_SPACE_STORAGE_LOCATION_LOCAL_FB};
        // Quest requires the storage location chained behind the component filter.
        XrSpaceComponentFilterInfoFB component{XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB, &storage, XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB};
        XrSpaceComponentFilterInfoFB bounded{XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB, &storage, XR_SPACE_COMPONENT_TYPE_SEMANTIC_LABELS_FB};
        std::pair<const char*, const XrSpaceFilterInfoBaseHeaderFB*> forms[] = {
            {"locatable+storage", reinterpret_cast<XrSpaceFilterInfoBaseHeaderFB*>(&component)},
            {"labels+storage", reinterpret_cast<XrSpaceFilterInfoBaseHeaderFB*>(&bounded)},
            {"storage", reinterpret_cast<XrSpaceFilterInfoBaseHeaderFB*>(&storage)}};
        sceneState = "query failed";
        for (auto& f : forms) {
            XrSpaceQueryInfoFB qi{XR_TYPE_SPACE_QUERY_INFO_FB, nullptr, XR_SPACE_QUERY_ACTION_LOAD_FB, 100, 0, f.second, nullptr};
            XrResult r = xrQuerySpacesFB_(session, reinterpret_cast<XrSpaceQueryInfoBaseHeaderFB*>(&qi), &queryId);
            event(std::string("query form ") + f.first + " -> " + std::to_string((int)r));
            if (XR_SUCCEEDED(r)) { sceneState = std::string("querying:") + f.first; return; }
        }
    }

    bool componentEnabled(XrSpace s, XrSpaceComponentTypeFB t) {
        XrSpaceComponentStatusFB st{XR_TYPE_SPACE_COMPONENT_STATUS_FB};
        return XR_SUCCEEDED(xrGetSpaceComponentStatusFB_(s, t, &st)) && st.enabled;
    }

    void retrieveSpaces(XrAsyncRequestIdFB id) {
        XrSpaceQueryResultsFB res{XR_TYPE_SPACE_QUERY_RESULTS_FB};
        if (!ok(xrRetrieveSpaceQueryResultsFB_(session, id, &res), "retrieve query count")) return;
        std::vector<XrSpaceQueryResultFB> out(res.resultCountOutput);
        res.resultCapacityInput = (uint32_t)out.size(); res.results = out.data();
        if (!ok(xrRetrieveSpaceQueryResultsFB_(session, id, &res), "retrieve query results")) return;
        static const char* known = "TABLE,COUCH,FLOOR,CEILING,WALL_FACE,WINDOW_FRAME,DOOR_FRAME,STORAGE,BED,SCREEN,LAMP,PLANT,WALL_ART,INVISIBLE_WALL_FACE,GLOBAL_MESH,OTHER";
        for (auto& r : out) {
            Entity e; e.space = r.space; e.uuid = hexUuid(r.uuid);
            if (!componentEnabled(r.space, XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB)) {
                XrSpaceComponentStatusSetInfoFB si{XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB, nullptr, XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB, XR_TRUE, 0};
                XrAsyncRequestIdFB rid; xrSetSpaceComponentStatusFB_(r.space, &si, &rid);
            }
            if (componentEnabled(r.space, XR_SPACE_COMPONENT_TYPE_SEMANTIC_LABELS_FB)) {
                XrSemanticLabelsSupportInfoFB sup{XR_TYPE_SEMANTIC_LABELS_SUPPORT_INFO_FB};
                sup.flags = XR_SEMANTIC_LABELS_SUPPORT_MULTIPLE_SEMANTIC_LABELS_BIT_FB | XR_SEMANTIC_LABELS_SUPPORT_ACCEPT_DESK_TO_TABLE_MIGRATION_BIT_FB |
                            XR_SEMANTIC_LABELS_SUPPORT_ACCEPT_INVISIBLE_WALL_FACE_BIT_FB;
                sup.recognizedLabels = known;
                XrSemanticLabelsFB lb{XR_TYPE_SEMANTIC_LABELS_FB}; lb.next = &sup;
                if (XR_SUCCEEDED(xrGetSpaceSemanticLabelsFB_(session, r.space, &lb)) && lb.bufferCountOutput) {
                    std::vector<char> buf(lb.bufferCountOutput); lb.bufferCapacityInput = (uint32_t)buf.size(); lb.buffer = buf.data();
                    if (XR_SUCCEEDED(xrGetSpaceSemanticLabelsFB_(session, r.space, &lb))) e.label.assign(buf.data(), strnlen(buf.data(), buf.size()));
                }
            }
            if (componentEnabled(r.space, XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB)) e.has2d = XR_SUCCEEDED(xrGetSpaceBoundingBox2DFB_(session, r.space, &e.r2));
            if (componentEnabled(r.space, XR_SPACE_COMPONENT_TYPE_BOUNDED_3D_FB)) e.has3d = XR_SUCCEEDED(xrGetSpaceBoundingBox3DFB_(session, r.space, &e.r3));
            entities.push_back(e);
        }
        event("scene query returned " + std::to_string(out.size()) + " spaces");
    }

    void loadPlacement() {
        std::ifstream f(gDir + "/placement.json"); if (!f) return;
        std::string s((std::istreambuf_iterator<char>(f)), {});
        char wall[64] = {}; float x, y, w, h;
        if (sscanf(s.c_str(), "{\"wall\":\"%63[0-9a-f]\",\"x\":%f,\"y\":%f,\"w\":%f,\"h\":%f", wall, &x, &y, &w, &h) == 5) {
            placed.active = true; placed.wall = wall; placed.x = x; placed.y = y; placed.w = w; placed.h = h; placed.valid = true;
            event(std::string("restored placement on wall ") + wall);
        }
    }
    void savePlacement() {
        std::ofstream f(gDir + "/placement.json");
        f << "{\"wall\":\"" << placed.wall << "\",\"x\":" << placed.x << ",\"y\":" << placed.y << ",\"w\":" << placed.w << ",\"h\":" << placed.h << "}";
    }

    void pollEvents() {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                state = sc->state; event("session state " + std::to_string((int)state));
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO}; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = ok(xrBeginSession(session, &bi), "xrBeginSession");
                } else if ((state == XR_SESSION_STATE_VISIBLE || state == XR_SESSION_STATE_FOCUSED) && entities.empty() && (sceneState == "idle" || sceneState == "query failed")) {
                    {
                        if (gRequestCapture && xrRequestSceneCaptureFB_) {
                            XrSceneCaptureRequestInfoFB ci{XR_TYPE_SCENE_CAPTURE_REQUEST_INFO_FB}; XrAsyncRequestIdFB rid;
                            gRequestCapture = false;  // one scan per launch; returning from setup must not re-request
                            if (ok(xrRequestSceneCaptureFB_(session, &ci, &rid), "xrRequestSceneCaptureFB")) sceneState = "capturing";
                        } else querySpaces();
                    }
                } else if (state == XR_SESSION_STATE_STOPPING) { xrEndSession(session); running = false; }
                else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) gQuit = true;
            } else if (ev.type == XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB) {
                retrieveSpaces(reinterpret_cast<XrEventDataSpaceQueryResultsAvailableFB*>(&ev)->requestId);
            } else if (ev.type == XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB) {
                sceneState = "loaded"; event("scene query complete");
            } else if (ev.type == XR_TYPE_EVENT_DATA_SCENE_CAPTURE_COMPLETE_FB) {
                event("scene capture complete"); entities.clear(); querySpaces();
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) gQuit = true;
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    // Wall-local rect of anything, with z range, for obstruction tests.
    struct Box { float x0, x1, y0, y1, z0, z1; };
    static Box boxOf(const XrPosef& wall, const std::vector<V3>& world) {
        Box b{1e9f, -1e9f, 1e9f, -1e9f, 1e9f, -1e9f};
        for (auto& p : world) {
            V3 l = untransform(wall, p);
            b.x0 = std::fmin(b.x0, l.x); b.x1 = std::fmax(b.x1, l.x); b.y0 = std::fmin(b.y0, l.y); b.y1 = std::fmax(b.y1, l.y);
            b.z0 = std::fmin(b.z0, l.z); b.z1 = std::fmax(b.z1, l.z);
        }
        return b;
    }
    static std::vector<V3> corners2d(const Entity& e) {
        float x0 = e.r2.offset.x, y0 = e.r2.offset.y, x1 = x0 + e.r2.extent.width, y1 = y0 + e.r2.extent.height;
        return {xform(e.pose, {x0, y0, 0}), xform(e.pose, {x1, y0, 0}), xform(e.pose, {x1, y1, 0}), xform(e.pose, {x0, y1, 0})};
    }
    static std::vector<V3> corners3d(const Entity& e) {
        std::vector<V3> c;
        for (int i = 0; i < 8; i++)
            c.push_back(xform(e.pose, {e.r3.offset.x + ((i & 1) ? e.r3.extent.width : 0), e.r3.offset.y + ((i & 2) ? e.r3.extent.height : 0),
                                       e.r3.offset.z + ((i & 4) ? e.r3.extent.depth : 0)}));
        return c;
    }

    // Fit a w x h panel on the wall around (hx, hy); reject if it leaves the wall or overlaps mapped objects.
    void validate(Placement& p, const Entity& wall) {
        float x0 = wall.r2.offset.x, y0 = wall.r2.offset.y, ww = wall.r2.extent.width, wh = wall.r2.extent.height;
        const float m = 0.03f;
        p.valid = true; p.reason = "";
        if (ww < p.w + 2 * m || wh < p.h + 2 * m) { p.valid = false; p.reason = "wall smaller than panel"; return; }
        p.x = std::fmin(std::fmax(p.x, x0 + m + p.w / 2), x0 + ww - m - p.w / 2);
        p.y = std::fmin(std::fmax(p.y, y0 + m + p.h / 2), y0 + wh - m - p.h / 2);
        float px0 = p.x - p.w / 2 - m, px1 = p.x + p.w / 2 + m, py0 = p.y - p.h / 2 - m, py1 = p.y + p.h / 2 + m;
        for (auto& e : entities) {
            if (&e == &wall || !e.located) continue;
            if (e.label.find("FLOOR") != std::string::npos || e.label.find("CEILING") != std::string::npos || isWall(e)) continue;
            Box b{};
            if (e.has3d) b = boxOf(wall.pose, corners3d(e));
            else if (e.has2d) b = boxOf(wall.pose, corners2d(e));
            else continue;
            bool near = b.z1 > -0.15f && b.z0 < 0.6f;
            if (near && b.x1 > px0 && b.x0 < px1 && b.y1 > py0 && b.y0 < py1) {
                p.valid = false; p.reason = "blocked by " + (e.label.empty() ? std::string("object") : e.label); return;
            }
        }
    }

    Entity* wallById(const std::string& id) { for (auto& e : entities) if (e.uuid == id) return &e; return nullptr; }

    void updateHands(XrTime t) {
        for (int h = 0; h < 2; h++) {
            handActive[h] = false;
            if (!hands[h]) continue;
            aim[h] = {XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
            XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
            if (has(XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME)) locs.next = &aim[h];
            locs.jointCount = XR_HAND_JOINT_COUNT_EXT; locs.jointLocations = joints[h];
            XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT}; li.baseSpace = local; li.time = t;
            XrResult r = xrLocateHandJointsEXT_(hands[h], &li, &locs); handResult[h] = (int)r;
            if (XR_SUCCEEDED(r)) handActive[h] = locs.isActive;
            HandRay& hr = ray[h]; hr.valid = false;
            if (!handActive[h]) { hr.pinch = false; continue; }
            auto& th = joints[h][XR_HAND_JOINT_THUMB_TIP_EXT]; auto& ix = joints[h][XR_HAND_JOINT_INDEX_TIP_EXT];
            auto& kn = joints[h][XR_HAND_JOINT_INDEX_PROXIMAL_EXT]; auto& tm = joints[h][XR_HAND_JOINT_THUMB_PROXIMAL_EXT];
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if (!(th.locationFlags & need) || !(ix.locationFlags & need) || !(kn.locationFlags & need) || !(tm.locationFlags & need)) { hr.pinch = false; continue; }
            hr.gap = len(v3(th.pose.position) - v3(ix.pose.position));
            hr.tip = (v3(th.pose.position) + v3(ix.pose.position)) * 0.5f;
            hr.pinchDown = hr.pinchDown ? hr.gap < 0.035f : hr.gap < 0.018f;
            // Thumb tip to the middle finger's proximal phalanx (segment knuckle -> middle joint).
            auto& mp = joints[h][XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT]; auto& mi = joints[h][XR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT];
            if ((mp.locationFlags & need) && (mi.locationFlags & need)) {
                V3 a = v3(mp.pose.position), b = v3(mi.pose.position), t = v3(th.pose.position), ab = b - a;
                float u = std::fmin(std::fmax(dot(t - a, ab) / std::fmax(dot(ab, ab), 1e-6f), 0.0f), 1.0f);
                hr.thumb = len(t - (a + ab * u));
                hr.thumbMin = std::fmin(hr.thumbMin, hr.thumb); hr.thumbMax = std::fmax(hr.thumbMax, hr.thumb);
                bool was = hr.thumbDown; hr.thumbDown = was ? hr.thumb < 0.038f : hr.thumb < 0.024f;
                if (hr.thumbDown && !was) hr.thumbPresses++;
            }
            bool pinchWas = hr.pinch;
            // Fist: all four fingertips curled to the palm. A fist rests the thumb on the fingers, so it never clicks.
            hr.fist = false;
            if (auto& pj = joints[h][XR_HAND_JOINT_PALM_EXT]; pj.locationFlags & need) {
                int curled = 0;
                for (int tip : {XR_HAND_JOINT_INDEX_TIP_EXT, XR_HAND_JOINT_MIDDLE_TIP_EXT, XR_HAND_JOINT_RING_TIP_EXT, XR_HAND_JOINT_LITTLE_TIP_EXT})
                    if ((joints[h][tip].locationFlags & need) && len(v3(joints[h][tip].pose.position) - v3(pj.pose.position)) < 0.07f) curled++;
                hr.fist = curled == 4;
            }
            if (hr.pinchDown && !pinchWas && !hr.thumbDown) hr.pinchPresses++;
            hr.pinch = !hr.fist && (hr.thumbDown || hr.pinchDown);
            V3 knuckles = (v3(kn.pose.position) + v3(mp.pose.position)) * 0.5f;
            V3 shoulder = xform(head, {h == 1 ? 0.17f : -0.17f, -0.2f, 0.05f});
            V3 ro = knuckles, rd = norm(knuckles - shoulder);
            hr.valid = len(knuckles - shoulder) > 0.15f;
            int slot = hr.histN % kHist; hr.histO[slot] = ro; hr.histD[slot] = rd; hr.histN++;
            if (hr.pinch && !pinchWas) {
                int back = std::min(kRewind, hr.histN - 1), i = (hr.histN - 1 - back) % kHist;
                hr.locked = true; hr.lockO = hr.histO[i]; hr.lockD = hr.histD[i];
            }
            if (hr.locked && hr.pinch && dot(rd, hr.lockD) < std::cos(kLockDeg * 0.0174533f)) { hr.locked = false; hr.lockBreaks++; }
            // Hold the lock through the release frame so the button-up lands where the click started.
            if (hr.locked && !hr.pinch && !pinchWas) hr.locked = false;
            hr.o = hr.locked ? hr.lockO : ro; hr.d = hr.locked ? hr.lockD : rd;
        }
    }

    // Notifications: short head-locked messages stacked in the top-right corner of the view. A toast has a key so
    // it can be replaced or cleared; seconds = 0 keeps it until cleared.
    struct Toast { std::string key, text; float col[4]; std::chrono::steady_clock::time_point until; bool sticky; };
    std::vector<Toast> toasts;
    void toast(const std::string& key, const std::string& text, const float* col, float seconds) {
        Toast t{key, text, {col[0], col[1], col[2], col[3]}, std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000)), seconds <= 0};
        for (auto& x : toasts) if (x.key == key) { x = t; return; }
        toasts.push_back(t); event("notify: " + text);
    }
    void clearToast(const std::string& key) { toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const Toast& t) { return t.key == key; }), toasts.end()); }
    void drawToasts(Batch& b) {
        auto now = std::chrono::steady_clock::now();
        toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const Toast& t) { return !t.sticky && now > t.until; }), toasts.end());
        const float px = 0.0022f, h = 0.026f, right = 0.27f, top = 0.17f, z = -0.7f;
        for (size_t k = 0; k < toasts.size(); k++) {
            const Toast& t = toasts[k];
            float w = UiGrid::textW(t.text, px) + 0.03f, cy = top - k * (h + 0.006f);
            Entity hud; hud.pose = head;
            auto P = [&](float x, float y, float dz) { return xform(head, {x, y, z + dz}); };
            const float bg[4] = {0.05f, 0.06f, 0.08f, 0.82f}, white[4] = {0.97f, 0.97f, 1.0f, 1.0f};
            b.quad(P(right - w, cy - h / 2, 0), P(right, cy - h / 2, 0), P(right, cy + h / 2, 0), P(right - w, cy + h / 2, 0), bg);
            b.quad(P(right - w, cy - h / 2, 0.0005f), P(right - w + 0.006f, cy - h / 2, 0.0005f), P(right - w + 0.006f, cy + h / 2, 0.0005f), P(right - w, cy + h / 2, 0.0005f), t.col);
            text(b, hud, right - w / 2 + 0.003f, cy, z + 0.001f, px, t.text.c_str(), white);
        }
    }
    // Battery: polled from Android's BatteryManager (the app can't read /sys) every 10 s on the render thread.
    JNIEnv* jni = nullptr; int batteryPct = -1; bool charging = false; std::chrono::steady_clock::time_point batteryAt{};
    void pollBattery() {
        auto now = std::chrono::steady_clock::now();
        if (!jni || !gActivity || now - batteryAt < std::chrono::seconds(10)) return;
        batteryAt = now;
        JNIEnv* e = jni;
        jclass ac = e->GetObjectClass(gActivity);
        jmethodID gss = e->GetMethodID(ac, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        jstring name = e->NewStringUTF("batterymanager");
        jobject bm = gss ? e->CallObjectMethod(gActivity, gss, name) : nullptr;
        if (bm && !e->ExceptionCheck()) {
            jclass bc = e->GetObjectClass(bm);
            jmethodID gip = e->GetMethodID(bc, "getIntProperty", "(I)I"), isc = e->GetMethodID(bc, "isCharging", "()Z");
            bool was = charging;
            if (gip) batteryPct = e->CallIntMethod(bm, gip, 4);  // BATTERY_PROPERTY_CAPACITY
            if (isc) charging = e->CallBooleanMethod(bm, isc);
            e->DeleteLocalRef(bc);
            const float red[4] = {1.0f, 0.25f, 0.2f, 1}, green[4] = {0.25f, 0.85f, 0.4f, 1};
            if (batteryPct >= 0 && batteryPct < 10 && !charging) toast("battery", "BATTERY " + std::to_string(batteryPct) + "% - CONNECT CHARGER", red, 0);
            else clearToast("battery");
            if (charging && !was && batteryPct >= 0) toast("charging", "CHARGING " + std::to_string(batteryPct) + "%", green, 4);
        }
        if (e->ExceptionCheck()) e->ExceptionClear();
        if (bm) e->DeleteLocalRef(bm);
        e->DeleteLocalRef(name); e->DeleteLocalRef(ac);
    }
    // ---- Summoning spell and extra screens
    // Point with the right index finger and draw a big circle in the air (spell.h). Clockwise summons a new screen
    // where the circle was, sized to fill it as seen on the wall behind; counterclockwise cancels a move in
    // progress, or dismisses the extra screen it was drawn over. A glowing trail shows the circle forming and a
    // burst marks the result. Extra screens are remembered (files/screens.json); each will be a real display the
    // Mac creates for it, and until that stream is wired they show as a placeholder.
    struct Screen { int id; Placement p; };
    std::vector<Screen> extras; int nextScreenId = 2;
    bool spellOn = true; SpellRecognizer spell; float spellCooldown = 0, spellProgress = 0; int spellTurn = 0;
    struct Burst { V3 c, ax, ay; float r; float col[4]; std::chrono::steady_clock::time_point t0; };
    std::vector<Burst> bursts;
    long spellsCw = 0, spellsCcw = 0;
    int extraBarHover = -1, extraMoving = -1; float extraHold = 0; std::chrono::steady_clock::time_point extraStart{};
    bool pointing(int h) {
        auto& pj = joints[h][XR_HAND_JOINT_PALM_EXT];
        auto far = [&](int j, float d) { return (joints[h][j].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && len(v3(joints[h][j].pose.position) - v3(pj.pose.position)) > d; };
        if (!(pj.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return false;
        return far(XR_HAND_JOINT_INDEX_TIP_EXT, 0.085f) && !far(XR_HAND_JOINT_MIDDLE_TIP_EXT, 0.075f) && !far(XR_HAND_JOINT_RING_TIP_EXT, 0.075f) && !far(XR_HAND_JOINT_LITTLE_TIP_EXT, 0.075f);
    }
    float now() const { return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count(); }
    void burst(V3 c, float r, const float* col) {
        V3 f = norm(v3(head.position) - c), ax = norm(cross({0, 1, 0}, f)), ay = cross(f, ax);
        bursts.push_back({c, ax, ay, r, {col[0], col[1], col[2], col[3]}, std::chrono::steady_clock::now()});
    }
    int extraAt(V3 o, V3 d, float& hx, float& hy) {
        Entity* w = rayWall(o, d, hx, hy); if (!w) return -1;
        for (size_t i = 0; i < extras.size(); i++) {
            const Placement& p = extras[i].p;
            if (p.wall == w->uuid && std::fabs(hx - p.x) < p.w / 2 && std::fabs(hy - p.y) < p.h / 2) return (int)i;
        }
        return -1;
    }
    void summon(V3 c, float r) {
        const float gold[4] = {1.0f, 0.8f, 0.25f, 1}, red[4] = {1.0f, 0.3f, 0.25f, 1};
        V3 eye = v3(head.position), d = norm(c - eye);
        float hx, hy; Entity* w = rayWall(eye, d, hx, hy);
        if (!w) { burst(c, r, red); toast("spell", "POINT THE CIRCLE AT A WALL", red, 3); return; }
        float distWall = len(xform(w->pose, {hx, hy, 0}) - eye), distHand = std::fmax(len(c - eye), 0.2f);
        Placement np; np.active = true; np.wall = w->uuid; np.x = hx; np.y = hy;
        np.w = std::fmin(std::fmax(0.85f * 2 * r * distWall / distHand, 0.6f), 3.0f); np.h = np.w * 0.625f;
        validate(np, *w);
        if (!np.valid) { burst(c, r, red); toast("spell", "NO ROOM HERE: " + np.reason, red, 3); return; }
        int id = nextScreenId++;
        extras.push_back({id, np}); saveScreens();
        burst(c, r, gold); toast("spell", "SCREEN " + std::to_string(id) + " SUMMONED", gold, 3);
        event("summoned screen " + std::to_string(id) + " on wall " + np.wall + " w " + std::to_string(np.w));
    }
    void counterSpell(V3 c, float r) {
        const float violet[4] = {0.75f, 0.45f, 1.0f, 1};
        if (moving || menuMoving || extraMoving >= 0) {
            moving = false; candidate.active = false; extraMoving = -1;
            if (menuMoving) { menuMoving = false; menuRestored = false; }  // the saved pose comes back next frame
            burst(c, r, violet); toast("spell", "CANCELLED", violet, 2); event("counter-spell: cancelled move"); return;
        }
        V3 eye = v3(head.position); float hx, hy;
        int i = extraAt(eye, norm(c - eye), hx, hy);
        if (i >= 0) {
            int id = extras[i].id; extras.erase(extras.begin() + i); saveScreens();
            burst(c, r, violet); toast("spell", "SCREEN " + std::to_string(id) + " DISMISSED", violet, 3); event("dismissed screen " + std::to_string(id)); return;
        }
        burst(c, r, violet); toast("spell", "CANCELLED", violet, 2);
    }
    void updateSpell() {
        float t = now(); spellProgress = 0; spellTurn = 0;
        if (spellCooldown > 0) spellCooldown -= 1.0f / 72;
        if (!spellOn || !handActive[1] || !pointing(1) || dictation.recording) { spell.clear(); return; }
        V3 tip = v3(joints[1][XR_HAND_JOINT_INDEX_TIP_EXT].pose.position);
        spell.add({tip.x, tip.y, tip.z}, t);
        V3 rt = rot(head.orientation, {1, 0, 0}), up = rot(head.orientation, {0, 1, 0});
        auto r = spell.analyze({rt.x, rt.y, rt.z}, {up.x, up.y, up.z});
        spellProgress = r.progress; spellTurn = r.turn;
        if (!r.found || spellCooldown > 0) return;
        spell.clear(); spellCooldown = 0.8f;
        V3 c{r.centre.x, r.centre.y, r.centre.z};
        if (r.clockwise) { spellsCw++; summon(c, r.radius); } else { spellsCcw++; counterSpell(c, r.radius); }
    }
    // Moving an extra screen: the same hold-to-grab bar as the main screen, shown while unlocked.
    bool updateExtras() {
        extraBarHover = -1;
        bool p1 = handActive[1] && ray[1].pinch;
        if (extraMoving >= 0 && extraMoving < (int)extras.size()) {
            Placement& p = extras[extraMoving].p; float hx, hy;
            if (ray[1].valid) if (Entity* w = rayWall(ray[1].o, ray[1].d, hx, hy)) {
                candidate.active = true; candidate.wall = w->uuid; candidate.x = hx; candidate.y = hy + p.h / 2 + kBarGap; candidate.w = p.w; candidate.h = p.h;
                validate(candidate, *w);
            }
            if (!p1) {
                if (candidate.active && candidate.valid) { p.wall = candidate.wall; p.x = candidate.x; p.y = candidate.y; saveScreens(); event("moved screen " + std::to_string(extras[extraMoving].id)); }
                extraMoving = -1; candidate.active = false;
            }
            return true;
        }
        extraMoving = -1;
        if (editLocked || !(handActive[1] && ray[1].valid)) return false;
        float hx, hy; Entity* w = rayWall(ray[1].o, ray[1].d, hx, hy); if (!w) return false;
        for (size_t i = 0; i < extras.size(); i++) {
            const Placement& p = extras[i].p;
            if (p.wall != w->uuid) continue;
            if (std::fabs(hx - p.x) < kBarW / 2 + 0.03f && std::fabs(hy - (p.y - p.h / 2 - kBarGap)) < kBarH / 2 + 0.03f) { extraBarHover = (int)i; break; }
        }
        if (extraBarHover < 0) { extraHold = 0; return false; }
        if (p1 && !pinchPrev[1]) extraStart = std::chrono::steady_clock::now();
        extraHold = p1 ? std::chrono::duration<float>(std::chrono::steady_clock::now() - extraStart).count() / kHoldToMove : 0;
        if (extraHold >= 1) { extraMoving = extraBarHover; extraHold = 0; event("moving screen " + std::to_string(extras[extraMoving].id)); }
        return true;
    }
    void saveScreens() {
        std::ofstream f(gDir + "/screens.json");
        f << "{\"next\":" << nextScreenId << ",\"screens\":[";
        for (size_t i = 0; i < extras.size(); i++) {
            const Placement& p = extras[i].p;
            f << (i ? "," : "") << "{\"id\":" << extras[i].id << ",\"wall\":\"" << p.wall << "\",\"x\":" << p.x << ",\"y\":" << p.y << ",\"w\":" << p.w << ",\"h\":" << p.h << "}";
        }
        f << "]}";
    }
    void loadScreens() {
        std::ifstream f(gDir + "/screens.json"); if (!f) return;
        std::string j((std::istreambuf_iterator<char>(f)), {});
        sscanf(j.c_str(), "{\"next\":%d", &nextScreenId);
        for (size_t at = j.find("{\"id\":"); at != std::string::npos; at = j.find("{\"id\":", at + 1)) {
            Screen sc{}; char wall[64] = {};
            if (sscanf(j.c_str() + at, "{\"id\":%d,\"wall\":\"%63[0-9a-f]\",\"x\":%f,\"y\":%f,\"w\":%f,\"h\":%f", &sc.id, wall, &sc.p.x, &sc.p.y, &sc.p.w, &sc.p.h) == 6) {
                sc.p.active = true; sc.p.valid = true; sc.p.wall = wall; extras.push_back(sc);
            }
        }
        event("restored " + std::to_string(extras.size()) + " extra screens");
    }
    void drawSpell(Batch& b) {
        // Trail: the fingertip path glows once a quarter turn is in, warming from cyan to gold toward a full circle.
        if (spellOn && spellProgress > 0.22f && spell.pts.size() > 2) {
            float t = now(), pr = std::fmin((spellProgress - 0.22f) / 0.78f, 1.0f);
            const float cyan[3] = {0.35f, 0.85f, 1.0f}, gold[3] = {1.0f, 0.8f, 0.25f}, violet[3] = {0.75f, 0.45f, 1.0f};
            const float* to = spellTurn > 0 ? violet : gold;  // counterclockwise reads as the cancelling colour
            for (size_t i = 1; i < spell.pts.size(); i++) {
                auto& a = spell.pts[i - 1]; auto& c = spell.pts[i];
                float age = t - c.t, alpha = std::fmax(0.0f, 1.0f - age / 1.6f) * (0.35f + 0.65f * pr);
                float col[4] = {cyan[0] + (to[0] - cyan[0]) * pr, cyan[1] + (to[1] - cyan[1]) * pr, cyan[2] + (to[2] - cyan[2]) * pr, alpha};
                V3 pa{a.p.x, a.p.y, a.p.z}, pc{c.p.x, c.p.y, c.p.z};
                b.line(pa, pc, norm(v3(head.position) - pc), 0.004f + 0.006f * pr, col);
            }
        }
        // Bursts: a ring that expands and fades over 0.7 s.
        auto tn = std::chrono::steady_clock::now();
        bursts.erase(std::remove_if(bursts.begin(), bursts.end(), [&](const Burst& x) { return tn - x.t0 > std::chrono::milliseconds(700); }), bursts.end());
        for (auto& x : bursts) {
            float f = std::chrono::duration<float>(tn - x.t0).count() / 0.7f, rr = x.r * (1 + 1.2f * f);
            float col[4] = {x.col[0], x.col[1], x.col[2], 1 - f};
            V3 n = norm(cross(x.ax, x.ay));
            for (int i = 0; i < 36; i++) {
                float a0 = i * 0.17453f, a1 = (i + 1) * 0.17453f;
                b.line(x.c + x.ax * (rr * std::cos(a0)) + x.ay * (rr * std::sin(a0)), x.c + x.ax * (rr * std::cos(a1)) + x.ay * (rr * std::sin(a1)), n, 0.008f * (1 - f) + 0.002f, col);
            }
        }
    }
    void drawExtras(Batch& b) {
        V3 eye = v3(head.position);
        for (size_t i = 0; i < extras.size(); i++) {
            const Placement& p = extras[i].p; Entity* w = wallById(p.wall); if (!w || !w->located) continue;
            if (!seenFrom(eye, xform(w->pose, {p.x, p.y, 0.02f}), rot(w->pose.orientation, {0, 0, 1}), p.wall)) continue;
            auto P = [&](float x, float y, float z) { return xform(w->pose, {x, y, z}); };
            const float bg[4] = {0.07f, 0.08f, 0.11f, 0.94f}, edge[4] = {0.35f, 0.75f, 1.0f, 0.8f}, white[4] = {0.92f, 0.95f, 1.0f, 1}, dim[4] = {0.6f, 0.66f, 0.76f, 1};
            float x0 = p.x - p.w / 2, x1 = p.x + p.w / 2, y0 = p.y - p.h / 2, y1 = p.y + p.h / 2;
            b.quad(P(x0, y0, 0.02f), P(x1, y0, 0.02f), P(x1, y1, 0.02f), P(x0, y1, 0.02f), bg);
            V3 n = rot(w->pose.orientation, {0, 0, 1});
            b.line(P(x0, y0, 0.021f), P(x1, y0, 0.021f), n, 0.006f, edge); b.line(P(x1, y0, 0.021f), P(x1, y1, 0.021f), n, 0.006f, edge);
            b.line(P(x1, y1, 0.021f), P(x0, y1, 0.021f), n, 0.006f, edge); b.line(P(x0, y1, 0.021f), P(x0, y0, 0.021f), n, 0.006f, edge);
            std::string title = "SCREEN " + std::to_string(extras[i].id);
            text(b, *w, p.x, p.y + p.h * 0.08f, 0.022f, UiGrid::fit(title, 0.012f, p.w * 0.6f, p.h * 0.2f), title.c_str(), white);
            text(b, *w, p.x, p.y - p.h * 0.12f, 0.022f, UiGrid::fit("MAC DISPLAY COMING", 0.006f, p.w * 0.7f, p.h * 0.1f), "MAC DISPLAY COMING", dim);
            if (!editLocked) {  // grab bar
                float y = p.y - p.h / 2 - kBarGap, hw = kBarW / 2, hh = kBarH / 2;
                const float idle[4] = {0.75f, 0.78f, 0.82f, 0.35f}, hover[4] = {0.9f, 0.93f, 1.0f, 0.75f}, fill[4] = {0.3f, 0.75f, 1.0f, 0.95f};
                const float* col = extraMoving == (int)i ? fill : extraBarHover == (int)i ? hover : idle;
                b.quad(P(p.x - hw, y - hh, 0.02f), P(p.x + hw, y - hh, 0.02f), P(p.x + hw, y + hh, 0.02f), P(p.x - hw, y + hh, 0.02f), col);
                if (extraBarHover == (int)i && extraHold > 0) { float xe = p.x - hw + kBarW * std::fmin(extraHold, 1.0f); b.quad(P(p.x - hw, y - hh, 0.022f), P(xe, y - hh, 0.022f), P(xe, y + hh, 0.022f), P(p.x - hw, y + hh, 0.022f), fill); }
            }
        }
        if (extraMoving >= 0 && candidate.active) if (auto* w = wallById(candidate.wall)) {
            const float good[4] = {0.2f, 1.0f, 0.4f, 0.9f}, bad[4] = {1.0f, 0.25f, 0.2f, 0.9f};
            panelQuad(b, candidate, *w, 0.03f, candidate.valid ? good : bad, false);
        }
    }
    void updateVoice() {
        const float dt = 1.0f / 72;
        bool raised = false;
        for (int h = 0; h < 2; h++) if (handActive[h] && ray[h].fist) {
            auto& pj = joints[h][XR_HAND_JOINT_PALM_EXT];
            if (pj.pose.position.y > head.position.y - 0.35f) raised = true;
        }
        if (raised) { fistTime += dt; openTime = 0; } else { openTime += dt; fistTime = 0; }
        V3 fwd = rot(head.orientation, {0, 0, -1}); fwd.y = 0; fwd = norm(fwd);
        for (int h = 0; h < 2; h++) {
            if (!handActive[h]) { trailN[h] = 0; sinceFist[h] = 9; continue; }
            sinceFist[h] = ray[h].fist ? 0 : sinceFist[h] + dt;
            palmTrail[h][trailN[h] % kTrail] = v3(joints[h][XR_HAND_JOINT_PALM_EXT].pose.position); trailN[h]++;
            if (trailN[h] < kTrail || pendingEnter) continue;
            V3 now = palmTrail[h][(trailN[h] - 1) % kTrail], then = palmTrail[h][trailN[h] % kTrail];
            float push = dot(now - then, fwd);  // forward travel over the last ~0.33 s
            V3 right = rot(head.orientation, {1, 0, 0}); right.y = 0; right = norm(right);
            float lateral = dot(now - then, right);
            bool recent = !typed.empty() && std::chrono::duration<float>(std::chrono::steady_clock::now() - typedAt).count() < kUndoWindow;
            if (lateral < -0.2f && std::fabs(push) < 0.12f && sinceFist[h] > 0.6f && handOpen(h) && recent && !dictation.recording) {
                int n = typed.back(); typed.pop_back(); undos++;
                backspaces(n); typedAt = std::chrono::steady_clock::now();
                caption = "ERASED"; captionAt = typedAt; event("undo: " + std::to_string(n) + " chars");
                trailN[h] = 0; continue;
            }
            if (push > 0.16f && sinceFist[h] < 0.6f && handOpen(h)) {
                throws++; event("throw: " + std::to_string(push) + " m");
                if (dictation.recording) dictation.stop();
                pendingEnter = true; pendingFor = 0; trailN[h] = 0;
            }
        }
        if (!dictation.recording && fistTime >= kFistHold) { dictation.start(); event("dictation started"); }
        if (dictation.recording && openTime >= kFistRelease) { dictation.stop(); event("dictation stopped"); }
        std::string t;
        while (dictation.take(t)) {
            if (streamInput()) {
                std::string out = t + " "; LiSendUtf8TextEvent(out.c_str(), (unsigned)out.size());
                typed.push_back(codepoints(out)); typedAt = std::chrono::steady_clock::now();
                if (typed.size() > 20) typed.erase(typed.begin());
            }
            caption = t; captionAt = std::chrono::steady_clock::now();
            event("dictated: " + t);
        }
        if (pendingEnter) {
            pendingFor += dt;
            if (dictation.drained() || pendingFor > 10) { pendingEnter = false; pressEnter(); }
        }
    }
    void drawVoice(Batch& b) {
        float since = std::chrono::duration<float>(std::chrono::steady_clock::now() - captionAt).count();
        bool showCaption = !caption.empty() && since < 5.0f;
        if (!dictation.recording && dictation.state != "transcribing" && !showCaption) return;
        // Head-locked strip, low in the view.
        V3 f = rot(head.orientation, {0, 0, -1}); f.y = 0; f = norm(f); if (len(f) < 0.5f) f = {0, 0, -1};
        float yaw = std::atan2(-f.x, -f.z);
        Entity hud; hud.pose.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
        V3 c = v3(head.position) + f * 0.75f; c.y -= 0.28f; hud.pose.position = {c.x, c.y, c.z};
        auto P = [&](float x, float y) { return xform(hud.pose, {x, y, 0}); };
        const float bg[4] = {0.05f, 0.06f, 0.08f, 0.75f}, white[4] = {0.95f, 0.97f, 1.0f, 1.0f}, red[4] = {1.0f, 0.25f, 0.2f, 1.0f}, amber[4] = {1.0f, 0.75f, 0.1f, 1.0f};
        std::string line = dictation.recording ? "LISTENING" : dictation.state == "transcribing" ? "TRANSCRIBING" : "";
        std::string cap;
        if (showCaption) { for (char ch : caption) cap += (char)toupper((unsigned char)ch); if (cap.size() > 44) cap = cap.substr(cap.size() - 44); }
        float w = 0.5f, h = cap.empty() ? 0.05f : 0.09f;
        b.quad(P(-w / 2, -h / 2), P(w / 2, -h / 2), P(w / 2, h / 2), P(-w / 2, h / 2), bg);
        if (!line.empty()) {
            const float* dot = dictation.recording ? red : amber;
            float pulse = dictation.recording ? 0.006f + 0.003f * std::fmin(dictation.level.load() / 3000.0f, 1.0f) : 0.006f, dx = -0.2f, dy = cap.empty() ? 0 : 0.02f;
            b.quad(xform(hud.pose, {dx - pulse, dy - pulse, 0.002f}), xform(hud.pose, {dx + pulse, dy - pulse, 0.002f}),
                   xform(hud.pose, {dx + pulse, dy + pulse, 0.002f}), xform(hud.pose, {dx - pulse, dy + pulse, 0.002f}), dot);
            text(b, hud, 0.0f, dy, 0.002f, 0.004f, line.c_str(), white);
        }
        if (!cap.empty()) text(b, hud, 0.0f, line.empty() ? 0 : -0.022f, 0.002f, 0.0018f, cap.c_str(), white);
    }
    void updatePlacement() {
        candidate.active = false;
        updateVisibility();
        pollBattery();
        updateVoice();
        updateSpell();
        if (updateMenu() || updateExtras()) {
            releasePointer(); barHover = false; cornerHover = -1;
            for (int k = 0; k < 2; k++) pinchPrev[k] = handActive[k] && ray[k].pinch;
            return;
        }
        float aspect = 0.625f;
        { std::lock_guard<std::mutex> g(video.lock); if (video.width > 0 && video.height > 0) aspect = (float)video.height / (float)video.width; }
        const float sizes[3][2] = {{1.0f, 1.0f * aspect}, {1.44f, 1.44f * aspect}, {2.0f, 2.0f * aspect}};
        if (placed.active && std::fabs(placed.h - placed.w * aspect) > 0.005f) { placed.h = placed.w * aspect; if (auto* w = wallById(placed.wall)) validate(placed, *w); }
        bool pinch[2];
        for (int h = 0; h < 2; h++) pinch[h] = handActive[h] && ray[h].pinch;
        float candW = placed.active ? placed.w : sizes[sizeIndex][0], candH = placed.active ? placed.h : sizes[sizeIndex][1];
        barHover = false;
        int h = 1;
        if (handActive[h] && ray[h].valid) {
            V3 o = ray[h].o, d = ray[h].d;
            aimFrames++; lastAimO = o; lastAimD = d;
            float best = 1e9f; Entity* hit = nullptr; float hx = 0, hy = 0; float nearest = 1e9f;
            for (auto& e : entities) {
                if (!isWall(e) || !e.located) continue;
                V3 lo = untransform(e.pose, o), ld = rot(qinv(e.pose.orientation), d);
                if (std::fabs(ld.z) < 1e-4f) continue;
                float t = -lo.z / ld.z; if (t <= 0 || t > best) continue;
                float x = lo.x + ld.x * t, y = lo.y + ld.y * t;
                if (t < nearest) { nearest = t; lastNearestWall = e.uuid.substr(0, 8) + " x=" + std::to_string(x) + " y=" + std::to_string(y); }
                if (x < e.r2.offset.x || y < e.r2.offset.y || x > e.r2.offset.x + e.r2.extent.width || y > e.r2.offset.y + e.r2.extent.height) continue;
                best = t; hit = &e; hx = x; hy = y;
            }
            lastNearest = nearest < 1e8f ? nearest : -1;
            bool onPanel = false;
            hoverButton = -1;
            if (moving) {
                // The bar stays under the hand: the panel hangs above the grab point.
                releasePointer();
                if (hit) {
                    candidate.active = true; candidate.wall = hit->uuid; candidate.x = hx; candidate.y = hy + candH / 2 + kBarGap;
                    candidate.w = candW; candidate.h = candH; validate(candidate, *hit);
                }
                if (!pinch[1]) {
                    if (candidate.active && candidate.valid) { placed = candidate; savePlacement(); event("moved panel to wall " + placed.wall); }
                    else event("move cancelled");
                    moving = false; candidate.active = false;
                }
                for (int k = 0; k < 2; k++) pinchPrev[k] = pinch[k];
                return;
            }
            Entity* pw = placed.active ? wallById(placed.wall) : nullptr;
            if (resizing2 || cornerDrag >= 0) {
                releasePointer();
                if (pw && resizing2) {
                    if (pinch[0] && pinch[1]) resizeTo(resizeStartW * len(ray[0].tip - ray[1].tip) / resizeStartD, *pw);
                    validate(placed, *pw);
                }
                if (pw && cornerDrag >= 0 && hit == pw) {
                    float aspect = placed.h / placed.w;
                    resizeTo(std::fmax(std::fabs(hx - anchorX), std::fabs(hy - anchorY) / aspect), *pw);
                    placed.x = anchorX + (cornerDrag & 1 ? 1 : -1) * placed.w / 2;
                    placed.y = anchorY + (cornerDrag & 2 ? 1 : -1) * placed.h / 2;
                    validate(placed, *pw);
                }
                if (resizing2 ? !(pinch[0] && pinch[1]) : !pinch[1]) {
                    resizing2 = false; cornerDrag = -1; savePlacement(); event("resized panel to " + std::to_string(placed.w) + " m");
                }
                for (int k = 0; k < 2; k++) pinchPrev[k] = pinch[k];
                return;
            }
            bool onScreen = hit && pw && hit == pw && std::fabs(hx - placed.x) < placed.w / 2 + 0.05f && std::fabs(hy - placed.y) < placed.h / 2 + kBarGap + 0.05f;
            if (onScreen && !editLocked && pinch[0] && pinch[1] && (!pinchPrev[0] || !pinchPrev[1]) && handActive[0]) {
                resizing2 = true; resizeStartW = placed.w; resizeStartD = std::fmax(len(ray[0].tip - ray[1].tip), 0.05f);
                releasePointer(); event("two-hand resize");
                for (int k = 0; k < 2; k++) pinchPrev[k] = pinch[k];
                return;
            }
            // Corners: bit 0 = right, bit 1 = top. Grabbing one scales the screen around the opposite corner.
            cornerHover = -1;
            if (onScreen && !editLocked) for (int c = 0; c < 4; c++) {
                float cx = placed.x + (c & 1 ? 1 : -1) * placed.w / 2, cy = placed.y + (c & 2 ? 1 : -1) * placed.h / 2;
                if (std::fabs(hx - cx) < kCorner && std::fabs(hy - cy) < kCorner) cornerHover = c;
            }
            if (cornerHover >= 0) {
                onPanel = true; releasePointer();
                if (pinch[1] && !pinchPrev[1]) {
                    cornerDrag = cornerHover;
                    anchorX = placed.x - (cornerDrag & 1 ? 1 : -1) * placed.w / 2; anchorY = placed.y - (cornerDrag & 2 ? 1 : -1) * placed.h / 2;
                    event("corner resize");
                }
            }
            if (!editLocked && cornerHover < 0 && hit && placed.active && hit->uuid == placed.wall) {
                float bx = placed.x, by = placed.y - placed.h / 2 - kBarGap;
                if (std::fabs(hx - bx) < kBarW / 2 + 0.03f && std::fabs(hy - by) < kBarH / 2 + 0.03f) barHover = true;
            }
            if (barHover) {
                onPanel = true;
                releasePointer();
                if (pinch[1] && !pinchPrev[1]) { barHolding = true; barPinchStart = std::chrono::steady_clock::now(); }
                if (barHolding && pinch[1]) {
                    barHold = std::chrono::duration<float>(std::chrono::steady_clock::now() - barPinchStart).count() / kHoldToMove;
                    if (barHold >= 1) { moving = true; barHolding = false; barHold = 0; event("move started"); }
                }
            }
            if (!barHover || !pinch[1]) { barHolding = false; barHold = 0; }
            if (cornerHover < 0 && !barHover && hit && placed.active && hit->uuid == placed.wall) {
                for (int b = 0; b < 2; b++) {
                    float bx, by; buttonCenter(b, bx, by);
                    if (std::fabs(hx - bx) < kButtonW / 2 && std::fabs(hy - by) < kButtonH / 2) hoverButton = b;
                }
                if (hoverButton >= 0) {
                    onPanel = true;  // buttons belong to the panel: no placement outline, no pointer
                    if (pinch[1] && !pinchPrev[1]) { launcher.launch(hostFor(hoverButton)); event(std::string("switch to ") + hosts[hoverButton].name); }
                }
            }
            if (cornerHover < 0 && !barHover && hit && placed.active && hit->uuid == placed.wall && hoverButton < 0) {
                float u = (hx - (placed.x - placed.w / 2)) / placed.w, v = 1.0f - (hy - (placed.y - placed.h / 2)) / placed.h;
                if (u >= 0 && u <= 1 && v >= 0 && v <= 1) {
                    onPanel = true;
                    if (!pointer.on) { pointer.u = u; pointer.v = v; }
                    // Heavy smoothing when the hand is still (precision), light when it moves fast (no lag).
                    float speed = std::hypot(u - pointer.u, v - pointer.v) * 72.0f;  // screen widths per second
                    float a = std::fmin(0.08f + speed * 0.9f, 0.7f);
                    if (ray[1].locked) a = 0;
                    pointer.u += (u - pointer.u) * a; pointer.v += (v - pointer.v) * a;
                    pointer.on = true;
                    drivePointer(pinch[1] && !pinchPrev[1], !pinch[1] && pinchPrev[1]);
                }
            }
            if (!onPanel) releasePointer();
            if (hit && !onPanel && !placed.active) { hitFrames++;
                candidate.active = true; candidate.wall = hit->uuid; candidate.x = hx; candidate.y = hy;
                candidate.w = candW; candidate.h = candH;
                validate(candidate, *hit);
                if (pinch[1] && !pinchPrev[1]) pinchEdges++;
                if (pinch[1] && !pinchPrev[1] && candidate.valid) { placed = candidate; savePlacement(); event("placed panel on wall " + placed.wall); }
            }
        }
        if (!(handActive[1] && ray[1].valid)) releasePointer();
        for (int k = 0; k < 2; k++) pinchPrev[k] = pinch[k];
    }

    static constexpr float kButtonW = 0.30f, kButtonH = 0.085f;
    void buttonCenter(int b, float& x, float& y) {
        x = placed.x + (b == 0 ? -0.17f : 0.17f);
        y = placed.y + placed.h / 2 + 0.075f;
        if (auto* w = wallById(placed.wall)) if (y + kButtonH / 2 > w->r2.offset.y + w->r2.extent.height) y = placed.y - placed.h / 2 - kBarGap - kBarH / 2 - 0.07f;
    }
    // 5x7 pixel font, A-Z, 0-9 and % - . : ! (bit 16 is the leftmost column).
    static const uint8_t* glyph(char c) {
        static const uint8_t F[26][7] = {
            {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
            {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
            {14, 4, 4, 4, 4, 4, 14}, {7, 2, 2, 2, 2, 18, 12}, {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
            {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
            {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4},
            {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
            {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31}};
        static const uint8_t D[10][7] = {
            {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31}, {31, 2, 4, 2, 1, 17, 14},
            {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14}, {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
            {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12}};
        static const uint8_t PCT[7] = {24, 25, 2, 4, 8, 19, 3}, DASH[7] = {0, 0, 0, 31, 0, 0, 0}, DOT[7] = {0, 0, 0, 0, 0, 12, 12},
                             COLON[7] = {0, 12, 12, 0, 12, 12, 0}, BANG[7] = {4, 4, 4, 4, 4, 0, 4};
        switch (c) { case '%': return PCT; case '-': return DASH; case '.': return DOT; case ':': return COLON; case '!': return BANG; }
        return c >= 'A' && c <= 'Z' ? F[c - 'A'] : c >= '0' && c <= '9' ? D[c - '0'] : nullptr;
    }
    void text(Batch& b, const Entity& w, float cx, float cy, float z, float px, const char* s, const float* col) {
        int n = (int)strlen(s); float x0 = cx - (n * 6 - 1) * px / 2, y0 = cy + 3.5f * px;
        for (int i = 0; i < n; i++) if (auto* g = glyph(s[i])) for (int r = 0; r < 7; r++) for (int c = 0; c < 5; c++) if (g[r] & (16 >> c)) {
            float x = x0 + (i * 6 + c) * px, y = y0 - (r + 1) * px;
            b.quad(xform(w.pose, {x, y, z}), xform(w.pose, {x + px, y, z}), xform(w.pose, {x + px, y + px, z}), xform(w.pose, {x, y + px, z}), col);
        }
    }
    void buttons(Batch& b) {
        auto* w = wallById(placed.wall); if (!w) return;
        std::string st = launcher.get(), current = video.source;
        for (int i = 0; i < 2; i++) {
            float x, y; buttonCenter(i, x, y);
            bool active = current == hosts[i].name, connecting = launcher.busy && st.find(hosts[i].name) != std::string::npos;
            const float idle[4] = {0.12f, 0.14f, 0.18f, 0.85f}, on[4] = {0.10f, 0.45f, 0.22f, 0.92f}, wait[4] = {0.55f, 0.40f, 0.08f, 0.92f};
            const float* fill = connecting ? wait : active ? on : idle;
            float f2[4] = {fill[0], fill[1], fill[2], fill[3]};
            if (hoverButton == i) for (int k = 0; k < 3; k++) f2[k] = std::fmin(1.0f, f2[k] + 0.18f);
            float hw = kButtonW / 2, hh = kButtonH / 2;
            b.quad(xform(w->pose, {x - hw, y - hh, 0.02f}), xform(w->pose, {x + hw, y - hh, 0.02f}), xform(w->pose, {x + hw, y + hh, 0.02f}), xform(w->pose, {x - hw, y + hh, 0.02f}), f2);
            const float white[4] = {0.95f, 0.97f, 1.0f, 1.0f};
            text(b, *w, x, y, 0.025f, 0.007f, hosts[i].name, white);
        }
    }

    bool streamInput() { return moonlight.running && moonlight.get(moonlight.stage) == "streaming"; }
    void drivePointer(bool down, bool up) {
        int W = moonlight.width, H = moonlight.height;
        int x = (int)(pointer.u * W + 0.5f), y = (int)(pointer.v * H + 0.5f);
        if (streamInput() && (x != pointer.sentX || y != pointer.sentY)) {
            LiSendMousePositionEvent((short)x, (short)y, (short)W, (short)H); pointer.sentX = x; pointer.sentY = y; pointer.moves++;
        }
        // Every press is a pure click at the spot where it started: down, then up 40 ms later. The button is never
        // held while the hand moves, so a click can't turn into a text selection or a window drag.
        if (down && !pointer.pressed) {
            pointer.pressed = true; pointer.clicks++;
            if (streamInput()) std::thread([this] {
                std::lock_guard<std::mutex> g(keyLock);
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
            }).detach();
        }
        if (up) pointer.pressed = false;
    }
    void releasePointer(bool leave = true) {
        pointer.pressed = false;  // the click already finished on the Mac
        if (leave) pointer.on = false;
    }

    void panelQuad(Batch& b, const Placement& p, const Entity& wall, float z, const float* col, bool screen) {
        V3 a = xform(wall.pose, {p.x - p.w / 2, p.y - p.h / 2, z}), bb = xform(wall.pose, {p.x + p.w / 2, p.y - p.h / 2, z});
        V3 c = xform(wall.pose, {p.x + p.w / 2, p.y + p.h / 2, z}), d = xform(wall.pose, {p.x - p.w / 2, p.y + p.h / 2, z});
        if (screen) b.quad(a, bb, c, d, col, true);
        else {
            V3 n = rot(wall.pose.orientation, {0, 0, 1});
            b.dashed(a, bb, n, 0.012f, col); b.dashed(bb, c, n, 0.012f, col); b.dashed(c, d, n, 0.012f, col); b.dashed(d, a, n, 0.012f, col);
            float fill[4] = {col[0], col[1], col[2], 0.12f}; b.quad(a, bb, c, d, fill);
        }
    }

    // Frame over the panel edge. Every style keeps an opaque core from 4 mm inside to 6 mm outside the edge,
    // which covers the compositor's line; the styles differ around it. AMBI vertices carry panel UVs shifted
    // by -10, and the eye shader then colours them from the screen's nearest edge (a high mip of the video).
    void bezelFrame(Batch& b, const Entity& wall) {
        float l = placed.x - placed.w / 2, r = placed.x + placed.w / 2, bt = placed.y - placed.h / 2, t = placed.y + placed.h / 2;
        const float z = 0.022f, in = 0.004f;
        auto P = [&](float x, float y) { return xform(wall.pose, {x, y, z}); };
        // A ring between distances d0 and d1 outside the panel edge (negative = inside), as four strips.
        auto ring = [&](float d0, float d1, const float* col) {
            float xs[4][4] = {{l - d1, t + d0, r + d1, t + d1}, {l - d1, bt - d1, r + d1, bt - d0},   // top, bottom
                              {l - d1, bt - d0, l - d0, t + d0}, {r + d0, bt - d0, r + d1, t + d0}};  // left, right
            for (auto& q : xs) {
                V3 a = P(q[0], q[1]), bb = P(q[2], q[1]), c = P(q[2], q[3]), d = P(q[0], q[3]);
                if (!bezelAmbi) { b.quad(a, bb, c, d, col); continue; }
                auto uv = [&](float x, float y, float* o) { o[0] = std::fmin(1, std::fmax(0, (x - l) / placed.w)) - 10; o[1] = std::fmin(1, std::fmax(0, (y - bt) / placed.h)); };
                float ua[2], ub[2], uc[2], ud[2]; uv(q[0], q[1], ua); uv(q[2], q[1], ub); uv(q[2], q[3], uc); uv(q[0], q[3], ud);
                b.tri(a, bb, c, col, ua, ub, uc); b.tri(a, c, d, col, ua, uc, ud);
            }
        };
        if (bezelAmbi) {  // the screen's own edge colours, opaque core then fading into the room
            const float core[4] = {1, 1, 1, 1}; ring(-in, 0.006f, core);
            for (int i = 0; i < 6; i++) { float a = 0.85f * (1 - i / 6.0f); const float c[4] = {1, 1, 1, a}; ring(0.006f + i * 0.004f, 0.010f + i * 0.004f, c); }
        } else if (bezelGlass) {  // frosted acrylic: light core, translucent rim showing the room, faint top highlight
            const float core[4] = {0.80f, 0.84f, 0.90f, 1}, rim[4] = {0.86f, 0.90f, 0.96f, 0.35f}, hi[4] = {1, 1, 1, 0.55f};
            ring(-in, 0.006f, core); ring(0.006f, 0.020f, rim);
            V3 a = P(l - 0.020f, t + 0.018f), bb = P(r + 0.020f, t + 0.018f), c = P(r + 0.020f, t + 0.020f), d = P(l - 0.020f, t + 0.020f);
            b.quad(a, bb, c, d, hi);
        } else if (bezelFade) {  // dark core melting into the room over ~2 cm
            const float core[4] = {0.06f, 0.06f, 0.07f, 1}; ring(-in, 0.006f, core);
            for (int i = 0; i < 6; i++) { float a = 0.8f * (1 - i / 6.0f); const float c[4] = {0.06f, 0.06f, 0.07f, a}; ring(0.006f + i * 0.003f, 0.009f + i * 0.003f, c); }
        } else {  // grey: the frame that removed the line
            const float grey[4] = {0.06f, 0.06f, 0.07f, 1}; ring(-in, 0.014f, grey);
        }
    }

    void build(Batch& occ, Batch& over) {
        const float wallCol[4] = {0.3f, 0.85f, 1.0f, 0.55f}, planeCol[4] = {1, 1, 1, 0.35f}, volCol[4] = {1.0f, 0.6f, 0.15f, 0.6f};
        bool placing = moving || !placed.active;
        for (auto& e : entities) {
            if (!e.located || !placing) continue;
            if (e.has2d && !e.has3d) {
                auto c = corners2d(e); V3 n = rot(e.pose.orientation, {0, 0, 1}); V3 lift = n * 0.01f;
                const float* col = isWall(e) ? wallCol : planeCol;
                for (int i = 0; i < 4; i++) occ.dashed(c[i] + lift, c[(i + 1) % 4] + lift, n, 0.008f, col);
            }
            if (e.has3d && false) {
                auto c = corners3d(e);
                int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (auto& ed : edges) occ.ray(c[ed[0]], c[ed[1]], 0.008f, volCol);
            }
        }
        if (bezel && quadShown && !panelLayer.curve && placed.active && panelSeen) if (auto* w = wallById(placed.wall)) bezelFrame(occ, *w);
        if (placed.active && panelSeen) if (auto* w = wallById(placed.wall)) { const float c[4] = {1, 1, 1, 1}; panelQuad(occ, placed, *w, 0.02f, c, true); buttons(occ); if (!editLocked) { grabBar(occ, *w); corners(occ, *w); } }
        drawMenu(over);
        drawVoice(over);
        drawExtras(occ);
        drawSpell(over);
        drawToasts(over);
        if (candidate.active) if (auto* w = wallById(candidate.wall)) {
            const float good[4] = {0.2f, 1.0f, 0.4f, 0.9f}, bad[4] = {1.0f, 0.25f, 0.2f, 0.9f};
            panelQuad(occ, candidate, *w, 0.03f, candidate.valid ? good : bad, false);
        }
        if (pointer.on && placed.active) if (auto* w = wallById(placed.wall)) {
            float px = placed.x - placed.w / 2 + pointer.u * placed.w, py = placed.y - placed.h / 2 + (1 - pointer.v) * placed.h;
            V3 c = xform(w->pose, {px, py, 0.035f}), n = rot(w->pose.orientation, {0, 0, 1}), ax = rot(w->pose.orientation, {1, 0, 0}), ay = rot(w->pose.orientation, {0, 1, 0});
            // Draw and loose: the ring tightens and warms as the thumb (or pinch) closes; a solid dot marks the click.
            const HandRay& hr = ray[1];
            float draw = std::fmax(std::fmin(std::fmax((0.065f - hr.thumb) / (0.065f - 0.024f), 0.0f), 1.0f),
                                   std::fmin(std::fmax((0.06f - hr.gap) / (0.06f - 0.018f), 0.0f), 1.0f));
            bool dragging = false;  // presses are pure clicks: there is no drag state any more
            if (pointer.pressed) draw = 1;
            const float white[3] = {1, 1, 1}, gold[3] = {1.0f, 0.8f, 0.2f}, blue[3] = {0.3f, 0.75f, 1.0f};
            float col[4];
            for (int k = 0; k < 3; k++) col[k] = dragging ? blue[k] : white[k] + (gold[k] - white[k]) * draw;
            col[3] = 0.75f + 0.25f * draw;
            // Constant angular size (about 1.4 degrees open, 0.6 closed) and a dark outline, so it reads on white windows.
            float dist = std::fmax(len(c - v3(head.position)), 0.3f), k = dist / 1.5f;
            float r = (0.018f - 0.010f * draw) * k, lw = 0.004f * k;
            const float shadow[4] = {0, 0, 0, 0.65f};
            auto ring = [&](float rr, float w, const float* cc, float z) {
                V3 cz = c + n * z;
                for (int i = 0; i < 20; i++) {
                    float a0 = i * 0.31416f, a1 = (i + 1) * 0.31416f;
                    over.line(cz + ax * (rr * std::cos(a0)) + ay * (rr * std::sin(a0)), cz + ax * (rr * std::cos(a1)) + ay * (rr * std::sin(a1)), n, w, cc);
                }
            };
            ring(r, lw * 2.2f, shadow, 0);
            ring(r, lw, col, 0.001f);
            float d = (pointer.pressed ? 0.005f : 0.0022f) * k;
            V3 cz = c + n * 0.001f;
            over.quad(cz - ax * (d * 1.8f) - ay * (d * 1.8f), cz + ax * (d * 1.8f) - ay * (d * 1.8f), cz + ax * (d * 1.8f) + ay * (d * 1.8f), cz - ax * (d * 1.8f) + ay * (d * 1.8f), shadow);
            over.quad(cz - ax * d - ay * d, cz + ax * d - ay * d, cz + ax * d + ay * d, cz - ax * d + ay * d, col);
        }
        const float rayCol[4] = {1, 1, 1, 0.35f};
        for (int h = 0; h < 2; h++) {
            if (!handActive[h]) continue;
            if (h == 1 && ray[h].valid) {
                V3 o = ray[h].o, d = ray[h].d;
                float L = lastNearest > 0 ? std::fmin(lastNearest, 6.0f) : 3.0f;
                const float hitCol[4] = {0.3f, 1.0f, 0.5f, 0.8f};
                over.ray(o + d * 0.05f, o + d * L, 0.003f, candidate.active ? hitCol : rayCol);
            }
        }
    }
    void drawMenu(Batch& b) {
        V3 palm;
        if (palmHold > 0 && !(editLocked && menuSaved) && palmFacing(palm)) {  // the ring fills while summoning
            V3 toHead = norm(v3(head.position) - palm), ax = norm(cross({0, 1, 0}, toHead)), ay = cross(toHead, ax);
            const float col[4] = {0.3f, 0.75f, 1.0f, 0.95f}; float r = 0.03f; int n = (int)(24 * std::fmin(palmHold, 1.0f));
            for (int i = 0; i < n; i++) {
                float a0 = 1.5708f - i * 0.2618f, a1 = 1.5708f - (i + 1) * 0.2618f;
                b.line(palm + ax * (r * std::cos(a0)) + ay * (r * std::sin(a0)), palm + ax * (r * std::cos(a1)) + ay * (r * std::sin(a1)), toHead, 0.005f, col);
            }
        }
        if (!menuOpen || !menuSeen) return;
        auto P = [&](float x, float y, float z) { return xform(menu.pose, {x, y, z}); };
        float hw = menuHalfW(), hh = menuHalfH();
        const float bg[4] = {0.05f, 0.06f, 0.08f, 0.8f}, white[4] = {0.95f, 0.97f, 1.0f, 1.0f};
        const float dim[4] = {0.62f, 0.68f, 0.78f, 1.0f};
        b.quad(P(-hw, -hh, 0), P(hw, -hh, 0), P(hw, hh, 0), P(-hw, hh, 0), bg);
        text(b, menu, 0, menuGrid.titleY, 0.003f, UiGrid::fit("SETTINGS", menuGrid.titlePx, 2 * hw - 2 * menuGrid.pad, menuGrid.titleH * 0.6f), "SETTINGS", white);
        for (auto& h : menuGrid.headings) text(b, menu, h.x, h.y, 0.003f, h.px, h.text.c_str(), dim);
        if (!editLocked) {
            float y = menuBarY(), bw = 0.3f, bh = 0.025f;
            const float idle[4] = {0.75f, 0.78f, 0.82f, 0.45f}, hover[4] = {0.9f, 0.93f, 1.0f, 0.8f}, fill[4] = {0.3f, 0.75f, 1.0f, 0.95f};
            const float* col = menuMoving ? fill : menuBarHover ? hover : idle;
            b.quad(P(-bw / 2, y - bh / 2, 0.002f), P(bw / 2, y - bh / 2, 0.002f), P(bw / 2, y + bh / 2, 0.002f), P(-bw / 2, y + bh / 2, 0.002f), col);
            if (menuBarHold > 0 && !menuMoving) {
                float x1 = -bw / 2 + bw * std::fmin(menuBarHold, 1.0f);
                b.quad(P(-bw / 2, y - bh / 2, 0.003f), P(x1, y - bh / 2, 0.003f), P(x1, y + bh / 2, 0.003f), P(-bw / 2, y + bh / 2, 0.003f), fill);
            }
        }
        std::string current = video.source;
        for (size_t k = 0; k < menuShown.size(); k++) {
            int i = menuShown[k]; const UiGrid::Cell& c = menuGrid.cells[k];
            bool on = itemOn(menuItems[i].label) > 0;
            const float idle[4] = {0.16f, 0.18f, 0.24f, 0.95f}, act[4] = {0.10f, 0.45f, 0.22f, 0.95f};
            float f[4]; for (int q = 0; q < 4; q++) f[q] = on ? act[q] : idle[q];
            if (menuHover == i) for (int q = 0; q < 3; q++) f[q] = std::fmin(1.0f, f[q] + 0.2f);
            b.quad(P(c.x - c.w / 2, c.y - c.h / 2, 0.002f), P(c.x + c.w / 2, c.y - c.h / 2, 0.002f),
                   P(c.x + c.w / 2, c.y + c.h / 2, 0.002f), P(c.x - c.w / 2, c.y + c.h / 2, 0.002f), f);
            text(b, menu, c.x, c.y, 0.004f, c.px, menuItems[i].label, white);
        }
        if (menuHover >= 0 && handActive[1] && ray[1].valid) {
            V3 lo = untransform(menu.pose, ray[1].o), ld = rot(qinv(menu.pose.orientation), ray[1].d);
            float t = -lo.z / ld.z; V3 c = P(lo.x + ld.x * t, lo.y + ld.y * t, 0.006f);
            V3 n = rot(menu.pose.orientation, {0, 0, 1}), ax = rot(menu.pose.orientation, {1, 0, 0}), ay = rot(menu.pose.orientation, {0, 1, 0});
            const float col[4] = {1.0f, 0.8f, 0.2f, 1.0f}; float r = 0.008f;
            for (int i = 0; i < 12; i++) {
                float a0 = i * 0.5236f, a1 = (i + 1) * 0.5236f;
                b.line(c + ax * (r * std::cos(a0)) + ay * (r * std::sin(a0)), c + ax * (r * std::cos(a1)) + ay * (r * std::sin(a1)), n, 0.003f, col);
            }
        }
    }
    void corners(Batch& b, const Entity& w) {
        bool show = pointer.on || barHover || cornerHover >= 0 || cornerDrag >= 0 || resizing2;
        if (!show) return;
        V3 n = rot(w.pose.orientation, {0, 0, 1});
        const float dim[4] = {0.9f, 0.93f, 1.0f, 0.45f}, hot[4] = {0.3f, 0.75f, 1.0f, 0.95f};
        for (int c = 0; c < 4; c++) {
            float sx = c & 1 ? 1 : -1, sy = c & 2 ? 1 : -1, x = placed.x + sx * (placed.w / 2 + 0.012f), y = placed.y + sy * (placed.h / 2 + 0.012f), L = 0.07f;
            const float* col = (c == cornerHover || c == cornerDrag || resizing2) ? hot : dim;
            V3 k = xform(w.pose, {x, y, 0.024f});
            b.line(k, xform(w.pose, {x - sx * L, y, 0.024f}), n, 0.008f, col);
            b.line(k, xform(w.pose, {x, y - sy * L, 0.024f}), n, 0.008f, col);
        }
    }
    void grabBar(Batch& b, const Entity& w) {
        float x = placed.x, y = placed.y - placed.h / 2 - kBarGap, hw = kBarW / 2, hh = kBarH / 2;
        const float idle[4] = {0.75f, 0.78f, 0.82f, 0.35f}, hover[4] = {0.9f, 0.93f, 1.0f, 0.75f}, fill[4] = {0.3f, 0.75f, 1.0f, 0.95f};
        auto rect = [&](float x0, float x1, const float* col, float z) {
            b.quad(xform(w.pose, {x0, y - hh, z}), xform(w.pose, {x1, y - hh, z}), xform(w.pose, {x1, y + hh, z}), xform(w.pose, {x0, y + hh, z}), col);
        };
        rect(x - hw, x + hw, moving ? fill : barHover ? hover : idle, 0.02f);
        if (barHold > 0 && !moving) rect(x - hw, x - hw + kBarW * std::fmin(barHold, 1.0f), fill, 0.022f);
    }

    void render(const XrView* views, const Batch& occ, const Batch& over) {
        std::vector<Vtx> all(occ.v); all.insert(all.end(), over.v.begin(), over.v.end());
        glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, all.size() * sizeof(Vtx), all.data(), GL_STREAM_DRAW);
        GLuint depthTex = 0;
        if (depthValid) {
            depthTex = depthImages[depthImage.swapchainIndex].image;
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_ARRAY, depthTex);
            if (!depthConfigured.count(depthTex)) {
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_NONE);
                depthConfigured.insert(depthTex);
            }
        }
        bool hasVideo = video.update(display) && video.mipTex;
        if (quadTick++ % 60 == 0) {
            quadFile = access((gDir + "/quad-on").c_str(), F_OK) == 0;
            mipqFile = access((gDir + "/mipq-on").c_str(), F_OK) == 0;  // forces MIPQ for self-tests
        }
        panelLayer.mipQuality = mipqMenu || mipqFile;
        bool quad = hasVideo && (panelLayer.enabled || quadFile) && placed.active;
        if (quad) panelLayer.update(session, video.mipFbo, video.mipW, video.mipH, video.mipVersion);
        quadShown = quad && panelLayer.hasImage;
        if (depthValid) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_ARRAY, depthTex); }
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, video.mipTex); glActiveTexture(GL_TEXTURE0);
        for (int i = 0; i < 2; i++) {
            Eye& e = eyes[i]; uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; xrAcquireSwapchainImage(e.chain, &ai, &idx);
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION; xrWaitSwapchainImage(e.chain, &wi);
            glBindFramebuffer(GL_FRAMEBUFFER, e.fbo[idx]); glViewport(0, 0, e.w, e.h);
            glClearColor(0, 0, 0, 0); glClearDepthf(1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND); glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glUseProgram(program); glBindVertexArray(vao);
            M4 vp = mul(projection(views[i].fov, 0.05f, 100.0f), viewFromPose(views[i].pose));
            glUniformMatrix4fv(uViewProj, 1, GL_FALSE, vp.m);
            glUniform1i(uDepth, 0); glUniform1i(uVideo, 1); glUniform1i(uHasVideo, hasVideo ? (quadShown ? 2 : 1) : 0);
            if (depthValid) {
                auto& dv = depthImage.views[i];
                M4 dvp = mul(projection(dv.fov, depthImage.nearZ, depthImage.farZ), viewFromPose(dv.pose));
                glUniformMatrix4fv(uDepthViewProj, 1, GL_FALSE, dvp.m);
                glUniform3f(uDepthParams, depthImage.nearZ, std::isinf(depthImage.farZ) ? 0.0f : depthImage.farZ, 0.05f);
                glUniform1f(uLayer, (float)i);
            } else glUniformMatrix4fv(uDepthViewProj, 1, GL_FALSE, vp.m);
            glUniform1i(uOcclude, depthValid ? 1 : 0);
            if (!occ.v.empty()) glDrawArrays(GL_TRIANGLES, 0, (GLsizei)occ.v.size());
            glUniform1i(uOcclude, 0);
            if (!over.v.empty()) glDrawArrays(GL_TRIANGLES, (GLint)occ.v.size(), (GLsizei)over.v.size());
            glBindVertexArray(0); glBindFramebuffer(GL_FRAMEBUFFER, 0);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(e.chain, &ri);
        }
    }

    void frame() {
        XrFrameState fs{XR_TYPE_FRAME_STATE}; XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(session, &wi, &fs))) return;
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO}; xrBeginFrame(session, &bi);
        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerPassthroughFB ptl{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        if (ptLayer) { ptl.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; ptl.layerHandle = ptLayer; layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&ptl)); }
        if (fs.shouldRender) {
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO}; vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime; vli.space = local;
            XrViewState vs{XR_TYPE_VIEW_STATE}; XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; uint32_t vc = 0;
            xrLocateViews(session, &vli, &vs, 2, &vc, views);
            XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(view, local, fs.predictedDisplayTime, &hl)) && (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) head = hl.pose;
            for (auto& e : entities) {
                XrSpaceLocation sl{XR_TYPE_SPACE_LOCATION};
                if (XR_SUCCEEDED(xrLocateSpace(e.space, local, fs.predictedDisplayTime, &sl)) && (sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                    (sl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) { e.pose = sl.pose; e.located = true; }
            }
            if (depthStarted) {
                XrEnvironmentDepthImageAcquireInfoMETA ai{XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META}; ai.space = local; ai.displayTime = fs.predictedDisplayTime;
                XrEnvironmentDepthImageMETA img{XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META};
                img.views[0].type = img.views[1].type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META;
                XrResult r = xrAcquireEnvironmentDepthImageMETA_(depthProvider, &ai, &img); depthLastResult = (int)r;
                if (r == XR_SUCCESS) { depthImage = img; depthValid = true; depthAcquired++; depthAcquiredSec++; }
            }
            updateHands(fs.predictedDisplayTime);
            updatePlacement();
            Batch occ, over; build(occ, over);
            render(views, occ, over);
            for (int i = 0; i < 2; i++) {
                pv[i].pose = views[i].pose; pv[i].fov = views[i].fov;
                pv[i].subImage.swapchain = eyes[i].chain; pv[i].subImage.imageRect = {{0, 0}, {eyes[i].w, eyes[i].h}};
            }
            proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; proj.space = local; proj.viewCount = 2; proj.views = pv;
            if (quadShown && panelSeen) if (auto* w = wallById(placed.wall)) {  // under the eye buffers, which leave the panel transparent
                V3 c = xform(w->pose, {placed.x, placed.y, 0.02f}); XrPosef qp; qp.orientation = w->pose.orientation; qp.position = {c.x, c.y, c.z};
                V3 a = xform(w->pose, {placed.x, placed.y, 0.02f + panelLayer.radius}); XrPosef cp = qp; cp.position = {a.x, a.y, a.z};
                layers.push_back(panelLayer.layer(local, qp, cp, placed.w, placed.h));
            }
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj));
            frames++; framesSec++;
        }
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO}; ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; ei.layerCount = (uint32_t)layers.size(); ei.layers = layers.data();
        xrEndFrame(session, &ei);
    }

    void status() {
        auto now = std::chrono::steady_clock::now();
        if (now - lastStatus < std::chrono::seconds(1)) return;
        lastStatus = now; fps = framesSec; framesSec = 0; depthAcquiredLastSec = depthAcquiredSec; depthAcquiredSec = 0;
        std::ostringstream j; j.setf(std::ios::fixed); j.precision(3);
        j << "{\"uptime\":" << std::chrono::duration<double>(now - start).count() << ",\"state\":" << (int)state << ",\"running\":" << running
          << ",\"fps\":" << fps << ",\"frames\":" << frames << ",\"passthrough\":" << (ptLayer != XR_NULL_HANDLE)
          << ",\"head\":[" << head.position.x << "," << head.position.y << "," << head.position.z << "]";
        j << ",\"depth\":{\"started\":" << depthStarted << ",\"size\":[" << depthW << "," << depthH << "],\"acquired\":" << depthAcquired
          << ",\"perSecond\":" << depthAcquiredLastSec << ",\"lastResult\":" << depthLastResult << ",\"near\":" << depthImage.nearZ
          << ",\"far\":" << (std::isinf(depthImage.farZ) ? -1.0f : depthImage.farZ) << "}";
        j << ",\"hands\":[";
        for (int h = 0; h < 2; h++) j << (h ? "," : "") << "{\"active\":" << handActive[h] << ",\"aim\":" << (bool)(aim[h].status & XR_HAND_TRACKING_AIM_VALID_BIT_FB)
                                      << ",\"pinch\":" << aim[h].pinchStrengthIndex << ",\"ray\":" << ray[h].valid << ",\"gap\":" << ray[h].gap << ",\"thumb\":" << ray[h].thumb << ",\"thumbMin\":" << ray[h].thumbMin << ",\"thumbMax\":" << ray[h].thumbMax
                                      << ",\"thumbPresses\":" << ray[h].thumbPresses << ",\"pinchPresses\":" << ray[h].pinchPresses << ",\"lockBreaks\":" << ray[h].lockBreaks << ",\"result\":" << handResult[h] << ",\"tracker\":" << (hands[h] != XR_NULL_HANDLE) << "}";
        j << "],\"scene\":{\"state\":\"" << sceneState << "\",\"entities\":[";
        bool first = true;
        for (auto& e : entities) {
            j << (first ? "" : ",") << "{\"uuid\":\"" << e.uuid << "\",\"label\":\"" << jsonEscape(e.label) << "\",\"located\":" << e.located;
            if (e.has2d) j << ",\"rect\":[" << e.r2.offset.x << "," << e.r2.offset.y << "," << e.r2.extent.width << "," << e.r2.extent.height << "]";
            if (e.has3d) j << ",\"box\":[" << e.r3.extent.width << "," << e.r3.extent.height << "," << e.r3.extent.depth << "]";
            j << ",\"pos\":[" << e.pose.position.x << "," << e.pose.position.y << "," << e.pose.position.z << "]}";
            first = false;
        }
        auto pj = [&](const Placement& p) {
            j << "{\"active\":" << p.active << ",\"wall\":\"" << p.wall << "\",\"x\":" << p.x << ",\"y\":" << p.y << ",\"w\":" << p.w << ",\"h\":" << p.h
              << ",\"valid\":" << p.valid << ",\"reason\":\"" << jsonEscape(p.reason) << "\"}";
        };
        j << "]},\"aim\":{\"frames\":" << aimFrames << ",\"hitFrames\":" << hitFrames << ",\"pinchEdges\":" << pinchEdges
          << ",\"o\":[" << lastAimO.x << "," << lastAimO.y << "," << lastAimO.z << "],\"d\":[" << lastAimD.x << "," << lastAimD.y << "," << lastAimD.z
          << "],\"nearestT\":" << lastNearest << ",\"nearest\":\"" << jsonEscape(lastNearestWall) << "\"}";
        j << ",\"pointer\":{\"on\":" << pointer.on << ",\"pressed\":" << pointer.pressed << ",\"u\":" << pointer.u << ",\"v\":" << pointer.v
          << ",\"moves\":" << pointer.moves << ",\"clicks\":" << pointer.clicks << "}";
        j << ",\"video\":" << video.json() << ",\"sharp\":" << video.sharp.json() << ",\"quad\":" << panelLayer.json() << ",\"native\":" << (nativeRes ? 1 : 0) << ",\"hz120\":" << (hz120 ? 1 : 0) << ",\"boost\":" << (boost ? 1 : 0) << ",\"refreshHz\":" << currentHz() << ",\"moonlight\":" << moonlight.json() << ",\"launcher\":\"" << jsonEscape(launcher.get()) << "\",\"hoverButton\":" << hoverButton;
        j << ",\"candidate\":"; pj(candidate); j << ",\"placed\":"; pj(placed);
        j << ",\"battery\":{\"pct\":" << batteryPct << ",\"charging\":" << (charging ? 1 : 0) << ",\"toasts\":" << toasts.size() << "}";
        j << ",\"spell\":{\"on\":" << spellOn << ",\"cw\":" << spellsCw << ",\"ccw\":" << spellsCcw << ",\"progress\":" << spellProgress << ",\"extras\":" << extras.size() << "}";
        j << ",\"voice\":" << dictation.json() << ",\"throws\":" << throws << ",\"enters\":" << enters << ",\"undos\":" << undos;
        j << ",\"errors\":[";
        for (size_t i = 0; i < errors.size(); i++) j << (i ? "," : "") << "\"" << jsonEscape(errors[i]) << "\"";
        j << "]}";
        std::string tmp = gDir + "/status.json.tmp";
        { std::ofstream f(tmp); f << j.str(); }
        rename(tmp.c_str(), (gDir + "/status.json").c_str());
    }

    // files/moonlight/session.json is replaced by moonlight_host.py launch; Sunshine only holds a launched
    // session for a few seconds, so connect as soon as a new one appears. When the stream ends, the Mac relay resumes.
    time_t sessionMtime = 0; long watchTick = 0; bool moonlightActive = false;
    void watchSession() {
        if (watchTick++ % 10) return;
        struct stat st{};
        std::string path = gDir + "/moonlight/session.json";
        if (stat(path.c_str(), &st) == 0 && st.st_mtime != sessionMtime) {
            bool first = sessionMtime == 0; sessionMtime = st.st_mtime;
            if (!first || time(nullptr) - st.st_mtime < 20) {
                moonlight.stop();
                if (moonlight.load(gDir)) {
                    video.relayPaused = true; moonlight.start(&video); moonlightActive = true;
                    event("moonlight session " + moonlight.host + " " + std::to_string(moonlight.width) + "x" + std::to_string(moonlight.height));
                }
            }
        }
        if (moonlightActive && !moonlight.running) {
            moonlightActive = false; moonlight.stop(); video.relayPaused = false; video.source = "mac relay";
            event("moonlight ended: " + moonlight.get(moonlight.error));
        }
    }

    void run(JNIEnv* env) {
        jni = env;
        { std::ofstream(gDir + "/events.log", std::ios::trunc); }
        if (!createInstance(env) || !createEgl()) { status(); return; }
        createGl();
        launcher.dir = gDir;
        video.start();  // Mac desktop relay by default; a freshly pushed Sunshine session switches to the Mini
        video.sharp.start(gDir);  // lossless refinement of still tiles, when files/sharp.json names a sender
        if (!createSession()) { lastStatus = {}; status(); return; }
        loadPlacement();
        while (!gQuit) {
            watchSession();
            pollEvents();
            if (running) frame(); else std::this_thread::sleep_for(std::chrono::milliseconds(10));
            status();
        }
        lastStatus = {}; status();
        event("exiting");
        dictation.shutdown();
        moonlight.stop();
        launcher.shutdown();
        video.shutdown();
        if (session) xrDestroySession(session);
        if (instance) xrDestroyInstance(instance);
    }
};

static void threadMain() {
    JNIEnv* env = nullptr; gVm->AttachCurrentThread(&env, nullptr);
    { App app; app.run(env); }
    if (!gJavaStop) {
        jclass c = env->GetObjectClass(gActivity);
        env->CallVoidMethod(gActivity, env->GetMethodID(c, "finish", "()V"));
    }
    gVm->DetachCurrentThread();
}

extern "C" JNIEXPORT void JNICALL Java_com_pinpin_wallscreen_roomlab_MainActivity_nativeStart(JNIEnv* env, jclass, jobject activity, jstring dir, jboolean capture) {
    env->GetJavaVM(&gVm);
    gActivity = env->NewGlobalRef(activity);
    const char* d = env->GetStringUTFChars(dir, nullptr); gDir = d; env->ReleaseStringUTFChars(dir, d);
    gRequestCapture = capture; gQuit = false; gJavaStop = false;
    gThread = std::thread(threadMain);
}

extern "C" JNIEXPORT void JNICALL Java_com_pinpin_wallscreen_roomlab_MainActivity_nativeStop(JNIEnv*, jclass) {
    gJavaStop = true; gQuit = true;
    if (gThread.joinable()) gThread.join();
}
