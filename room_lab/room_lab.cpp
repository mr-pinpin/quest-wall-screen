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
    MoonlightSource moonlight;

    PFN_xrCreatePassthroughFB xrCreatePassthroughFB_ = nullptr;
    PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB_ = nullptr;
    XrPassthroughFB passthrough = XR_NULL_HANDLE; XrPassthroughLayerFB ptLayer = XR_NULL_HANDLE;

    PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT_ = nullptr;
    PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT_ = nullptr;
    XrHandTrackerEXT hands[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrHandJointLocationEXT joints[2][XR_HAND_JOINT_COUNT_EXT];
    XrHandTrackingAimStateFB aim[2];
    bool handActive[2] = {false, false}, pinchPrev[2] = {false, false};
    int handResult[2] = {0, 0};
    // Ray and pinch derived from joints: shoulder estimate -> pinch point, thumb/index tip distance.
    struct HandRay { bool valid = false, pinch = false; V3 o{0, 0, 0}, d{0, 0, 0}; float gap = 1; };
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
            XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME};
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
  if(vUv.x>=0.0){
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
            hr.pinch = hr.pinch ? hr.gap < 0.035f : hr.gap < 0.018f;
            V3 grip = (v3(kn.pose.position) + v3(tm.pose.position)) * 0.5f;
            V3 shoulder = xform(head, {h == 1 ? 0.17f : -0.17f, -0.2f, 0.05f});
            hr.o = grip; hr.d = norm(grip - shoulder); hr.valid = len(grip - shoulder) > 0.15f;
        }
    }

    void updatePlacement() {
        candidate.active = false;
        float aspect = 0.625f;
        { std::lock_guard<std::mutex> g(video.lock); if (video.width > 0 && video.height > 0) aspect = (float)video.height / (float)video.width; }
        const float sizes[3][2] = {{1.0f, 1.0f * aspect}, {1.44f, 1.44f * aspect}, {2.0f, 2.0f * aspect}};
        if (placed.active && std::fabs(placed.h - placed.w * aspect) > 0.005f) { placed.h = placed.w * aspect; if (auto* w = wallById(placed.wall)) validate(placed, *w); }
        bool pinch[2];
        for (int h = 0; h < 2; h++) pinch[h] = handActive[h] && ray[h].pinch;
        if (pinch[0] && !pinchPrev[0]) {
            sizeIndex = (sizeIndex + 1) % 3;
            if (placed.active) { placed.w = sizes[sizeIndex][0]; placed.h = sizes[sizeIndex][1]; if (auto* w = wallById(placed.wall)) validate(placed, *w); savePlacement(); }
            event("panel size " + std::to_string(sizes[sizeIndex][0]));
        }
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
            if (hit) { hitFrames++;
                candidate.active = true; candidate.wall = hit->uuid; candidate.x = hx; candidate.y = hy;
                candidate.w = sizes[sizeIndex][0]; candidate.h = sizes[sizeIndex][1];
                validate(candidate, *hit);
                if (pinch[1] && !pinchPrev[1]) pinchEdges++;
                if (pinch[1] && !pinchPrev[1] && candidate.valid) { placed = candidate; savePlacement(); event("placed panel on wall " + placed.wall); }
            }
        }
        for (int k = 0; k < 2; k++) pinchPrev[k] = pinch[k];
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

    void build(Batch& occ, Batch& over) {
        const float wallCol[4] = {0.3f, 0.85f, 1.0f, 0.55f}, planeCol[4] = {1, 1, 1, 0.35f}, volCol[4] = {1.0f, 0.6f, 0.15f, 0.6f};
        for (auto& e : entities) {
            if (!e.located) continue;
            if (e.has2d && !e.has3d) {
                auto c = corners2d(e); V3 n = rot(e.pose.orientation, {0, 0, 1}); V3 lift = n * 0.01f;
                const float* col = isWall(e) ? wallCol : planeCol;
                for (int i = 0; i < 4; i++) occ.dashed(c[i] + lift, c[(i + 1) % 4] + lift, n, 0.008f, col);
            }
            if (e.has3d) {
                auto c = corners3d(e);
                int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (auto& ed : edges) occ.ray(c[ed[0]], c[ed[1]], 0.008f, volCol);
            }
        }
        if (placed.active) if (auto* w = wallById(placed.wall)) { const float c[4] = {1, 1, 1, 1}; panelQuad(occ, placed, *w, 0.02f, c, true); }
        if (candidate.active) if (auto* w = wallById(candidate.wall)) {
            const float good[4] = {0.2f, 1.0f, 0.4f, 0.9f}, bad[4] = {1.0f, 0.25f, 0.2f, 0.9f};
            panelQuad(occ, candidate, *w, 0.03f, candidate.valid ? good : bad, false);
        }
        const float handCol[4] = {0.2f, 0.9f, 1.0f, 0.9f}, pinchCol[4] = {1.0f, 0.9f, 0.2f, 1.0f}, rayCol[4] = {1, 1, 1, 0.5f};
        for (int h = 0; h < 2; h++) {
            if (!handActive[h]) continue;
            bool pinching = ray[h].pinch;
            for (auto& j : joints[h]) if (j.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) over.cube(v3(j.pose.position), 0.008f, pinching ? pinchCol : handCol);
            if (h == 1 && ray[h].valid) {
                V3 o = ray[h].o, d = ray[h].d;
                float L = lastNearest > 0 ? std::fmin(lastNearest, 6.0f) : 3.0f;
                const float hitCol[4] = {0.3f, 1.0f, 0.5f, 0.8f};
                over.ray(o + d * 0.05f, o + d * L, 0.004f, candidate.active ? hitCol : rayCol);
            }
        }
        // Head-locked status dots: passthrough, room model, depth, hands.
        bool walls = false; for (auto& e : entities) walls |= isWall(e) && e.located;
        bool st[4] = {ptLayer != XR_NULL_HANDLE, walls, depthValid, handActive[0] || handActive[1]};
        bool waiting[4] = {false, sceneState.rfind("querying", 0) == 0 || sceneState == "capturing", depthStarted && !depthValid, false};
        for (int i = 0; i < 4; i++) {
            const float g[4] = {0.2f, 1, 0.3f, 0.9f}, r[4] = {1, 0.2f, 0.2f, 0.9f}, a[4] = {1, 0.75f, 0.1f, 0.9f};
            over.cube(xform(head, {-0.045f + i * 0.03f, -0.22f, -0.8f}), 0.015f, st[i] ? g : waiting[i] ? a : r);
        }
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
            glUniform1i(uDepth, 0); glUniform1i(uVideo, 1); glUniform1i(uHasVideo, hasVideo ? 1 : 0);
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
                                      << ",\"pinch\":" << aim[h].pinchStrengthIndex << ",\"ray\":" << ray[h].valid << ",\"gap\":" << ray[h].gap << ",\"result\":" << handResult[h] << ",\"tracker\":" << (hands[h] != XR_NULL_HANDLE) << "}";
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
        j << ",\"video\":" << video.json() << ",\"moonlight\":" << moonlight.json();
        j << ",\"candidate\":"; pj(candidate); j << ",\"placed\":"; pj(placed);
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
        { std::ofstream(gDir + "/events.log", std::ios::trunc); }
        if (!createInstance(env) || !createEgl()) { status(); return; }
        createGl();
        video.start();  // Mac desktop relay by default; a freshly pushed Sunshine session switches to the Mini
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
        moonlight.stop();
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
