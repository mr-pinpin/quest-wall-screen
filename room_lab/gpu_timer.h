// GPU time of the headset's frame (GL_EXT_disjoint_timer_query), for the budget arbiter (room_lab/budget.h).
// CPU time between xrBeginFrame and xrEndFrame only measures command submission; the GPU work (atlas copies, mip
// builds, eye buffers) finishes later. A ring of 4 queries is read without stalling: a result is collected only
// once available (normally 1-2 frames later), and results spanning a GPU disjoint event (frequency change, power
// collapse) are discarded. Unsupported (no extension or entry point): supported stays false and callers fall
// back to the CPU proxy.
#pragma once
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <cstring>

struct GpuTimer {
    static constexpr int kRing = 4;
    bool tried = false, supported = false, open = false;
    GLuint q[kRing] = {}; int head = 0, inFlight = 0;
    double ms = 0, lastMs = 0; long samples = 0, disjoints = 0;
    PFNGLGETQUERYOBJECTUI64VEXTPROC getU64 = nullptr;

    void init() {
        tried = true;
        const char* e = (const char*)glGetString(GL_EXTENSIONS);
        if (!e || !strstr(e, "GL_EXT_disjoint_timer_query")) return;
        getU64 = (PFNGLGETQUERYOBJECTUI64VEXTPROC)eglGetProcAddress("glGetQueryObjectui64vEXT");
        if (!getU64) return;
        glGenQueries(kRing, q);
        glGetError();  // clear GL_GPU_DISJOINT_EXT's initial state
        GLint d = 0; glGetIntegerv(GL_GPU_DISJOINT_EXT, &d);
        supported = true;
    }
    // Call around the frame's GL work, on the GL thread. One TIME_ELAPSED query can be open at a time.
    void begin() {
        if (!tried) init();
        if (!supported || open) return;
        collect();
        if (inFlight == kRing) return;  // GPU more than 4 frames behind: skip this frame's sample
        glBeginQuery(GL_TIME_ELAPSED_EXT, q[(head + inFlight) % kRing]); open = true;
    }
    void end() {
        if (!open) return;
        glEndQuery(GL_TIME_ELAPSED_EXT); open = false; inFlight++;
    }
    void collect() {
        GLint disjoint = 0; glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        while (inFlight > 0) {
            GLuint avail = 0; glGetQueryObjectuiv(q[head], GL_QUERY_RESULT_AVAILABLE, &avail);
            if (!avail) break;
            GLuint64 ns = 0; getU64(q[head], GL_QUERY_RESULT, &ns);
            if (disjoint) disjoints++;
            else { lastMs = ns / 1e6; ms = samples ? ms * 0.95 + lastMs * 0.05 : lastMs; samples++; }
            head = (head + 1) % kRing; inFlight--;
        }
    }
    bool valid() const { return supported && samples > 0; }
};
