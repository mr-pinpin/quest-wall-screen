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
#include "shoulder.h"
#include "house.h"
#include "wallfit.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include "spell.h"
#include "gamestream.h"
#include "panel_layer.h"
#include "budget.h"
#include "gpu_timer.h"
#include "hosts.h"
#include "model_view.h"

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
    GLint uViewProj, uDepthViewProj, uDepth, uOcclude, uLayer, uDepthParams, uVideo, uHasVideo, uAtlas = -1;
    // Extra screens (summoned): one H.264 atlas of the laptop's virtual displays (sharp/../AtlasSender.swift),
    // cropped per screen into its own compositor layer. files/atlas.json: {"host": "<Mac IP>", "port": 57720}.
    VideoStream atlasVideo; bool atlasLaptop = false;
    // lvl and src (the part of the Mac display the cell covers) are protocol v2 (docs/virtual-texturing.md); a v1
    // sender leaves lvl -1 and sw 0. A screen may come as several cells (tiles).
    using AtlasCell = vt::Cell;
    std::map<int, AtlasCell> atlasCells; std::map<int, std::vector<AtlasCell>> atlasTiles; int atlasW = 0, atlasH = 0; std::string atlasMeta;
    std::map<int, PanelLayer> extraLayers; std::set<int> extraLive;
    // VT flag (virtual texturing, headset side; docs/virtual-texturing.md): hidden screens are not re-copied from
    // the atlas every frame and keep their last sharp image; a screen whose cell changes size keeps showing its
    // old image (holdLayers) until the new one has arrived; demand carries the wanted level ("lvl") and the one
    // shown ("have"). files/vt-on forces it for self-tests.
    bool vtMenu = false, vtFile = false; bool vt() const { return vtMenu || vtFile; }
    std::map<int, PanelLayer> holdLayers; long vtHolds = 0, vtSkipped = 0; std::map<int, vt::Resident> vtResident;
    // HERO flag (docs/virtual-texturing.md v3): screens chosen by vt::Heroes leave the atlas for their own stream,
    // demuxed from 0x81 packets into a decoder-only VideoStream each (created on the render thread; packets that
    // arrive first wait from their last IDR). files/hero-on forces it.
    bool heroMenu = false, heroFile = false; bool heroOn() const { return heroMenu || heroFile; }
    // KEYREQ flag (pairs with the sender's noperiodic): demand "keyReq" counts keyframe requests (vt::KeyReq).
    bool keyReqMenu = false, keyReqFile = false; bool keyReqOn() const { return keyReqMenu || keyReqFile; }
    // SHOULDER flag (docs/house-onboarding.md): the shoulder button and its cascading menus; files/shoulder-on forces it.
    bool shoulderMenuFlag = false, shoulderFile = false; bool shoulderOn() const { return shoulderMenuFlag || shoulderFile; }
    // HOUSE flag (room_lab/house.h): the layout is also kept in house.json on the Mini; files/house-on forces it.
    bool houseMenu = false, houseFile = false; bool houseOn() const { return houseMenu || houseFile; }
    // DEPTHGAP flag (room_lab/wallfit.h): read-only; measures how far each screen sits from the real wall behind it.
    bool depthGapMenu = false, depthGapFile = false; bool depthGap() const { return depthGapMenu || depthGapFile; }
    vt::Heroes heroPicker; std::set<int> heroLayout;  // heroLayout: the sender's layout "hero":[ids]
    std::mutex heroLock; std::map<int, std::unique_ptr<VideoStream>> heroStreams; std::map<int, std::deque<std::vector<uint8_t>>> heroPending;
    long heroPackets = 0;
    void onHeroPacket(int s, const uint8_t* p, size_t n) {  // atlas network thread
        std::lock_guard<std::mutex> g(heroLock); heroPackets++;
        auto it = heroStreams.find(s);
        if (it != heroStreams.end()) { it->second->heroPacket(p, n); return; }
        auto& q = heroPending[s];
        if (p[0] & 1) q.clear();
        if (!q.empty() || (p[0] & 1)) q.emplace_back(p, p + n);
        while (q.size() > 240) q.pop_front();
    }
    void syncHeroStreams() {  // render thread: a decoder for every hero in the layout, none for the rest
        std::lock_guard<std::mutex> g(heroLock);
        for (int s : heroLayout) if (!heroStreams.count(s)) {
            auto v = std::make_unique<VideoStream>(); v->relay = false; v->source = "hero " + std::to_string(s); v->start();
            for (auto& pk : heroPending[s]) v->heroPacket(pk.data(), pk.size());
            heroPending.erase(s); heroStreams[s] = std::move(v);
        }
        for (auto it = heroStreams.begin(); it != heroStreams.end();) {
            if (heroLayout.count(it->first)) { ++it; continue; }
            it->second->shutdown(); it = heroStreams.erase(it);
        }
    }
    // BUDGET flag (docs/3d-content.md, room_lab/budget.h): one arbiter splits the link between the atlas, the game
    // stream and asset downloads by mode (wall / cinema / game) and, under frame-time pressure, makes unfocused
    // screens one or two levels coarser (with VT). Frame time: the GPU's (GPUTIME flag, gpu_timer.h) when the driver
    // has timer queries, else frameWorkMs, the CPU time from xrBeginFrame to xrEndFrame. files/budget-on and
    // files/gputime-on force the flags for self-tests.
    bool budgetMenu = false, budgetFile = false; bool budgetOn() const { return budgetMenu || budgetFile; }
    budget::Arbiter arbiter; budget::Output lastBudget; double frameWorkMs = 0;
    // MODELS flag (docs/3d-content.md, model_view.h): a headset-rendered glTF model from files/model.json, streamed
    // at the BUDGET arbiter's asset share (20 Mbit/s without BUDGET), coarse texture first. files/models-on forces it.
    ModelView model; bool modelsMenu = false, modelsFile = false, modelStarted = false; bool models() const { return modelsMenu || modelsFile; }
    // LODPOSE flag (docs/sources.md roadmap 1): a screen's wanted detail from its projection in both axes (distance,
    // viewing angle, its height as well as its width), not width-at-distance only. files/lodpose-on forces it.
    bool lodPoseMenu = false, lodPoseFile = false; bool lodPose() const { return lodPoseMenu || lodPoseFile; }
    bool codecMenu = false, codecFile = false;  // CODEC flag: game streams in H.264 instead of HEVC (applies at the next launch)
    GpuTimer gpuTimer; bool gpuTimeMenu = false, gpuTimeFile = false; bool gpuTime() const { return gpuTimeMenu || gpuTimeFile; }
    double frameMsForBudget() const { return gpuTime() && gpuTimer.valid() ? gpuTimer.ms : frameWorkMs; }
    std::map<int, vt::Level> vtLevels;
    int vtSrcW(int id) { auto it = atlasTiles.find(id); return it == atlasTiles.end() ? 1920 : vt::srcW(it->second); }
    int vtWant(int id, int px, bool vis) { return vtLevels[id].want(vtSrcW(id), px, vis, now()); }
    int vtHave(int id) {  // the level on screen: the cell's lvl (v2), else derived from its width; -1 = nothing yet
        auto L = extraLayers.find(id), H = holdLayers.find(id);
        bool shown = (L != extraLayers.end() && L->second.hasImage) || (H != holdLayers.end() && H->second.hasImage);
        auto c = atlasCells.find(id); if (!shown || c == atlasCells.end()) return -1;
        return vt::levelOf(c->second, vtSrcW(id));
    }
    VideoStream video;
    PanelLayer panelLayer;  // QUAD: the panel as a compositor quad layer
    bool bezel = true;   // BEZEL flag (default: it removed the line completely): a frame over the panel edge, drawn in the eye
                         // buffers above the layer with 4x MSAA, hides the compositor's aliased edge line
    bool bezelFade = false, bezelGlass = false, bezelAmbi = true;   // FADE / GLASS / AMBI bezel styles (grey when all off); AMBI is the user pick
    bool nativeRes = false;  // NATIVE: stream the laptop at its panel's real pixels (2880x1800) instead of 2560x1600
    float currentHz() { float hz = 0; if (xrGetDisplayRefreshRateFB_ && session) xrGetDisplayRefreshRateFB_(session, &hz); return hz; }
    // HOSTRES flag (default on): the laptop's address is resolved, not hard-coded (room_lab/hosts.h): files/hosts.json
    // override, then mDNS <LocalHostName>.local, then the last address that worked, then kLaptopDefault. Re-resolved
    // every 30 s on its own thread; the game stream, the extra displays, the demand and the atlas all follow it.
    static constexpr const char* kLaptopDefault = "192.168.0.104";
    bool hostResMenu = true; mutable std::mutex laptopLock; std::string laptopIp = kLaptopDefault, laptopVia = "default";
    std::thread hostThread; std::atomic<bool> hostStop{false};
    std::string laptop() const { std::lock_guard<std::mutex> g(laptopLock); return laptopIp; }
    std::string laptopViaNow() const { std::lock_guard<std::mutex> g(laptopLock); return laptopVia; }
    void resolveLaptop() {
        hosts::Result r = hosts::laptop(gDir, hostResMenu, kLaptopDefault);
        bool changed;
        { std::lock_guard<std::mutex> g(laptopLock); changed = r.ip != laptopIp; laptopIp = r.ip; laptopVia = r.via; }
        if (atlasLaptop) { std::lock_guard<std::mutex> g(atlasVideo.lock); atlasVideo.directHost = r.ip; }
        if (changed) event("laptop at " + r.ip + " (" + r.via + ")");
    }
    void startHostResolver() {
        resolveLaptop();  // before the streams start (up to ~1 s)
        hostThread = std::thread([this] {
            while (!hostStop) {
                for (int i = 0; i < 300 && !hostStop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!hostStop) resolveLaptop();
            }
        });
    }
    GameStreamHost hostFor(int i) const {
        GameStreamHost h = hosts[i];
        if (i == 1) h.address = laptop();
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
                               {"LAPTOP", kLaptopDefault, 57984, "server-air.pem", 2560, 1600, 60, 80000}};  // direct: the Mini relay is retired
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
    // Each item carries a one-line explanation in plain words, shown under its label (5x7 font: A-Z 0-9 % - . : !).
    struct MenuItem { const char* label; const char* section; const char* hint; };
    static constexpr int kMenuItems = 32;
    const MenuItem menuItems[kMenuItems] = {
        {"MINI", "SOURCE", "SHOW THE MAC MINI"}, {"LAPTOP", "SOURCE", "SHOW THE LAPTOP"},
        {"H264", "SOURCE", "GAME IN H264 NOT HEVC"}, {"HOSTRES", "SOURCE", "FIND THE LAPTOP BY NAME"},
        {"SMALL", "SIZE", "MAIN SCREEN 1 M WIDE"}, {"MEDIUM", "SIZE", "MAIN SCREEN 1.6 M WIDE"}, {"LARGE", "SIZE", "MAIN SCREEN 2.4 M WIDE"},
        {"LOCK", "PANEL", "PIN SCREENS AND THIS PANEL"}, {"QUAD", "PANEL", "SHARPER MAIN SCREEN"}, {"CLOSE", "PANEL", "HIDE THIS PANEL"},
        {"CURVE", "DISPLAY", "BEND THE SCREEN AROUND YOU"},
        {"MARGIN", "BORDER", "SOFT CLEAR EDGE"}, {"SUPER", "BORDER", "SMOOTHER EDGES MORE GPU"}, {"BEZEL", "BORDER", "FRAME AROUND THE SCREEN"},
        {"FADE", "BEZEL", "DARK FRAME THAT FADES"}, {"GLASS", "BEZEL", "FROSTED GLASS FRAME"}, {"AMBI", "BEZEL", "FRAME GLOWS WITH SCREEN COLOURS"},
        {"SPELL", "GESTURES", "DRAW A CIRCLE FOR A NEW SCREEN"}, {"MACDISP", "SCREENS", "EXTRA SCREENS ARE MAC DISPLAYS"},
        {"SAVESET", "MEMORY", "REMEMBER THESE SETTINGS"}, {"RESUME", "MEMORY", "REOPEN LAST SOURCE AT START"},
        {"VT", "EXPERIMENTAL", "SHARP ONLY WHERE YOU LOOK"}, {"BUDGET", "EXPERIMENTAL", "SHARE WI-FI BETWEEN STREAMS"},
        {"GPUTIME", "EXPERIMENTAL", "MEASURE GPU TIME"}, {"WIFILL", "EXPERIMENTAL", "LOW LATENCY WI-FI"},
        {"MODELS", "EXPERIMENTAL", "SHOW A 3D MODEL"}, {"LODPOSE", "EXPERIMENTAL", "DETAIL BY VIEWING ANGLE"},
        {"HERO", "EXPERIMENTAL", "OWN STREAM FOR A BIG SCREEN"}, {"KEYREQ", "EXPERIMENTAL", "ASK FOR KEYFRAMES"},
        {"SHOULDER", "EXPERIMENTAL", "MENU BUTTON BY YOUR SHOULDER"}, {"HOUSE", "EXPERIMENTAL", "SYNC LAYOUT TO THE HOUSE FILE"},
        {"DEPTHGAP", "EXPERIMENTAL", "MEASURE SCREENS VS REAL WALLS"}};
    // SAVESET (default on): every toggle above is kept in settings.json and restored at start. RESUME (default on):
    // the last source pressed (MINI / LAPTOP) is started again at start, instead of an empty main panel.
    bool saveSet = true, resumeOn = true; std::string lastSource, pendingResume; bool resumeDone = false;
    UiGrid menuGrid; std::vector<int> menuShown;  // grid cell k shows menuItems[menuShown[k]]
    bool itemVisible(const std::string& l) const { return !(l == "CLOSE" && editLocked); }  // pinned: nothing to close
    // Highlight state: the active source, or a toggle that is on. -1 means not a toggle.
    int itemOn(const std::string& l) {
        if (l == "MINI" || l == "LAPTOP") return video.source == l;
        if (l == "LOCK") return editLocked;
        if (l == "QUAD") return panelLayer.enabled;
        if (l == "CURVE") return panelLayer.curve;
        if (l == "SPELL") return spellOn;
        if (l == "MACDISP") return macDisplays;
        if (l == "VT") return vt();
        if (l == "BUDGET") return budgetOn();
        if (l == "GPUTIME") return gpuTime();
        if (l == "WIFILL") return wifiLl();
        if (l == "HOSTRES") return hostResMenu;
        if (l == "MODELS") return models();
        if (l == "LODPOSE") return lodPose();
        if (l == "HERO") return heroOn();
        if (l == "KEYREQ") return keyReqOn();
        if (l == "SHOULDER") return shoulderOn();
        if (l == "HOUSE") return houseOn();
        if (l == "DEPTHGAP") return depthGap();
        if (l == "H264") return codecMenu || codecFile;
        if (l == "MARGIN") return panelLayer.margin;
        if (l == "SUPER") return panelLayer.super;
        if (l == "BEZEL") return bezel;
        if (l == "FADE") return bezelFade;
        if (l == "GLASS") return bezelGlass;
        if (l == "AMBI") return bezelAmbi;
        if (l == "SAVESET") return saveSet;
        if (l == "RESUME") return resumeOn;
        return -1;
    }
    void layoutMenu() {
        std::vector<UiGrid::Item> items; menuShown.clear();
        for (int i = 0; i < kMenuItems; i++) if (itemVisible(menuItems[i].label)) { items.push_back({menuItems[i].label, menuItems[i].section, menuItems[i].hint}); menuShown.push_back(i); }
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
          << menuRel.orientation.y << "," << menuRel.orientation.z << "," << menuRel.orientation.w << "]";
        f << ",\"saveset\":" << (saveSet ? 1 : 0) << ",\"resume\":" << (resumeOn ? 1 : 0) << ",\"source\":\"" << lastSource << "\"";
        if (saveSet) {
            f << ",\"toggles\":{";
            const std::pair<const char*, bool> t[] = {{"QUAD", panelLayer.enabled}, {"CURVE", panelLayer.curve}, {"MARGIN", panelLayer.margin},
                {"SUPER", panelLayer.super}, {"BEZEL", bezel}, {"FADE", bezelFade}, {"GLASS", bezelGlass}, {"AMBI", bezelAmbi},
                {"SPELL", spellOn}, {"MACDISP", macDisplays}, {"H264", codecMenu}, {"HOSTRES", hostResMenu}, {"VT", vtMenu},
                {"BUDGET", budgetMenu}, {"GPUTIME", gpuTimeMenu}, {"WIFILL", wifiLlMenu}, {"MODELS", modelsMenu},
                {"LODPOSE", lodPoseMenu}, {"HERO", heroMenu}, {"KEYREQ", keyReqMenu}, {"SHOULDER", shoulderMenuFlag}, {"HOUSE", houseMenu}, {"DEPTHGAP", depthGapMenu}};
            bool first = true; for (auto& [k, v] : t) { f << (first ? "" : ",") << "\"" << k << "\":" << (v ? 1 : 0); first = false; }
            f << "}";
        }
        f << "}";
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
        saveSet = j.find("\"saveset\":0") == std::string::npos;  // absent (a v0.43 or older file): on
        resumeOn = j.find("\"resume\":0") == std::string::npos;
        size_t sp = j.find("\"source\":\""); if (sp != std::string::npos) { sp += 10; lastSource = j.substr(sp, j.find('"', sp) - sp); }
        size_t tp = j.find("\"toggles\":{"); int restored = 0;
        if (saveSet && tp != std::string::npos) {
            auto get = [&](const char* k, bool& v) {
                size_t q = j.find(std::string("\"") + k + "\":", tp); if (q == std::string::npos) return;
                v = j[q + strlen(k) + 3] == '1'; restored++;
            };
            get("QUAD", panelLayer.enabled); get("CURVE", panelLayer.curve); get("MARGIN", panelLayer.margin); get("SUPER", panelLayer.super);
            get("BEZEL", bezel); get("FADE", bezelFade); get("GLASS", bezelGlass); get("AMBI", bezelAmbi); get("SPELL", spellOn);
            get("MACDISP", macDisplays); get("H264", codecMenu); get("HOSTRES", hostResMenu); get("VT", vtMenu); get("BUDGET", budgetMenu);
            get("GPUTIME", gpuTimeMenu); get("WIFILL", wifiLlMenu); get("MODELS", modelsMenu); get("LODPOSE", lodPoseMenu);
            get("HERO", heroMenu); get("KEYREQ", keyReqMenu); get("SHOULDER", shoulderMenuFlag); get("HOUSE", houseMenu); get("DEPTHGAP", depthGapMenu);
        }
        if (resumeOn && (lastSource == "MINI" || lastSource == "LAPTOP")) pendingResume = lastSource;
        event(std::string("settings loaded, locked ") + (editLocked ? "on" : "off") + ", toggles restored " + std::to_string(restored) +
              (pendingResume.empty() ? "" : ", resume " + pendingResume));
    }
    void choose(int i) {
        std::string l = menuItems[i].label;
        auto* w = placed.active ? wallById(placed.wall) : nullptr;
        if (l == "MINI" || l == "LAPTOP") lastSource = l;
        if (l == "MINI") launcher.launch(hostFor(0));
        else if (l == "LAPTOP") launcher.launch(hostFor(1));
        else if (l == "SAVESET") saveSet = !saveSet;
        else if (l == "RESUME") resumeOn = !resumeOn;
        else if (w && (l == "SMALL" || l == "MEDIUM" || l == "LARGE")) {
            resizeTo(l == "SMALL" ? 1.0f : l == "MEDIUM" ? 1.6f : 2.4f, *w); validate(placed, *w); savePlacement();
        } else if (l == "LOCK") editLocked = !editLocked;
        else if (l == "SPELL") { spellOn = !spellOn; spell.clear(); }
        else if (l == "MACDISP") {  // give every extra screen a real Mac display, or take them all away
            macDisplays = !macDisplays;
            for (auto& sc : extras) macDisplay(sc.id, macDisplays);
        }
        else if (l == "CLOSE" && !editLocked) menuOpen = false;
        else if (l == "QUAD") panelLayer.enabled = !panelLayer.enabled;     // A/B the compositor quad layer
        else if (l == "VT") vtMenu = !vtMenu;                                 // virtual texturing for the extra screens
        else if (l == "BUDGET") budgetMenu = !budgetMenu;                     // one arbiter for atlas / game / assets
        else if (l == "GPUTIME") gpuTimeMenu = !gpuTimeMenu;                  // GPU timer queries for the budget's frame time
        else if (l == "H264") codecMenu = !codecMenu;                         // CODEC: next game stream in H.264
        else if (l == "WIFILL") wifiLlMenu = !wifiLlMenu;                     // Wi-Fi low-latency lock while streaming
        else if (l == "MODELS") modelsMenu = !modelsMenu;                     // headset-rendered 3D model (files/model.json)
        else if (l == "LODPOSE") lodPoseMenu = !lodPoseMenu;                  // detail from the screen's projection, both axes
        else if (l == "HERO") heroMenu = !heroMenu;                           // big in-gaze screens get their own stream
        else if (l == "KEYREQ") keyReqMenu = !keyReqMenu;                     // ask the sender for keyframes (noperiodic)
        else if (l == "SHOULDER") { shoulderMenuFlag = !shoulderMenuFlag; if (!shoulderOn()) sm.stack.clear(); }  // shoulder button menus
        else if (l == "HOUSE") { houseMenu = !houseMenu; if (houseOn()) { houseStart(); houseQueueScreens(); } }  // layout to the house file
        else if (l == "DEPTHGAP") depthGapMenu = !depthGapMenu;               // screen vs real wall, in status.json
        else if (l == "HOSTRES") { hostResMenu = !hostResMenu; std::thread([this] { resolveLaptop(); }).detach(); }  // find the laptop by name
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
        if (l == "LOCK" || saveSet || l == "SAVESET" || l == "RESUME" || (resumeOn && (l == "MINI" || l == "LAPTOP"))) saveSettings();
        int on = itemOn(l);
        event("settings: " + l + (on < 0 || l == "MINI" || l == "LAPTOP" ? "" : on ? " on" : " off"));
    }
    // Returns true when the settings panel takes the right hand this frame.
    bool updateMenu() {
        if (!settingsLoaded) { settingsLoaded = true; loadSettings(); loadScreens(); }
        if (!resumeDone && !pendingResume.empty() && placed.active && std::chrono::steady_clock::now() - start > std::chrono::seconds(4)) {
            resumeDone = true; event("resume: " + pendingResume);
            launcher.launch(hostFor(pendingResume == "MINI" ? 0 : 1));
        }
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
    // ---- Shoulder menu (flag SHOULDER; room_lab/shoulder.h, docs/house-onboarding.md). A button by the shoulder opens a
    // cascading stack of panels that drive the screens and every setting; each button explains itself in one line.
    // With the flag off nothing here runs and nothing is drawn.
    shoulder::Menu sm; bool stickyMove = false, smButtonHover = false, smPokePrev = false; shoulder::Hit smHover; long smPresses = 0;
    static shoulder::P3 sp(V3 v) { return {v.x, v.y, v.z}; }
    static V3 vp(shoulder::P3 p) { return {p.x, p.y, p.z}; }
    int extraIndex(int id) const { for (size_t i = 0; i < extras.size(); i++) if (extras[i].id == id) return (int)i; return -1; }
    shoulder::Level shoulderLevel(const std::string& n) {
        shoulder::Level L; L.title = n;
        if (n == "ROOT") {
            L.title = "MENU";
            L.items = {{"SCREENS", "SIZE AND MOVE SCREENS", "open:SCREENS"}, {"SETTINGS", "EVERY SWITCH", "open:SETTINGS"}, {"CLOSE", "HIDE THIS MENU", "close"}};
        } else if (n == "SCREENS") {
            if (placed.active) L.items.push_back({"MAIN", "THE BIG SCREEN", "open:SCREEN 1"});
            for (auto& e : extras) L.items.push_back({"SCREEN " + std::to_string(e.id), "AN EXTRA SCREEN", "open:SCREEN " + std::to_string(e.id)});
            L.items.push_back({"BACK", "ONE MENU BACK", "back"});
        } else if (n == "SETTINGS") {
            L.grid.cols = 5;  // 30 switches: wide rather than tall
            for (auto& m : menuItems) if (std::string(m.label) != "CLOSE") L.items.push_back({m.label, m.hint, std::string("set:") + m.label});
            L.items.push_back({"BACK", "ONE MENU BACK", "back"});
        } else if (n.rfind("SCREEN ", 0) == 0) {
            L.items = {{"BIGGER", "20% WIDER", "bigger"}, {"SMALLER", "20% NARROWER", "smaller"}, {"MOVE", "POINT AT A WALL THEN PINCH", "move"}};
            if (n != "SCREEN 1") L.items.push_back({"DELETE", "REMOVE THIS SCREEN", "delete"});
            L.items.push_back({"BACK", "ONE MENU BACK", "back"});
        }
        return L;
    }
    // Returns true when the shoulder button or one of its panels takes the right hand this frame.
    bool updateShoulder() {
        smButtonHover = false; smHover = {};
        if (!shoulderOn()) { sm.stack.clear(); smPokePrev = false; return false; }
        if (!sm.build) sm.build = [this](const std::string& n) { return shoulderLevel(n); };
        V3 hp = v3(head.position), gaze = rot(head.orientation, {0, 0, -1});
        bool edge = handActive[1] && ray[1].pinch && !pinchPrev[1];
        bool poke = false;  // either index tip touching the button: one press per touch
        for (int h = 0; h < 2; h++) if (handActive[h]) poke = poke || sm.buttonPoke(sp(hp), sp(gaze), sp(v3(joints[h][XR_HAND_JOINT_INDEX_TIP_EXT].pose.position)));
        bool pokeEdge = poke && !smPokePrev; smPokePrev = poke;
        if (handActive[1] && ray[1].valid) smButtonHover = sm.buttonHit(sp(hp), sp(gaze), sp(ray[1].o), sp(ray[1].d));
        if (pokeEdge || (smButtonHover && edge)) {
            sm.toggle(sp(hp), sp(gaze)); smPresses++;
            event(std::string("shoulder menu ") + (sm.open() ? "opened" : "closed"));
            return true;
        }
        if (smButtonHover || poke) return true;
        if (!sm.open() || !(handActive[1] && ray[1].valid)) return false;
        smHover = sm.hit(sp(ray[1].o), sp(ray[1].d));
        if (smHover.level < 0) return false;
        if (edge) {
            std::string top = sm.stack.back().name, a = sm.press(smHover);
            if (!a.empty()) shoulderAction(top, a);
            if (sm.open()) sm.refresh();
        }
        return true;
    }
    void shoulderAction(const std::string& level, const std::string& a) {
        const float cyan[4] = {0.35f, 0.85f, 1.0f, 1}, red[4] = {1.0f, 0.3f, 0.25f, 1};
        event("shoulder: " + level + " " + a);
        if (a.rfind("set:", 0) == 0) { for (int i = 0; i < kMenuItems; i++) if (a.substr(4) == menuItems[i].label) { choose(i); break; } return; }
        if (level.rfind("SCREEN ", 0) != 0) return;
        int id = atoi(level.c_str() + 7);
        if (id == 1) {
            Entity* w = placed.active ? wallById(placed.wall) : nullptr; if (!w) return;
            if (a == "bigger" || a == "smaller") { resizeTo(placed.w * (a == "bigger" ? 1.2f : 1 / 1.2f), *w); validate(placed, *w); savePlacement(); }
            else if (a == "move") { sm.stack.clear(); moving = true; stickyMove = true; toast("shoulder", "POINT AT A WALL THEN PINCH", cyan, 3); }
            return;
        }
        int i = extraIndex(id); if (i < 0) return;
        Placement& p = extras[i].p; Entity* w = wallById(p.wall);
        if ((a == "bigger" || a == "smaller") && w) {
            Placement q = p; q.w = std::fmax(p.w * (a == "bigger" ? 1.2f : 1 / 1.2f), 0.3f); q.h = q.w * p.h / p.w; validate(q, *w);
            if (q.valid) { p = q; saveScreens(); } else toast("shoulder", "NO ROOM: " + q.reason, red, 3);
        } else if (a == "move") { sm.stack.clear(); extraMoving = i; stickyMove = true; toast("shoulder", "POINT AT A WALL THEN PINCH", cyan, 3); }
        else if (a == "delete") {
            extras.erase(extras.begin() + i); saveScreens();
            if (macDisplays) macDisplay(id, false);
            event("dismissed screen " + std::to_string(id)); sm.pop();
        }
    }
    void drawShoulder(Batch& b) {
        if (!shoulderOn()) return;
        V3 hp = v3(head.position), gaze = rot(head.orientation, {0, 0, -1});
        V3 c = vp(sm.button(sp(hp), sp(gaze))), toHead = norm(hp - c), ax = norm(cross({0, 1, 0}, toHead)), ay = cross(toHead, ax);
        const float idleB[4] = {0.75f, 0.8f, 0.9f, 0.85f}, hotB[4] = {0.3f, 0.75f, 1.0f, 1.0f};
        const float* col = smButtonHover || sm.open() ? hotB : idleB; float r = 0.025f;
        for (int i = 0; i < 24; i++) {
            float a0 = i * 0.2618f, a1 = (i + 1) * 0.2618f;
            b.line(c + ax * (r * std::cos(a0)) + ay * (r * std::sin(a0)), c + ax * (r * std::cos(a1)) + ay * (r * std::sin(a1)), toHead, 0.004f, col);
        }
        for (int k = -1; k <= 1; k++) b.line(c + ax * -0.011f + ay * (k * 0.008f), c + ax * 0.011f + ay * (k * 0.008f), toHead, 0.003f, col);
        for (size_t k = 0; k < sm.stack.size(); k++) {
            const shoulder::Level& L = sm.stack[k]; bool top = k + 1 == sm.stack.size();
            Entity pe; pe.pose.orientation = {0, std::sin(L.yaw / 2), 0, std::cos(L.yaw / 2)}; pe.pose.position = {L.centre.x, L.centre.y, L.centre.z};
            float s = L.scale, hw = L.grid.halfW * s, hh = L.grid.halfH * s, fade = top ? 1.0f : 0.45f;
            auto P = [&](float x, float y, float z) { return xform(pe.pose, {x, y, z}); };
            const float bg[4] = {0.05f, 0.06f, 0.08f, 0.8f * fade}, white[4] = {0.95f, 0.97f, 1.0f, fade}, dim[4] = {0.62f, 0.68f, 0.78f, fade};
            b.quad(P(-hw, -hh, 0), P(hw, -hh, 0), P(hw, hh, 0), P(-hw, hh, 0), bg);
            text(b, pe, 0, L.grid.titleY * s, 0.003f * s, UiGrid::fit(L.title, L.grid.titlePx, 2 * L.grid.halfW - 2 * L.grid.pad, L.grid.titleH * 0.6f) * s, L.title.c_str(), white);
            for (size_t i = 0; i < L.items.size() && i < L.grid.cells.size(); i++) {
                const UiGrid::Cell& cl = L.grid.cells[i]; const shoulder::Item& it = L.items[i];
                bool on = it.action.rfind("set:", 0) == 0 && itemOn(it.action.substr(4)) > 0;
                const float idle[4] = {0.16f, 0.18f, 0.24f, 0.95f}, act[4] = {0.10f, 0.45f, 0.22f, 0.95f};
                float f[4]; for (int q = 0; q < 4; q++) f[q] = on ? act[q] : idle[q];
                f[3] *= fade;
                if (top && smHover.level == (int)k && smHover.item == (int)i) for (int q = 0; q < 3; q++) f[q] = std::fmin(1.0f, f[q] + 0.2f);
                float x0 = (cl.x - cl.w / 2) * s, x1 = (cl.x + cl.w / 2) * s, y0 = (cl.y - cl.h / 2) * s, y1 = (cl.y + cl.h / 2) * s;
                b.quad(P(x0, y0, 0.002f * s), P(x1, y0, 0.002f * s), P(x1, y1, 0.002f * s), P(x0, y1, 0.002f * s), f);
                text(b, pe, cl.x * s, cl.labelY * s, 0.004f * s, cl.px * s, it.label.c_str(), white);
                if (!cl.hint1.empty()) text(b, pe, cl.x * s, cl.hint1Y * s, 0.004f * s, cl.hintPx * s, cl.hint1.c_str(), dim);
                if (!cl.hint2.empty()) text(b, pe, cl.x * s, cl.hint2Y * s, 0.004f * s, cl.hintPx * s, cl.hint2.c_str(), dim);
            }
        }
    }
    // ---- DEPTHGAP (flag; read-only). Twice a second the environment depth of both eyes is copied to the CPU: a
    // fullscreen pass texelFetches the depth array into a 512x256 RGBA8 target (eye 0 left, eye 1 right; d*65535 as
    // two bytes), read back through a PBO with a fence, so the render thread never waits. A worker thread feeds
    // wallfit.h: fused planes from depth, and for every screen the signed gap between the screen and the measured
    // surface over its rect (positive = the real surface is in front, i.e. the screen is sunk), with corner medians to
    // tell a yaw/relocalisation error from a plane offset. Results go to status.json "depthGap". files/depth-dump writes
    // the next frame (both eyes, raw d16 + meta.json with fov, poses, near/far and the scene walls) to files/depth-<t>/.
    // The depth image carries no capture time of its own: timeNs is the display time it was acquired for.
    static constexpr int kRB = 256;
    XrTime frameTime = 0;
    GLuint rbProg = 0, rbFbo = 0, rbTex = 0, rbVao = 0, rbPbo[2] = {0, 0}; GLint rbLocD = -1; GLsync rbFence[2] = {nullptr, nullptr};
    bool rbBusy[2] = {false, false}; long rbLast = -1000, rbReads = 0; std::string rbErr;
    struct GapScreen { int id; std::string wall; wallfit::Pose pose; float x, y, w, h; };
    struct SceneWall { std::string uuid, label; wallfit::Pose pose, centre; float w, h; };
    struct DepthShot { XrEnvironmentDepthImageMETA img{}; XrTime now = 0; std::vector<uint16_t> d[2]; std::vector<GapScreen> screens; std::vector<SceneWall> walls; XrPosef head{}; bool dump = false; };
    DepthShot rbShot[2];
    std::mutex gapLock; std::condition_variable gapCv; std::unique_ptr<DepthShot> gapNext; bool gapThread = false; std::string gapJson;
    bool depthDumpWanted = false;
    static wallfit::Pose wp(const XrPosef& p) { return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}}; }
    void depthReadback(GLuint depthTex) {
        for (int k = 0; k < 2; k++) if (rbBusy[k]) {  // finished reads first
            GLenum st = glClientWaitSync(rbFence[k], 0, 0);
            if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED) continue;
            glDeleteSync(rbFence[k]); rbFence[k] = nullptr; rbBusy[k] = false;
            glBindBuffer(GL_PIXEL_PACK_BUFFER, rbPbo[k]);
            const uint8_t* m = (const uint8_t*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 2 * kRB * kRB * 4, GL_MAP_READ_BIT);
            if (m) {
                auto shot = std::make_unique<DepthShot>(std::move(rbShot[k]));
                for (int e = 0; e < 2; e++) {
                    shot->d[e].resize(kRB * kRB);
                    for (int y = 0; y < kRB; y++) for (int x = 0; x < kRB; x++) {
                        const uint8_t* px = m + ((size_t)y * 2 * kRB + e * kRB + x) * 4;
                        shot->d[e][(size_t)y * kRB + x] = (uint16_t)(px[0] << 8 | px[1]);
                    }
                }
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER); rbReads++;
                gapSubmit(std::move(shot));
            } else rbErr = "map failed";
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
        if (frames - rbLast < 36 && !depthDumpWanted) return;
        int k = !rbBusy[0] ? 0 : !rbBusy[1] ? 1 : -1; if (k < 0) return;
        if (!rbProg) {
            const char* vs = "#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
            const char* fs = "#version 300 es\nprecision highp float; precision highp int; precision highp sampler2DArray;\n"
                "uniform sampler2DArray uD; out vec4 o;\n"
                "void main(){ int e=int(gl_FragCoord.x)>=256?1:0; vec2 q=(vec2(gl_FragCoord.x-float(e*256),gl_FragCoord.y))/256.0;\n"
                " ivec2 sz=textureSize(uD,0).xy; ivec2 t=clamp(ivec2(q*vec2(sz)),ivec2(0),sz-1);\n"
                " uint v=uint(clamp(texelFetch(uD,ivec3(t,e),0).r,0.0,1.0)*65535.0+0.5);\n"
                " o=vec4(float(v>>8u)/255.0,float(v&255u)/255.0,0.0,1.0);}";
            rbProg = glCreateProgram();
            GLuint a = compile(GL_VERTEX_SHADER, vs), b = compile(GL_FRAGMENT_SHADER, fs);
            glAttachShader(rbProg, a); glAttachShader(rbProg, b); glLinkProgram(rbProg);
            GLint ok = 0; glGetProgramiv(rbProg, GL_LINK_STATUS, &ok); if (!ok) { rbErr = "link failed"; return; }
            rbLocD = glGetUniformLocation(rbProg, "uD");
            glGenVertexArrays(1, &rbVao);
            glGenTextures(1, &rbTex); glBindTexture(GL_TEXTURE_2D, rbTex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 2 * kRB, kRB);
            glGenFramebuffers(1, &rbFbo);
            GLint prevFb = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFb);
            glBindFramebuffer(GL_FRAMEBUFFER, rbFbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rbTex, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) rbErr = "fbo incomplete";
            glBindFramebuffer(GL_FRAMEBUFFER, prevFb);
            glGenBuffers(2, rbPbo);
            for (int i = 0; i < 2; i++) { glBindBuffer(GL_PIXEL_PACK_BUFFER, rbPbo[i]); glBufferData(GL_PIXEL_PACK_BUFFER, 2 * kRB * kRB * 4, nullptr, GL_STREAM_READ); }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_ARRAY, depthTex);
            if (!rbErr.empty()) return;
        }
        rbLast = frames;
        GLint prevFb = 0, prevProg = 0, prevVao = 0, vp[4]; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFb); glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao); glGetIntegerv(GL_VIEWPORT, vp);
        GLboolean dt = glIsEnabled(GL_DEPTH_TEST), bl = glIsEnabled(GL_BLEND), sc = glIsEnabled(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, rbFbo); glViewport(0, 0, 2 * kRB, kRB);
        glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
        glUseProgram(rbProg); glBindVertexArray(rbVao);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_ARRAY, depthTex); glUniform1i(rbLocD, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, rbPbo[k]); glReadPixels(0, 0, 2 * kRB, kRB, GL_RGBA, GL_UNSIGNED_BYTE, nullptr); glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        rbFence[k] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); rbBusy[k] = true;
        glBindFramebuffer(GL_FRAMEBUFFER, prevFb); glViewport(vp[0], vp[1], vp[2], vp[3]); glUseProgram(prevProg); glBindVertexArray(prevVao);
        if (dt) glEnable(GL_DEPTH_TEST); if (bl) glEnable(GL_BLEND); if (sc) glEnable(GL_SCISSOR_TEST);
        // What the depth was taken against: the screens and scene walls as they stand now.
        DepthShot& sh = rbShot[k]; sh = DepthShot{}; sh.img = depthImage; sh.now = frameTime; sh.head = head; sh.dump = depthDumpWanted; depthDumpWanted = false;
        auto addScreen = [&](int id, const Placement& p) {
            Entity* w = p.active ? wallById(p.wall) : nullptr; if (!w || !w->located) return;
            sh.screens.push_back({id, p.wall, wp(w->pose), p.x, p.y, p.w, p.h});
        };
        addScreen(1, placed); for (auto& e : extras) addScreen(e.id, e.p);
        for (auto& e : entities) if (e.located && e.has2d) {
            V3 c = xform(e.pose, {e.r2.offset.x + e.r2.extent.width / 2, e.r2.offset.y + e.r2.extent.height / 2, 0});
            XrPosef cp = e.pose; cp.position = {c.x, c.y, c.z};
            sh.walls.push_back({e.uuid, e.label, wp(e.pose), wp(cp), e.r2.extent.width, e.r2.extent.height});
        }
    }
    void gapSubmit(std::unique_ptr<DepthShot> shot) {
        std::lock_guard<std::mutex> g(gapLock);
        gapNext = std::move(shot); gapCv.notify_one();  // latest only: an unprocessed older shot is dropped
        if (gapThread) return;
        gapThread = true;
        std::thread([this] {
            wallfit::Fuser fuser; std::map<int, std::vector<float>> recent;  // per screen: the last medians
            for (;;) {
                std::unique_ptr<DepthShot> sh;
                { std::unique_lock<std::mutex> g(gapLock); gapCv.wait(g, [&] { return gapNext != nullptr; }); sh = std::move(gapNext); }
                auto t0 = std::chrono::steady_clock::now();
                wallfit::DepthFrame f[2];
                for (int e = 0; e < 2; e++) {
                    const auto& v = sh->img.views[e];
                    f[e].w = f[e].h = kRB; f[e].d16 = sh->d[e].data();
                    f[e].fov = {v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown}; f[e].pose = wp(v.pose);
                    f[e].nearZ = sh->img.nearZ; f[e].farZ = sh->img.farZ; f[e].timeNs = (int64_t)sh->now;
                    fuser.add(f[e], (int64_t)sh->now);
                }
                std::vector<wallfit::Wall> fused = fuser.walls();
                std::vector<wallfit::HouseWall> scene; for (auto& w : sh->walls) scene.push_back({w.centre, w.w, w.h});
                std::ostringstream j; j.setf(std::ios::fixed); j.precision(3);
                auto num = [](float v) { return std::isfinite(v) ? v : -99.0f; };
                j << "\"fused\":" << fused.size() << ",\"screens\":[";
                bool first = true;
                for (auto& s : sh->screens) {
                    wallfit::Gap g = wallfit::screenGap(f[0], s.pose, s.x, s.y, s.w, s.h), g1 = wallfit::screenGap(f[1], s.pose, s.x, s.y, s.w, s.h);
                    if (g1.valid > g.valid) g = g1;
                    // The fused plane that is this screen's scene wall (loose: the point is to measure how far off it is).
                    int si = -1; for (size_t i = 0; i < sh->walls.size(); i++) if (sh->walls[i].uuid == s.wall) si = (int)i;
                    std::string plane = "null";
                    if (si >= 0) for (auto& w : fused) {
                        wallfit::Match m = wallfit::match(w, {scene[si]}, 0.30f, 10.0f, 0.5f);
                        if (m.index < 0) continue;
                        wallfit::Gap gp = wallfit::screenGapPlane(w, s.pose, s.x, s.y, s.w, s.h);
                        std::ostringstream q; q.setf(std::ios::fixed); q.precision(3);
                        q << "{\"median\":" << num(gp.median) << ",\"tl\":" << num(gp.tl) << ",\"tr\":" << num(gp.tr) << ",\"bl\":" << num(gp.bl) << ",\"br\":" << num(gp.br)
                          << ",\"wallOffset\":" << m.offset << ",\"wallDeg\":" << m.angleDeg << ",\"rms\":" << w.rms_m << ",\"conf\":" << w.confidence << "}";
                        plane = q.str(); break;
                    }
                    auto& r = recent[s.id]; if (g.n) { r.push_back(g.median); if (r.size() > 20) r.erase(r.begin()); }
                    std::vector<float> sr = r; std::sort(sr.begin(), sr.end());
                    j << (first ? "" : ",") << "{\"id\":" << s.id << ",\"wall\":\"" << s.wall.substr(0, 8) << "\",\"median\":" << num(g.median) << ",\"p10\":" << num(g.p10) << ",\"p90\":" << num(g.p90)
                      << ",\"valid\":" << g.valid << ",\"tl\":" << num(g.tl) << ",\"tr\":" << num(g.tr) << ",\"bl\":" << num(g.bl) << ",\"br\":" << num(g.br)
                      << ",\"median20\":" << (sr.empty() ? -99.0f : sr[sr.size() / 2]) << ",\"plane\":" << plane << "}";
                    first = false;
                }
                float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
                j << "],\"workerMs\":" << ms << ",\"dropped\":" << fuser.dropped;
                if (sh->dump) depthDump(*sh);
                std::lock_guard<std::mutex> g(gapLock); gapJson = j.str();
            }
        }).detach();
    }
    void depthDump(const DepthShot& sh) {
        std::string dir = gDir + "/depth-" + std::to_string((long long)(sh.now / 1000000));
        mkdir(dir.c_str(), 0755);
        for (int e = 0; e < 2; e++) house::writeAtomic(dir + (e ? "/right.d16" : "/left.d16"), std::string((const char*)sh.d[e].data(), sh.d[e].size() * 2));
        std::ostringstream j; j.precision(9);
        auto pose = [&](const XrPosef& p) {
            std::ostringstream q; q.precision(9);
            q << "{\"p\":[" << p.position.x << "," << p.position.y << "," << p.position.z << "],\"q\":[" << p.orientation.x << "," << p.orientation.y << "," << p.orientation.z << "," << p.orientation.w << "]}";
            return q.str();
        };
        auto wpose = [&](const wallfit::Pose& p) {
            std::ostringstream q; q.precision(9);
            q << "{\"p\":[" << p.p.x << "," << p.p.y << "," << p.p.z << "],\"q\":[" << p.q.x << "," << p.q.y << "," << p.q.z << "," << p.q.w << "]}";
            return q.str();
        };
        j << "{\"w\":" << kRB << ",\"h\":" << kRB << ",\"format\":\"uint16 little-endian, d*65535, row 0 = bottom\",\"near\":" << sh.img.nearZ << ",\"far\":"
          << (std::isinf(sh.img.farZ) ? -1.0f : sh.img.farZ) << ",\"timeNs\":" << (long long)sh.now << ",\"nowNs\":" << (long long)sh.now << ",\"eyes\":[";
        for (int e = 0; e < 2; e++) {
            const auto& v = sh.img.views[e];
            j << (e ? "," : "") << "{\"fov\":[" << v.fov.angleLeft << "," << v.fov.angleRight << "," << v.fov.angleUp << "," << v.fov.angleDown << "],\"pose\":" << pose(v.pose) << "}";
        }
        j << "],\"head\":" << pose(sh.head) << ",\"walls\":[";
        for (size_t i = 0; i < sh.walls.size(); i++)
            j << (i ? "," : "") << "{\"uuid\":\"" << sh.walls[i].uuid << "\",\"label\":\"" << sh.walls[i].label << "\",\"pose\":" << wpose(sh.walls[i].pose)
              << ",\"centre\":" << wpose(sh.walls[i].centre) << ",\"w\":" << sh.walls[i].w << ",\"h\":" << sh.walls[i].h << "}";
        j << "],\"screens\":[";
        for (size_t i = 0; i < sh.screens.size(); i++)
            j << (i ? "," : "") << "{\"id\":" << sh.screens[i].id << ",\"wall\":\"" << sh.screens[i].wall << "\",\"x\":" << sh.screens[i].x << ",\"y\":" << sh.screens[i].y
              << ",\"w\":" << sh.screens[i].w << ",\"h\":" << sh.screens[i].h << "}";
        j << "]}";
        house::writeAtomic(dir + "/meta.json", j.str());
        event("depth dump written to " + dir);
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
uniform sampler2D uVideo; uniform int uHasVideo; uniform sampler2D uAtlas;
in vec4 vCol; in vec2 vUv; in vec4 vDepthClip;
uniform sampler2DArray uDepth; uniform int uOcclude; uniform float uLayer; uniform vec3 uDepthParams;
out vec4 oColor;
void main(){
  vec4 c=vCol;
  if(vUv.x<-15.0){  // AMBI bezel of an extra screen: colour from its cell of the atlas (UVs already in atlas space)
    c=vec4(textureLod(uAtlas,clamp(vec2(vUv.x+20.0,vUv.y),0.002,0.998),5.0).rgb*0.85,vCol.a);
  } else if(vUv.x<-5.0){  // AMBI bezel: colour from the screen's nearest edge (blurred by a high mip), alpha from the vertex
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
        uVideo = glGetUniformLocation(program, "uVideo"); uHasVideo = glGetUniformLocation(program, "uHasVideo"); uAtlas = glGetUniformLocation(program, "uAtlas");
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
        if (houseOn() && !houseApplying) houseQueueScreens();
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
    // WIFILL flag: while a stream plays (game stream or extra screens), hold a WifiLock in WIFI_MODE_FULL_LOW_LATENCY
    // (Android 10+): Wi-Fi power save off and the driver's low-latency mode, so packets don't wait for the next
    // beacon wake-up (idle Quest pings: 50-100 ms, half lost). Released when nothing streams. files/wifill-on forces it.
    bool wifiLlMenu = false, wifiLlFile = false; bool wifiLl() const { return wifiLlMenu || wifiLlFile; }
    jobject wifiLock = nullptr; bool wifiLockHeld = false; std::string wifiLockErr; std::chrono::steady_clock::time_point wifiAt{};
    void pollWifiLock() {
        auto t = std::chrono::steady_clock::now();
        if (!jni || !gActivity || t - wifiAt < std::chrono::seconds(2)) return;
        wifiAt = t;
        bool want = wifiLl() && (moonlight.get(moonlight.stage) == "streaming" || !extraLive.empty());
        if (want == wifiLockHeld) return;
        JNIEnv* e = jni;
        if (!wifiLock) {
            jclass ac = e->GetObjectClass(gActivity);
            jmethodID gss = e->GetMethodID(ac, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
            jstring name = e->NewStringUTF("wifi"), tag = e->NewStringUTF("pinpin-stream");
            jobject wm = gss ? e->CallObjectMethod(gActivity, gss, name) : nullptr;
            if (wm && !e->ExceptionCheck()) {
                jclass wc = e->GetObjectClass(wm);
                jmethodID cwl = e->GetMethodID(wc, "createWifiLock", "(ILjava/lang/String;)Landroid/net/wifi/WifiManager$WifiLock;");
                jobject lock = cwl ? e->CallObjectMethod(wm, cwl, 4, tag) : nullptr;  // 4 = WIFI_MODE_FULL_LOW_LATENCY
                if (lock && !e->ExceptionCheck()) {
                    jclass lc = e->GetObjectClass(lock);
                    jmethodID rc = e->GetMethodID(lc, "setReferenceCounted", "(Z)V");
                    if (rc) e->CallVoidMethod(lock, rc, (jboolean)false);
                    wifiLock = e->NewGlobalRef(lock); e->DeleteLocalRef(lc);
                }
                if (lock) e->DeleteLocalRef(lock);
                e->DeleteLocalRef(wc);
            }
            if (e->ExceptionCheck()) { e->ExceptionClear(); wifiLockErr = "createWifiLock failed"; }
            if (wm) e->DeleteLocalRef(wm);
            e->DeleteLocalRef(name); e->DeleteLocalRef(tag); e->DeleteLocalRef(ac);
            if (!wifiLock) return;
        }
        jclass lc = e->GetObjectClass(wifiLock);
        jmethodID m = e->GetMethodID(lc, want ? "acquire" : "release", "()V");
        if (m) e->CallVoidMethod(wifiLock, m);
        if (e->ExceptionCheck()) { e->ExceptionClear(); wifiLockErr = want ? "acquire failed" : "release failed"; }
        else { wifiLockHeld = want; wifiLockErr.clear(); event(std::string("wifi low-latency lock ") + (want ? "held" : "released")); }
        e->DeleteLocalRef(lc);
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
    // Each extra screen can be a real display on the laptop (mac/pinpin-displays: CGVirtualDisplay behind a small
    // HTTP API on :57710), streamed back through the atlas (AtlasSender.swift). MACDISP is on by default now that the
    // headset shows those displays; turning it off removes them so nothing wanders onto a monitor nobody can see.
    bool macDisplays = true; std::atomic<long> displayCalls{0}, displayFailures{0};
    void macDisplay(int id, bool create) {
        // 1x: the atlas cell is 1920x1200 anyway, and a 2x display makes YouTube pull 1440p/4K over a ~30 Mbps house line.
        std::string path = create ? "/displays?id=" + std::to_string(id) + "&w=1920&h=1200&scale=1" : "/remove?id=" + std::to_string(id);
        std::string host = laptop();
        std::thread([this, host, path] {
            int fd = socket(AF_INET, SOCK_STREAM, 0); bool ok = false;
            timeval tv{5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(57710); inet_pton(AF_INET, host.c_str(), &a.sin_addr);
            if (fd >= 0 && connect(fd, (sockaddr*)&a, sizeof a) == 0) {
                std::string req = "POST " + path + " HTTP/1.0\r\nHost: " + host + "\r\nContent-Length: 0\r\n\r\n";
                if (send(fd, req.data(), req.size(), 0) == (ssize_t)req.size()) { char buf[256] = {}; ok = recv(fd, buf, sizeof buf - 1, 0) > 0 && strstr(buf, " 200 "); }
            }
            if (fd >= 0) close(fd);
            displayCalls++; if (!ok) displayFailures++;
        }).detach();
        event(std::string(create ? "mac display for screen " : "remove mac display of screen ") + std::to_string(id));
    }
    std::vector<Screen> extras; int nextScreenId = 2;
    bool spellOn = true; SpellRecognizer spell; float spellCooldown = 0, spellProgress = 0; int spellTurn = 0;
    struct Burst { V3 c, ax, ay; float r; float col[4]; std::chrono::steady_clock::time_point t0; };
    std::vector<Burst> bursts;
    long spellsCw = 0, spellsCcw = 0;
    int extraBarHover = -1, extraMoving = -1; float extraHold = 0; std::chrono::steady_clock::time_point extraStart{};
    // Pointer on an extra screen: its display is not the one Sunshine streams (and Sunshine refuses clicks off its
    // own display), so the laptop helper "PinPin Displays" (UDP :57711) moves the pointer and posts the click
    // there. Keys from the stream reach the focused window on any display.
    std::chrono::steady_clock::time_point extraClickAt{};
    int extraPtr = -1, mouseSock = -1, sentU = -1, sentV = -1; float eu = 0.5f, ev = 0.5f; long extraClicks = 0, extraMoves = 0;
    void sendMouse(char op, int id, float u, float v) {
        if (mouseSock < 0) mouseSock = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(57711); inet_pton(AF_INET, laptop().c_str(), &a.sin_addr);
        char msg[64]; int n = snprintf(msg, sizeof msg, "%c %d %d %d", op, id, (int)(u * 10000), (int)(v * 10000));
        sendto(mouseSock, msg, n, 0, (sockaddr*)&a, sizeof a);
    }
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
        if (macDisplays) macDisplay(id, true);
        burst(c, r, gold); toast("spell", "SCREEN " + std::to_string(id) + " SUMMONED", gold, 3);
        event("summoned screen " + std::to_string(id) + " on wall " + np.wall + " w " + std::to_string(np.w));
    }
    void counterSpell(V3 c, float r) {
        const float violet[4] = {0.75f, 0.45f, 1.0f, 1};
        if (moving || menuMoving || extraMoving >= 0) {
            moving = false; candidate.active = false; extraMoving = -1; stickyMove = false;
            if (menuMoving) { menuMoving = false; menuRestored = false; }  // the saved pose comes back next frame
            burst(c, r, violet); toast("spell", "CANCELLED", violet, 2); event("counter-spell: cancelled move"); return;
        }
        V3 eye = v3(head.position); float hx, hy;
        int i = extraAt(eye, norm(c - eye), hx, hy);
        if (i >= 0) {
            int id = extras[i].id; extras.erase(extras.begin() + i); saveScreens();
            if (macDisplays) macDisplay(id, false);
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
            if (stickyMove ? (p1 && !pinchPrev[1]) : !p1) {
                stickyMove = false;
                if (candidate.active && candidate.valid) { p.wall = candidate.wall; p.x = candidate.x; p.y = candidate.y; saveScreens(); event("moved screen " + std::to_string(extras[extraMoving].id)); }
                extraMoving = -1; candidate.active = false;
            }
            return true;
        }
        extraMoving = -1;
        if (!(handActive[1] && ray[1].valid)) { extraPtr = -1; return false; }
        float hx, hy; Entity* w = rayWall(ray[1].o, ray[1].d, hx, hy); if (!w) { extraPtr = -1; return false; }
        for (size_t i = 0; i < extras.size() && !editLocked; i++) {  // grab bars only while unlocked; pointing always
            const Placement& p = extras[i].p;
            if (p.wall != w->uuid) continue;
            if (std::fabs(hx - p.x) < kBarW / 2 + 0.03f && std::fabs(hy - (p.y - p.h / 2 - kBarGap)) < kBarH / 2 + 0.03f) { extraBarHover = (int)i; break; }
        }
        if (extraBarHover < 0) {
            extraHold = 0;
            // Pointing at an extra screen: drive the Mac pointer on its display.
            int prev = extraPtr; extraPtr = -1;
            float ex, ey; int i = extraAt(ray[1].o, ray[1].d, ex, ey);
            if (i < 0) return false;
            const Placement& p = extras[i].p;
            float u = (ex - (p.x - p.w / 2)) / p.w, v = 1 - (ey - (p.y - p.h / 2)) / p.h;
            if (prev != i) { eu = u; ev = v; }
            float speed = std::hypot(u - eu, v - ev) * 72.0f, a = ray[1].locked ? 0 : std::fmin(0.08f + speed * 0.9f, 0.7f);
            eu += (u - eu) * a; ev += (v - ev) * a; extraPtr = i;
            int iu = (int)(eu * 10000), iv = (int)(ev * 10000);
            if (iu != sentU || iv != sentV) { sendMouse('m', extras[i].id, eu, ev); sentU = iu; sentV = iv; extraMoves++; }
            if (p1 && !pinchPrev[1]) { sendMouse('c', extras[i].id, eu, ev); extraClicks++; extraClickAt = std::chrono::steady_clock::now(); }
            return true;
        }
        extraPtr = -1;
        if (p1 && !pinchPrev[1]) extraStart = std::chrono::steady_clock::now();
        extraHold = p1 ? std::chrono::duration<float>(std::chrono::steady_clock::now() - extraStart).count() / kHoldToMove : 0;
        if (extraHold >= 1) { extraMoving = extraBarHover; stickyMove = false; extraHold = 0; event("moving screen " + std::to_string(extras[extraMoving].id)); }
        return true;
    }
    // Demand: what the user can see right now, so the laptop spends bandwidth only there ("virtual texture" style).
    // Five times a second the headset sends one UDP datagram to the laptop (:57721):
    //   {"t":<ms>,"budgetKbps":<n>,"screens":[{"id":1,"vis":1,"px":1400,"pri":1.00},...]}
    // id 1 is the main panel, extras use their screen ids. vis: on screen and not behind a wall. px: how many
    // headset pixels the screen spans horizontally (the most detail worth sending). pri: 0..1 gaze weight
    // (1 within ~12 degrees of where the head points, falling to 0.1 at the edge of view).
    static constexpr float kPxPerRad = 1260.0f;  // Quest 3: about 22 px per degree at the centre of the lens
    int demandSock = -1; std::chrono::steady_clock::time_point demandAt{}; long demandSent = 0; int budgetKbps = 60000;
    struct Demand { int id; bool vis; int px; float pri; };
    Demand demandFor(int id, const Placement& p, Entity* w) {
        Demand d{id, false, 0, 0};
        if (!w || !w->located || !p.active) return d;
        V3 eye = v3(head.position), c = xform(w->pose, {p.x, p.y, 0.02f}), n = rot(w->pose.orientation, {0, 0, 1});
        float dist = std::fmax(len(c - eye), 0.2f);
        float half = std::atan(p.w / 2 / dist);
        float off = std::acos(std::fmin(std::fmax(dot(norm(c - eye), rot(head.orientation, {0, 0, -1})), -1.0f), 1.0f));
        d.vis = seenFrom(eye, c, n, p.wall) && off - half < 0.95f;  // ~55 degrees: inside the headset's view
        d.px = (int)std::fmin(3840.0f, 2 * half * kPxPerRad);
        if (lodPose()) {  // LODPOSE: both axes of the screen's projection from the head pose (room_lab/vt.h projectedPx)
            V3 rx = rot(w->pose.orientation, {1, 0, 0}), uy = rot(w->pose.orientation, {0, 1, 0});
            int sw = vtSrcW(id), sh = (int)std::lround(sw * p.h / std::fmax(0.01f, p.w));
            d.px = vt::projectedPx({eye.x, eye.y, eye.z}, {c.x, c.y, c.z}, {rx.x, rx.y, rx.z}, {uy.x, uy.y, uy.z}, p.w, p.h, sw, sh, kPxPerRad);
        }
        d.pri = !d.vis ? 0.0f : off < 0.21f ? 1.0f : std::fmax(0.1f, 1.0f - (off - 0.21f) / 0.75f);
        return d;
    }
    // Stress test (bench/stress.py): files/stress.json {"n":10,"sweep":3} replaces the extras' demand with N
    // synthetic screens 2..N+1 on a ring around the user, at 1.5/2.5/3.5 m, while a virtual gaze turns one
    // screen every `sweep` seconds. Exercises the sender's stash/resize/bitrate paths with nobody wearing it.
    // "scenario" (optional): ring (default), cinema, vr or wall, the same patterns as bench/stress/headset_sim.py.
    int stressN = 0; float stressSweep = 3; std::string stressScenario = "ring"; std::chrono::steady_clock::time_point stressCheck{};
    void pollStress() {
        auto t = std::chrono::steady_clock::now();
        if (t - stressCheck < std::chrono::seconds(2)) return;
        stressCheck = t;
        std::ifstream f(gDir + "/stress.json"); int n = 0;
        if (f) { std::string j((std::istreambuf_iterator<char>(f)), {}); n = atoi(SharpLayer::field(j, "n").c_str());
                 float sw = atof(SharpLayer::field(j, "sweep").c_str()); if (sw > 0.2f) stressSweep = sw;
                 std::string sc = SharpLayer::field(j, "scenario"); stressScenario = sc.empty() ? "ring" : sc; }
        n = std::max(0, std::min(n, 16));
        if (n != stressN) { stressN = n; event("stress: " + std::to_string(n) + " synthetic screens"); }
        // files/launch ("LAPTOP" or "MINI"): start that stream as if its menu button was pressed (bench/stress.py --game)
        std::ifstream lf(gDir + "/launch");
        if (lf) {
            std::string h; lf >> h; lf.close(); unlink((gDir + "/launch").c_str());
            if (h == "MINI" || h == "LAPTOP") { launcher.launch(hostFor(h == "MINI" ? 0 : 1)); event("launch " + h + " (file)"); }
        }
    }
    Demand stressDemand(int k) {
        if (stressScenario == "cinema") {  // screen 2 fills the view; its two neighbours peek in; the rest are behind
            bool peek = k == 1 || k == stressN - 1;
            return k == 0 ? Demand{2, true, 3000, 1.0f} : Demand{k + 2, peek, peek ? 300 : 0, peek ? 0.1f : 0.0f};
        }
        if (stressScenario == "vr") return Demand{k + 2, false, 0, 0.0f};  // a full VR game: no screen visible
        if (stressScenario == "wall") return Demand{k + 2, true, 500, k == 0 ? 1.0f : 0.3f};  // all visible, small
        const float tau = 6.2831853f;
        float gaze = std::fmod(now() / (stressSweep * stressN), 1.0) * tau, at = tau * k / stressN;
        float off = std::fabs(std::remainder(at - gaze, tau)), dist = 1.5f + (k % 3), half = std::atan(0.7f / dist);
        Demand d{k + 2, off - half < 0.95f, (int)(2 * half * kPxPerRad), 0};
        d.pri = !d.vis ? 0.0f : off < 0.21f ? 1.0f : std::fmax(0.1f, 1.0f - (off - 0.21f) / 0.75f);
        return d;
    }
    void sendDemand() {
        auto t = std::chrono::steady_clock::now();
        if (t - demandAt < std::chrono::milliseconds(200)) return;
        demandAt = t;
        pollStress();
        std::vector<Demand> ds;
        ds.push_back(demandFor(1, placed, placed.active ? wallById(placed.wall) : nullptr));
        if (stressN > 0) for (int k = 0; k < stressN; k++) ds.push_back(stressDemand(k));
        else for (auto& sc : extras) ds.push_back(demandFor(sc.id, sc.p, wallById(sc.p.wall)));
        if (budgetOn()) {
            budget::Input in; in.now = now(); in.frameMs = frameMsForBudget();
            in.gameActive = moonlight.get(moonlight.stage) == "streaming";
            for (auto& d : ds) if (d.id != 1) in.screens.push_back({d.id, d.vis, d.px, d.pri});
            lastBudget = arbiter.split(in);
            budgetKbps = lastBudget.atlasKbps;
        } else budgetKbps = 60000;
        model.kbps = budgetOn() ? lastBudget.assetKbps : 20000;  // 0 while a game streams: assets yield
        bool v2 = vt() || heroOn(), v3 = heroOn();
        std::vector<int> heroes;
        if (v3) {
            std::vector<vt::HeroIn> hin; for (auto& d : ds) if (d.id != 1) hin.push_back({d.id, d.vis, d.px, d.pri});
            int maxHeroes = budgetOn() && lastBudget.mode == budget::Mode::Game ? 1 : 3;  // the decoder budget around a game
            heroes = heroPicker.pick(hin, now(), maxHeroes);
        }
        std::string j = std::string(v3 ? "{\"v\":3,\"heroOk\":1,\"heroCodec\":\"hevc\"," : v2 ? "{\"v\":2," : "{") + "\"t\":" + std::to_string((long)(now() * 1000)) + ",\"budgetKbps\":" + std::to_string(budgetKbps) + ",\"screens\":[";
        for (size_t i = 0; i < ds.size(); i++) {
            char b[160]; int n = snprintf(b, sizeof b, "%s{\"id\":%d,\"vis\":%d,\"px\":%d,\"pri\":%.2f", i ? "," : "", ds[i].id, ds[i].vis ? 1 : 0, ds[i].px, ds[i].pri);
            int bias = budgetOn() && ds[i].pri < 0.5f ? lastBudget.lvlBias : 0;  // unfocused screens shed detail first
            bool hero = std::find(heroes.begin(), heroes.end(), ds[i].id) != heroes.end();
            if (v2 && ds[i].id != 1) snprintf(b + n, sizeof b - n, ",\"lvl\":%d,\"have\":%d%s}", std::min(4, vtWant(ds[i].id, ds[i].px, ds[i].vis) + bias), vtHave(ds[i].id), hero ? ",\"hero\":1" : "");
            else snprintf(b + n, sizeof b - n, "}");
            j += b;
        }
        j += "]";
        if (keyReqOn()) {  // KEYREQ: a counter of keyframe requests (atlas + heroes); the sender keys on any change
            long kr = atlasVideo.keyRequests();
            { std::lock_guard<std::mutex> g(heroLock); for (auto& h : heroStreams) kr += h.second->keyRequests(); }
            j += ",\"keyReq\":" + std::to_string(kr);
        }
        // "game":1 while a game/desktop stream plays: the sender's gameyield flag then halves the atlas encode rate
        // so the shared media engine serves the game first (docs/gaming-latency-laptop.md).
        if (moonlight.get(moonlight.stage) == "streaming") j += ",\"game\":1";
        j += "}";
        if (demandSock < 0) demandSock = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(57721); inet_pton(AF_INET, laptop().c_str(), &a.sin_addr);
        sendto(demandSock, j.data(), j.size(), 0, (sockaddr*)&a, sizeof a);
        demandSent++; lastDemand = j;
    }
    std::string lastDemand;
    void saveScreens() {
        std::ofstream f(gDir + "/screens.json");
        f << "{\"next\":" << nextScreenId << ",\"screens\":[";
        for (size_t i = 0; i < extras.size(); i++) {
            const Placement& p = extras[i].p;
            f << (i ? "," : "") << "{\"id\":" << extras[i].id << ",\"wall\":\"" << p.wall << "\",\"x\":" << p.x << ",\"y\":" << p.y << ",\"w\":" << p.w << ",\"h\":" << p.h << "}";
        }
        f << "]}";
        f.close();
        if (houseOn() && !houseApplying) houseQueueScreens();
    }
    // ---- House file (flag HOUSE; room_lab/house.h, house/README.md). The layout also lives in house.json on the Mini
    // (192.168.0.50:57800, or files/house-host). placement.json and screens.json are still written exactly as before,
    // so turning HOUSE off loses nothing. A worker thread owns the store and does all the network; the render thread
    // only queues edits (houseQueueScreens) and adopts the snapshot the worker publishes (houseUpdate): at start, and
    // whenever the house changes elsewhere (a newer rev, nothing of ours still queued, no move in progress).
    // For now screens point at Meta scene walls: a house wall id maps to its newest scene uuid, and a scene wall the
    // house doesn't know yet is added as "w-<first 8 hex>".
    std::mutex houseLock; std::vector<house::Edit> houseQueue; std::shared_ptr<const json::Value> houseDoc;
    long houseGen = 0, houseSeenGen = 0, houseAppliedRev = -1, houseRev = -1, housePending = 0, houseGets = 0, housePuts = 0, houseConflicts = 0;
    std::string houseErr; bool houseApplying = false; std::atomic<bool> houseStarted{false};
    void houseStart() {
        if (houseStarted.exchange(true)) return;
        std::string host = "192.168.0.50";
        { std::ifstream hf(gDir + "/house-host"); std::string h; if (hf >> h) host = h; }
        event("house: syncing with " + host + ":57800");
        std::thread([this, host] {
            house::Store st(gDir); st.host = host;
            st.loadCache();
            for (;;) {
                { std::lock_guard<std::mutex> g(houseLock); for (auto& e : houseQueue) st.edit(e); houseQueue.clear(); }
                bool ok = st.sync();
                {
                    std::lock_guard<std::mutex> g(houseLock);
                    if (st.doc.type == json::Value::Obj) houseDoc = std::make_shared<const json::Value>(st.doc);
                    houseGen++; houseRev = st.rev; housePending = (long)st.pending.size();
                    houseGets = st.gets; housePuts = st.puts; houseConflicts = st.conflicts; houseErr = ok ? "" : st.lastError;
                }
                for (int i = 0; i < (st.pending.empty() ? 75 : 10); i++) {  // 15 s idle, 2 s while edits wait; an edit wakes it
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    std::lock_guard<std::mutex> g(houseLock); if (!houseQueue.empty()) break;
                }
            }
        }).detach();
    }
    static std::string houseSceneOf(const json::Value& d, const std::string& wid) {
        for (auto& w : d["walls"].a) if (w["id"].str() == wid) return w["scene"][0].str();
        return "";
    }
    void houseUpdate() {
        std::shared_ptr<const json::Value> d; long rev;
        {
            std::lock_guard<std::mutex> g(houseLock);
            if (houseGen == houseSeenGen || !houseDoc || !houseQueue.empty() || housePending) return;
            houseSeenGen = houseGen; d = houseDoc; rev = houseRev;
        }
        if (rev <= houseAppliedRev || moving || extraMoving >= 0 || (*d)["screens"].size() == 0) return;
        houseAppliedRev = rev;
        Placement main; bool haveMain = false; std::vector<Screen> ex;
        for (auto& sc : (*d)["screens"].a) {
            Placement p; p.wall = houseSceneOf(*d, sc["wall"].str()); if (p.wall.empty()) continue;
            p.x = (float)sc["x"].num(); p.y = (float)sc["y"].num(); p.w = (float)sc["w"].num(1.44); p.h = (float)sc["h"].num(0.9);
            p.active = sc["on"].integer(1) != 0; p.valid = true;
            int id = sc["id"].integer();
            if (id == 1) { main = p; haveMain = true; } else if (id > 1 && p.active) ex.push_back({id, p});
        }
        std::set<int> before, after; for (auto& e : extras) before.insert(e.id); for (auto& e : ex) after.insert(e.id);
        houseApplying = true;
        if (haveMain) { placed = main; savePlacement(); }
        extras = ex; nextScreenId = std::max(nextScreenId, (*d)["nextScreenId"].integer(nextScreenId));
        for (auto& e : extras) nextScreenId = std::max(nextScreenId, e.id + 1);
        saveScreens();
        houseApplying = false;
        if (macDisplays && houseAppliedOnce) {  // screens added or removed elsewhere: their Mac displays follow
            for (int id : after) if (!before.count(id)) macDisplay(id, true);
            for (int id : before) if (!after.count(id)) macDisplay(id, false);
        }
        houseAppliedOnce = true;
        event("house: rev " + std::to_string(rev) + " applied, " + std::to_string(extras.size()) + " extra screens");
    }
    bool houseAppliedOnce = false;
    void houseQueueScreens() {
        std::shared_ptr<const json::Value> d; { std::lock_guard<std::mutex> g(houseLock); d = houseDoc; }
        json::Value none; const json::Value& doc = d ? *d : none;
        std::vector<house::Edit> edits; std::set<std::string> newWalls;
        auto wallId = [&](const std::string& uuid) {
            for (auto& w : doc["walls"].a) for (auto& u : w["scene"].a) if (u.str() == uuid) return w["id"].str();
            std::string id = "w-" + uuid.substr(0, 8);
            if (newWalls.insert(id).second) {
                json::Value w = house::obj(); w.o["id"] = house::str(id); w.o["room"] = house::str("r1"); w.o["kind"] = house::str("wall");
                w.o["source"] = house::str("scene"); json::Value sc = house::arr(); sc.a.push_back(house::str(uuid)); w.o["scene"] = sc;
                if (Entity* e = wallById(uuid)) if (e->has2d) { w.o["w"] = house::num(e->r2.extent.width); w.o["h"] = house::num(e->r2.extent.height); }
                edits.push_back({"walls", id, w});
            }
            return id;
        };
        auto screenObj = [&](int id, const Placement& p, const char* role, const json::Value& source) {
            json::Value o = house::obj();
            for (auto& sc : doc["screens"].a) if (sc["id"].integer() == id) o = sc;  // keep fields we don't edit (source, world)
            o.o["id"] = house::num(id); o.o["role"] = house::str(role); o.o["on"] = house::num(1); o.o["wall"] = house::str(wallId(p.wall));
            o.o["x"] = house::num(p.x); o.o["y"] = house::num(p.y); o.o["w"] = house::num(p.w); o.o["h"] = house::num(p.h);
            if (!o.has("source")) o.o["source"] = source;
            edits.push_back({"screens", std::to_string(id), o});
        };
        if (placed.active && !placed.wall.empty()) { json::Value src = house::obj(); src.o["kind"] = house::str("atlas"); screenObj(1, placed, "main", src); }
        std::set<int> have;
        for (auto& e : extras) {
            if (e.p.wall.empty()) continue;
            json::Value src = house::obj(); src.o["kind"] = house::str("mac-display"); src.o["screen"] = house::num(e.id);
            screenObj(e.id, e.p, "extra", src); have.insert(e.id);
        }
        for (auto& sc : doc["screens"].a) { int id = sc["id"].integer(); if (id > 1 && !have.count(id)) edits.push_back({"screens", std::to_string(id), json::Value{}}); }
        std::lock_guard<std::mutex> g(houseLock);
        for (auto& e : edits) houseQueue.push_back(e);
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
            if (extraLive.count(extras[i].id)) {  // its compositor layer shows the Mac display; frame it like the main panel
                const AtlasCell& c = atlasCells[extras[i].id];
                const float cell[4] = {c.x / (float)atlasW, c.y / (float)atlasH, (c.x + c.w) / (float)atlasW, (c.y + c.h) / (float)atlasH};
                auto tl = atlasTiles.find(extras[i].id);
                if (bezel) bezelFrame(b, *w, p, cell, vt() && tl != atlasTiles.end() ? &tl->second : nullptr);
            } else {
            b.quad(P(x0, y0, 0.02f), P(x1, y0, 0.02f), P(x1, y1, 0.02f), P(x0, y1, 0.02f), bg);
            V3 n = rot(w->pose.orientation, {0, 0, 1});
            b.line(P(x0, y0, 0.021f), P(x1, y0, 0.021f), n, 0.006f, edge); b.line(P(x1, y0, 0.021f), P(x1, y1, 0.021f), n, 0.006f, edge);
            b.line(P(x1, y1, 0.021f), P(x0, y1, 0.021f), n, 0.006f, edge); b.line(P(x0, y1, 0.021f), P(x0, y0, 0.021f), n, 0.006f, edge);
            std::string title = "SCREEN " + std::to_string(extras[i].id);
            text(b, *w, p.x, p.y + p.h * 0.08f, 0.022f, UiGrid::fit(title, 0.012f, p.w * 0.6f, p.h * 0.2f), title.c_str(), white);
            text(b, *w, p.x, p.y - p.h * 0.12f, 0.022f, UiGrid::fit("MAC DISPLAY COMING", 0.006f, p.w * 0.7f, p.h * 0.1f), "MAC DISPLAY COMING", dim);
            }
            if (extraPtr == (int)i) {  // cursor: constant angular size, dark outline, gold while pressing
                V3 c = P(p.x - p.w / 2 + eu * p.w, p.y - p.h / 2 + (1 - ev) * p.h, 0.035f);
                V3 ax = rot(w->pose.orientation, {1, 0, 0}), ay = rot(w->pose.orientation, {0, 1, 0}), n = rot(w->pose.orientation, {0, 0, 1});
                float k = std::fmax(len(c - eye), 0.3f) / 1.5f, rr = (ray[1].pinch ? 0.009f : 0.016f) * k;
                const float sh[4] = {0, 0, 0, 0.65f}, wc[4] = {1, 1, 1, 0.95f}, gc[4] = {1.0f, 0.8f, 0.2f, 1};
                for (int pass = 0; pass < 2; pass++) for (int s = 0; s < 20; s++) {
                    float a0 = s * 0.31416f, a1 = (s + 1) * 0.31416f; V3 cz = c + n * (pass * 0.001f);
                    b.line(cz + ax * (rr * std::cos(a0)) + ay * (rr * std::sin(a0)), cz + ax * (rr * std::cos(a1)) + ay * (rr * std::sin(a1)), n,
                           (pass ? 0.004f : 0.009f) * k, pass ? (ray[1].pinch ? gc : wc) : sh);
                }
                if (ray[1].pinch || std::chrono::steady_clock::now() - extraClickAt < std::chrono::milliseconds(250)) {  // click mark
                    float dd = 0.005f * k; V3 cz = c + n * 0.002f;
                    b.quad(cz - ax * dd - ay * dd, cz + ax * dd - ay * dd, cz + ax * dd + ay * dd, cz - ax * dd + ay * dd, gc);
                }
            }
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
        sendDemand();
        pollBattery();
        pollWifiLock();
        updateVoice();
        updateSpell();
        if (updateShoulder() || updateMenu() || updateExtras()) {
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
                if (stickyMove ? (pinch[1] && !pinchPrev[1]) : !pinch[1]) {
                    stickyMove = false;
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
                    if (barHold >= 1) { moving = true; stickyMove = false; barHolding = false; barHold = 0; event("move started"); }
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
    void startAtlas() {
        std::ifstream f(gDir + "/atlas.json"); if (!f) return;
        std::stringstream ss; ss << f.rdbuf(); std::string j = ss.str();
        atlasVideo.directHost = SharpLayer::field(j, "host"); atlasVideo.directPort = atoi(SharpLayer::field(j, "port").c_str());
        // The atlas sender runs on the laptop: "host": "laptop" (or any address, with HOSTRES on) follows the resolver.
        atlasLaptop = hostResMenu || atlasVideo.directHost == "laptop";
        if (atlasLaptop && atlasVideo.directPort > 0) atlasVideo.directHost = laptop();
        if (atlasVideo.directHost.empty() || atlasVideo.directPort <= 0) { atlasVideo.directHost.clear(); return; }
        atlasVideo.source = "atlas";
        atlasVideo.onHero = [this](int s, const uint8_t* p, size_t n) { onHeroPacket(s, p, n); };
        atlasVideo.start();
    }
    // Layout text from the sender: {"atlas":[W,H],"cells":[{"screen":2,"x":0,"y":0,"w":1920,"h":1200},...]}
    void parseAtlas() {
        std::string m; { std::lock_guard<std::mutex> g(atlasVideo.lock); m = atlasVideo.meta; }
        if (m == atlasMeta) return;
        vt::Layout L = vt::parse(m);
        heroLayout.clear();  // v3: "hero":[ids] (screens streamed on their own, no atlas cell)
        size_t hp = m.find("\"hero\":[");
        if (hp != std::string::npos) for (const char* q = m.c_str() + hp + 8; *q && *q != ']'; q++) if (isdigit((unsigned char)*q)) { heroLayout.insert(atoi(q)); while (isdigit((unsigned char)q[1])) q++; }
        atlasMeta = m; atlasW = L.W; atlasH = L.H; atlasTiles = L.screens; atlasCells.clear();
        for (auto& [id, cells] : atlasTiles) atlasCells[id] = cells.front();  // the first tile: bezel colours, v1 paths
        event("atlas layout: " + std::to_string(atlasCells.size()) + " screens");
    }
    // cell: the screen's rectangle in atlas texture space {u0, v0, u1, v1} (v from the top) for extra screens;
    // null for the main panel, whose AMBI colours come from uVideo.
    void bezelFrame(Batch& b, const Entity& wall, const Placement& pl, const float* cell = nullptr, const std::vector<vt::Cell>* tiles = nullptr) {
        const bool tiled = tiles && tiles->size() > 1 && atlasW > 0 && atlasH > 0;  // VT: the screen arrives as several tiles
        float l = pl.x - pl.w / 2, r = pl.x + pl.w / 2, bt = pl.y - pl.h / 2, t = pl.y + pl.h / 2;
        const float z = 0.022f, in = 0.004f;
        auto P = [&](float x, float y) { return xform(wall.pose, {x, y, z}); };
        // A ring between distances d0 and d1 outside the panel edge (negative = inside), as four strips.
        auto ring = [&](float d0, float d1, const float* col) {
            float xs[4][4] = {{l - d1, t + d0, r + d1, t + d1}, {l - d1, bt - d1, r + d1, bt - d0},   // top, bottom
                              {l - d1, bt - d0, l - d0, t + d0}, {r + d0, bt - d0, r + d1, t + d0}};  // left, right
            for (auto& q0 : xs) {
                // Tiled (VT): cut the strip along its length so each piece samples the tile under it; else one piece.
                int segs = tiled && bezelAmbi ? 16 : 1; bool alongX = (q0[2] - q0[0]) >= (q0[3] - q0[1]);
                for (int s = 0; s < segs; s++) {
                float q[4] = {q0[0], q0[1], q0[2], q0[3]};
                if (segs > 1) {
                    if (alongX) { float L0 = q0[0], L1 = q0[2]; q[0] = L0 + (L1 - L0) * s / segs; q[2] = L0 + (L1 - L0) * (s + 1) / segs; }
                    else { float L0 = q0[1], L1 = q0[3]; q[1] = L0 + (L1 - L0) * s / segs; q[3] = L0 + (L1 - L0) * (s + 1) / segs; }
                }
                V3 a = P(q[0], q[1]), bb = P(q[2], q[1]), c = P(q[2], q[3]), d = P(q[0], q[3]);
                if (!bezelAmbi) { b.quad(a, bb, c, d, col); continue; }
                auto uv = [&](float x, float y, float* o) {
                    float u = std::fmin(1, std::fmax(0, (x - l) / pl.w)), v = std::fmin(1, std::fmax(0, (y - bt) / pl.h));
                    float ax, ay;
                    if (tiled && vt::atlasAt(*tiles, u, 1 - v, ax, ay)) { o[0] = ax / atlasW - 20; o[1] = ay / atlasH; }
                    else if (cell) { o[0] = cell[0] + u * (cell[2] - cell[0]) - 20; o[1] = cell[1] + (1 - v) * (cell[3] - cell[1]); }
                    else { o[0] = u - 10; o[1] = v; }
                };
                float ua[2], ub[2], uc[2], ud[2]; uv(q[0], q[1], ua); uv(q[2], q[1], ub); uv(q[2], q[3], uc); uv(q[0], q[3], ud);
                b.tri(a, bb, c, col, ua, ub, uc); b.tri(a, c, d, col, ua, uc, ud);
                }
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
        if (bezel && quadShown && !panelLayer.curve && placed.active && panelSeen) if (auto* w = wallById(placed.wall)) bezelFrame(occ, *w, placed);
        if (placed.active && panelSeen) if (auto* w = wallById(placed.wall)) { const float c[4] = {1, 1, 1, 1}; panelQuad(occ, placed, *w, 0.02f, c, true); buttons(occ); if (!editLocked) { grabBar(occ, *w); corners(occ, *w); } }
        drawMenu(over);
        drawShoulder(over);
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
            text(b, menu, c.x, c.labelY, 0.004f, c.px, menuItems[i].label, white);
            if (!c.hint1.empty()) text(b, menu, c.x, c.hint1Y, 0.004f, c.hintPx, c.hint1.c_str(), dim);
            if (!c.hint2.empty()) text(b, menu, c.x, c.hint2Y, 0.004f, c.hintPx, c.hint2.c_str(), dim);
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
            if (depthGap() || depthDumpWanted) depthReadback(depthTex);
        }
        bool hasVideo = video.update(display) && video.mipTex;
        if (!atlasVideo.directHost.empty()) parseAtlas();  // the layout arrives before the first decoded frame
        bool atlasOk = !atlasVideo.directHost.empty() && atlasVideo.update(display) && atlasVideo.mipTex;
        extraLive.clear();
        if (heroOn() || !heroLayout.empty()) syncHeroStreams();
        for (auto& e : extras) {  // hero screens: their own decoder, straight into their layer
            if (!heroLayout.count(e.id)) continue;
            VideoStream* hv = nullptr; { std::lock_guard<std::mutex> g(heroLock); auto it = heroStreams.find(e.id); if (it != heroStreams.end()) hv = it->second.get(); }
            if (!hv || !hv->update(display) || !hv->mipTex) continue;
            PanelLayer& L = extraLayers[e.id]; PanelLayer& H = holdLayers[e.id];
            if (vt::needsHold(L.hasImage, hv->mipW, hv->mipH, L.contentW, L.contentH)) { H.destroy(); std::swap(H, L); vtHolds++; }  // a rung switch: hold the old image
            L.superSample = panelLayer.superSample; L.super = panelLayer.super;
            L.update(session, hv->mipFbo, hv->mipW, hv->mipH, hv->mipVersion);
            if (L.hasImage && H.chain) H.destroy();
            if (L.hasImage || H.hasImage) extraLive.insert(e.id);
        }
        if (atlasOk) {
            for (auto& e : extras) {
                if (heroLayout.count(e.id)) continue;  // streamed on its own
                auto it = atlasCells.find(e.id); if (it == atlasCells.end()) continue;
                PanelLayer& L = extraLayers[e.id]; L.superSample = panelLayer.superSample; L.super = panelLayer.super;
                if (!vt()) {
                    L.update(session, atlasVideo.mipFbo, it->second.w, it->second.h, atlasVideo.mipVersion, it->second.x, it->second.y);
                    if (L.hasImage) extraLive.insert(e.id);
                    continue;
                }
                PanelLayer& H = holdLayers[e.id];
                bool vis = demandFor(e.id, e.p, wallById(e.p.wall)).vis;
                vt::Resident& R = vtResident[e.id];
                if ((L.hasImage || H.hasImage) && !R.shouldCopy(vis, vt::levelOf(it->second, vtSrcW(e.id)), now())) {  // keep the sharp copy
                    vtSkipped++; extraLive.insert(e.id); continue;
                }
                int cw = 0, ch = 0; std::vector<vt::Tile> tiles = vt::tiles(atlasTiles[e.id], cw, ch);
                if (vt::needsHold(L.hasImage, cw, ch, L.contentW, L.contentH)) {  // new size: show the old image until the new one is in
                    H.destroy(); std::swap(H, L); vtHolds++;
                }
                L.superSample = panelLayer.superSample; L.super = panelLayer.super;
                L.update(session, atlasVideo.mipFbo, cw, ch, atlasVideo.mipVersion, tiles);
                if (L.hasImage) R.copied(vt::levelOf(it->second, vtSrcW(e.id)));
                if (L.hasImage && H.chain) H.destroy();
                if (L.hasImage || H.hasImage) extraLive.insert(e.id);
            }
        }
        if (quadTick++ % 60 == 0) {
            quadFile = access((gDir + "/quad-on").c_str(), F_OK) == 0;
            mipqFile = access((gDir + "/mipq-on").c_str(), F_OK) == 0;  // forces MIPQ for self-tests
            vtFile = access((gDir + "/vt-on").c_str(), F_OK) == 0;      // forces VT for self-tests
            budgetFile = access((gDir + "/budget-on").c_str(), F_OK) == 0;  // forces BUDGET for self-tests
            gpuTimeFile = access((gDir + "/gputime-on").c_str(), F_OK) == 0;  // forces GPUTIME for self-tests
            codecFile = access((gDir + "/codec-h264").c_str(), F_OK) == 0;    // forces CODEC (H.264) for bench pairs
            wifiLlFile = access((gDir + "/wifill-on").c_str(), F_OK) == 0;    // forces WIFILL for bench pairs
            modelsFile = access((gDir + "/models-on").c_str(), F_OK) == 0;    // forces MODELS for self-tests
            lodPoseFile = access((gDir + "/lodpose-on").c_str(), F_OK) == 0;  // forces LODPOSE for self-tests
            heroFile = access((gDir + "/hero-on").c_str(), F_OK) == 0;        // forces HERO for self-tests
            keyReqFile = access((gDir + "/keyreq-on").c_str(), F_OK) == 0;    // forces KEYREQ for self-tests
            shoulderFile = access((gDir + "/shoulder-on").c_str(), F_OK) == 0;  // forces SHOULDER for self-tests
            houseFile = access((gDir + "/house-on").c_str(), F_OK) == 0;        // forces HOUSE for self-tests
            depthGapFile = access((gDir + "/depthgap-on").c_str(), F_OK) == 0;  // forces DEPTHGAP for self-tests
            if (access((gDir + "/depth-dump").c_str(), F_OK) == 0 && !depthDumpWanted) { depthDumpWanted = true; unlink((gDir + "/depth-dump").c_str()); event("depth dump requested"); }
            if (houseOn()) { houseStart(); houseUpdate(); }
            if (models() && !modelStarted) {
                std::ifstream mf(gDir + "/model.json");
                if (mf) { std::stringstream ms; ms << mf.rdbuf(); model.start(ms.str()); modelStarted = true; event("model: loading " + model.url); }
            }
            launcher.h264 = codecMenu || codecFile;
        }
        panelLayer.mipQuality = mipqMenu || mipqFile;
        bool quad = hasVideo && (panelLayer.enabled || quadFile) && placed.active;
        if (quad) panelLayer.update(session, video.mipFbo, video.mipW, video.mipH, video.mipVersion);
        quadShown = quad && panelLayer.hasImage;
        if (models() && modelStarted) model.update();
        std::vector<float> screenCorners;  // depth-only rectangles of every visible screen, so models behind them hide
        if (models() && modelStarted) {
            auto addScreen = [&](const Placement& pl, Entity* w) {
                if (!w || !w->located || !pl.active) return;
                for (auto c : {V3{pl.x - pl.w / 2, pl.y - pl.h / 2, 0.02f}, V3{pl.x + pl.w / 2, pl.y - pl.h / 2, 0.02f}, V3{pl.x + pl.w / 2, pl.y + pl.h / 2, 0.02f}, V3{pl.x - pl.w / 2, pl.y + pl.h / 2, 0.02f}}) {
                    V3 v = xform(w->pose, c); screenCorners.insert(screenCorners.end(), {v.x, v.y, v.z});
                }
            };
            addScreen(placed, placed.active ? wallById(placed.wall) : nullptr);
            for (auto& e : extras) addScreen(e.p, wallById(e.p.wall));
        }
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
            glUniform1i(uAtlas, 2); glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, atlasVideo.mipTex); glActiveTexture(GL_TEXTURE0);
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
            if (models() && modelStarted) {  // last: screens as depth only (they hide the model behind them), then the model.
                // After the UI on purpose: those depth quads sit on the screen planes, where the pointer ring, the grab
                // bars and the wall highlights are drawn; drawn before the UI they hid all of it (2026-11-03 demo).
                model.drawDepthQuads(vp.m, screenCorners); model.draw(vp.m);
                glUseProgram(program); glBindVertexArray(vao);
            }
            glBindVertexArray(0); glBindFramebuffer(GL_FRAMEBUFFER, 0);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(e.chain, &ri);
        }
    }

    void frame() {
        XrFrameState fs{XR_TYPE_FRAME_STATE}; XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(session, &wi, &fs))) return;
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO}; xrBeginFrame(session, &bi);
        auto workStart = std::chrono::steady_clock::now();
        if (gpuTime()) gpuTimer.begin();
        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerPassthroughFB ptl{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        if (ptLayer) { ptl.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; ptl.layerHandle = ptLayer; layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&ptl)); }
        if (fs.shouldRender) {
            frameTime = fs.predictedDisplayTime;
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
            V3 eye = v3(head.position);
            for (auto& e : extras) {  // extra screens: one quad layer each, cut from the atlas
                if (!extraLive.count(e.id)) continue;
                Entity* w = wallById(e.p.wall); if (!w || !w->located) continue;
                V3 c = xform(w->pose, {e.p.x, e.p.y, 0.02f});
                if (!seenFrom(eye, c, rot(w->pose.orientation, {0, 0, 1}), e.p.wall)) continue;
                XrPosef qp; qp.orientation = w->pose.orientation; qp.position = {c.x, c.y, c.z};
                PanelLayer& L = extraLayers[e.id]; PanelLayer* show = L.hasImage ? &L : &holdLayers[e.id];  // the held image while the new one loads
                if (show->hasImage) layers.push_back(show->layer(local, qp, qp, e.p.w, e.p.h));
            }
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj));
            frames++; framesSec++;
        }
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO}; ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; ei.layerCount = (uint32_t)layers.size(); ei.layers = layers.data();
        double workMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - workStart).count();
        frameWorkMs = frameWorkMs ? frameWorkMs * 0.95 + workMs * 0.05 : workMs;
        gpuTimer.end();
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
        j << ",\"video\":" << video.json() << ",\"sharp\":" << video.sharp.json() << ",\"quad\":" << panelLayer.json() << ",\"atlas\":{\"video\":" << atlasVideo.json() << ",\"cells\":" << atlasCells.size() << ",\"live\":" << extraLive.size() << ",\"vt\":" << (vt() ? 1 : 0) << ",\"vtHolds\":" << vtHolds << ",\"vtSkipped\":" << vtSkipped << ",\"hero\":" << (heroOn() ? 1 : 0) << ",\"heroScreens\":" << heroLayout.size() << ",\"heroPackets\":" << heroPackets << "}" << ",\"budget\":{\"on\":" << (budgetOn() ? 1 : 0) << ",\"mode\":\"" << budget::name(lastBudget.mode) << "\",\"atlasKbps\":" << lastBudget.atlasKbps << ",\"gameKbps\":" << lastBudget.gameKbps << ",\"assetKbps\":" << lastBudget.assetKbps << ",\"lvlBias\":" << lastBudget.lvlBias << ",\"workMs\":" << frameWorkMs << ",\"gpuTime\":" << (gpuTime() ? 1 : 0) << ",\"gpuSupported\":" << (gpuTimer.supported ? 1 : 0) << ",\"gpuMs\":" << gpuTimer.ms << ",\"gpuSamples\":" << gpuTimer.samples << ",\"gpuDisjoints\":" << gpuTimer.disjoints << ",\"frameMs\":" << frameMsForBudget() << "}" << ",\"laptop\":{\"ip\":\"" << laptop() << "\",\"via\":\"" << laptopViaNow() << "\",\"hostres\":" << (hostResMenu ? 1 : 0) << "}" << ",\"model\":" << (models() ? model.json() : std::string("null")) << ",\"wifiLock\":{\"on\":" << (wifiLl() ? 1 : 0) << ",\"held\":" << (wifiLockHeld ? 1 : 0) << ",\"error\":\"" << wifiLockErr << "\"}" << ",\"native\":" << (nativeRes ? 1 : 0) << ",\"hz120\":" << (hz120 ? 1 : 0) << ",\"boost\":" << (boost ? 1 : 0) << ",\"refreshHz\":" << currentHz() << ",\"moonlight\":" << moonlight.json() << ",\"launcher\":\"" << jsonEscape(launcher.get()) << "\",\"hoverButton\":" << hoverButton
          << ",\"shoulder\":{\"on\":" << (shoulderOn() ? 1 : 0) << ",\"depth\":" << sm.stack.size() << ",\"presses\":" << smPresses << ",\"top\":\"" << (sm.open() ? sm.stack.back().name : std::string()) << "\"}";
        { std::lock_guard<std::mutex> g(houseLock);
          j << ",\"house\":{\"on\":" << (houseOn() ? 1 : 0) << ",\"rev\":" << houseRev << ",\"applied\":" << houseAppliedRev << ",\"pending\":" << housePending + (long)houseQueue.size()
            << ",\"gets\":" << houseGets << ",\"puts\":" << housePuts << ",\"conflicts\":" << houseConflicts << ",\"error\":\"" << jsonEscape(houseErr) << "\"}"; }
        { std::lock_guard<std::mutex> g(gapLock); j << ",\"depthGap\":{\"on\":" << (depthGap() ? 1 : 0) << ",\"reads\":" << rbReads << ",\"rb\":\"" << rbErr << "\"" << (gapJson.empty() ? "" : "," + gapJson) << "}"; }
        j << ",\"candidate\":"; pj(candidate); j << ",\"placed\":"; pj(placed);
        j << ",\"battery\":{\"pct\":" << batteryPct << ",\"charging\":" << (charging ? 1 : 0) << ",\"toasts\":" << toasts.size() << "}";
        j << ",\"spell\":{\"on\":" << spellOn << ",\"cw\":" << spellsCw << ",\"ccw\":" << spellsCcw << ",\"progress\":" << spellProgress << ",\"extras\":" << extras.size() << ",\"macDisplays\":" << macDisplays << ",\"demandSent\":" << demandSent << ",\"stressN\":" << stressN << ",\"extraClicks\":" << extraClicks << ",\"extraMoves\":" << extraMoves << ",\"displayCalls\":" << displayCalls.load() << ",\"displayFailures\":" << displayFailures.load() << "}";
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
        startHostResolver();
        video.start();  // Mac desktop relay by default; a freshly pushed Sunshine session switches to the Mini
        video.sharp.start(gDir);  // lossless refinement of still tiles, when files/sharp.json names a sender
        startAtlas();             // extra screens' atlas, when files/atlas.json names a sender
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
        hostStop = true; if (hostThread.joinable()) hostThread.join();
        model.shutdown();
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
