// Low-latency desktop video for the wall panel.
// Network thread: WebSocket from the mini's viewer relay (through `adb reverse`) -> hardware H.264 decoder
// rendering into an AImageReader. Render thread: latest image -> EGLImage -> GL_TEXTURE_EXTERNAL_OES.
// Packet layout from MacScreenSender: [flags u8][pts us u64][width u16][height u16][avc profile/compat/level][Annex B].
#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <android/hardware_buffer.h>
#include <arpa/inet.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct VideoStream {
    int port = 8087;
    std::atomic<bool> stop{false};
    std::thread net;

    std::mutex lock;  // guards the fields below shared with the render thread / status
    std::string state = "idle", message;
    int width = 0, height = 0;
    long packets = 0, keyframes = 0, decoded = 0, displayed = 0, dropped = 0, bytes = 0, reconnects = 0;
    double latencyMs = 0, latencyMaxMs = 0;  // packet arrival -> image on the GPU
    std::deque<std::pair<int64_t, double>> arrivals;  // pts us -> arrival seconds

    AMediaCodec* codec = nullptr;
    AImageReader* reader = nullptr;
    int codecW = 0, codecH = 0;
    bool needKey = true;

    // render-thread state
    AImage* current = nullptr;
    EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    bool hasFrame = false;
    // Mipmapped sRGB copy of the latest frame: proper minification instead of one bilinear tap.
    GLuint mipTex = 0, mipFbo = 0, blitProgram = 0, emptyVao = 0; int mipW = 0, mipH = 0; float anisotropy = 1;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getClientBuffer = nullptr;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC targetTexture = nullptr;

    static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void setState(const std::string& s) { std::lock_guard<std::mutex> g(lock); state = s; }

    void start() {
        getClientBuffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
        createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        destroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        targetTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        createBlit();
        net = std::thread([this] { run(); });
    }
    void shutdown() {
        stop = true;
        if (net.joinable()) net.join();
        releaseCodec();
    }

    // ------------------------------------------------------------ network thread
    static bool readExact(int fd, uint8_t* p, size_t n) {
        while (n) { ssize_t r = recv(fd, p, n, 0); if (r <= 0) return false; p += r; n -= (size_t)r; }
        return true;
    }
    int connectRelay() {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        timeval tv{2, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (sockaddr*)&a, sizeof a) < 0) { close(fd); return -1; }
        std::string req = "GET /stream HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nOrigin: http://127.0.0.1:" + std::to_string(port) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        send(fd, req.data(), req.size(), 0);
        std::string head; char c;
        while (head.size() < 4096 && head.find("\r\n\r\n") == std::string::npos) { if (recv(fd, &c, 1, 0) != 1) { close(fd); return -1; } head += c; }
        if (head.find(" 101 ") == std::string::npos) { close(fd); return -1; }
        timeval none{0, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
        timeval idle{5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof idle);
        return fd;
    }
    void run() {
        while (!stop) {
            setState("connecting");
            int fd = connectRelay();
            if (fd < 0) { setState("relay unreachable"); sleepCheck(1000); continue; }
            setState("connected");
            needKey = true;
            std::vector<uint8_t> buf;
            while (!stop) {
                uint8_t h[2];
                if (!readExact(fd, h, 2)) break;
                uint64_t n = h[1] & 0x7f;
                if (n == 126) { uint8_t e[2]; if (!readExact(fd, e, 2)) break; n = (e[0] << 8) | e[1]; }
                else if (n == 127) { uint8_t e[8]; if (!readExact(fd, e, 8)) break; n = 0; for (int i = 0; i < 8; i++) n = (n << 8) | e[i]; }
                if (n > (16u << 20)) break;
                buf.resize(n);
                if (n && !readExact(fd, buf.data(), n)) break;
                int op = h[0] & 0x0f;
                if (op == 8) break;
                if (op == 2 && n) packet(buf.data(), n);
                drain();
            }
            close(fd);
            { std::lock_guard<std::mutex> g(lock); reconnects++; }
            setState("reconnecting");
            sleepCheck(500);
        }
    }
    void sleepCheck(int ms) { for (int i = 0; i < ms / 50 && !stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

    void packet(const uint8_t* p, size_t n) {
        if (p[0] == 0x80) { std::lock_guard<std::mutex> g(lock); message.assign((const char*)p + 1, n - 1); return; }
        if (n < 20) return;
        bool key = p[0] & 1;
        int64_t pts = 0; for (int i = 0; i < 8; i++) pts = (pts << 8) | p[1 + i];
        int w = (p[9] << 8) | p[10], h = (p[11] << 8) | p[12];
        const uint8_t* data = p + 16; size_t len = n - 16;
        double t = now();
        {
            std::lock_guard<std::mutex> g(lock);
            packets++; bytes += (long)n; if (key) keyframes++; width = w; height = h; message.clear();
            arrivals.emplace_back(pts, t); while (arrivals.size() > 120) arrivals.pop_front();
        }
        if (key && (!codec || w != codecW || h != codecH)) configure(w, h, data, len);
        if (!codec) return;
        if (needKey && !key) { std::lock_guard<std::mutex> g(lock); dropped++; return; }
        ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, 20000);
        if (idx < 0) { needKey = true; std::lock_guard<std::mutex> g(lock); dropped++; return; }  // decoder backed up: resync on next key
        size_t cap = 0; uint8_t* in = AMediaCodec_getInputBuffer(codec, (size_t)idx, &cap);
        if (!in || cap < len) { AMediaCodec_queueInputBuffer(codec, (size_t)idx, 0, 0, pts, 0); return; }
        memcpy(in, data, len);
        AMediaCodec_queueInputBuffer(codec, (size_t)idx, 0, len, (uint64_t)pts, 0);
        if (key) needKey = false;
    }
    void drain() {
        if (!codec) return;
        AMediaCodecBufferInfo info;
        for (;;) {
            ssize_t o = AMediaCodec_dequeueOutputBuffer(codec, &info, 0);
            if (o >= 0) { AMediaCodec_releaseOutputBuffer(codec, (size_t)o, info.size > 0); std::lock_guard<std::mutex> g(lock); decoded++; }
            else if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || o == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
            else break;
        }
    }
    static void nal(const uint8_t* d, size_t n, int type, std::vector<uint8_t>& out) {
        for (size_t i = 0; i + 4 < n; i++) {
            if (d[i] || d[i + 1] || d[i + 2] != 0 || d[i + 3] != 1 || (d[i + 4] & 0x1f) != type) continue;
            size_t j = i + 4; while (j + 3 < n && !(d[j] == 0 && d[j + 1] == 0 && d[j + 2] == 0 && d[j + 3] == 1)) j++;
            if (j + 3 >= n) j = n;
            out.assign(d + i, d + j); return;
        }
    }
    void releaseCodec() {
        if (codec) { AMediaCodec_stop(codec); AMediaCodec_delete(codec); codec = nullptr; }
        // reader is released by the render thread once its last image is gone
    }
    void configure(int w, int h, const uint8_t* data, size_t len) {
        releaseCodec();
        std::vector<uint8_t> sps, pps; nal(data, len, 7, sps); nal(data, len, 8, pps);
        AImageReader* r = nullptr;
        if (AImageReader_newWithUsage(w, h, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4, &r) != AMEDIA_OK) { setState("image reader failed"); return; }
        ANativeWindow* window = nullptr; AImageReader_getWindow(r, &window);
        AMediaCodec* c = AMediaCodec_createDecoderByType("video/avc");
        AMediaFormat* f = AMediaFormat_new();
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, w); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, h);
        AMediaFormat_setInt32(f, "low-latency", 1);
        AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
        AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", 1);  // decode-order output, as Moonlight does on Qualcomm
        AMediaFormat_setInt32(f, "priority", 0);
        if (!sps.empty()) AMediaFormat_setBuffer(f, "csd-0", sps.data(), sps.size());
        if (!pps.empty()) AMediaFormat_setBuffer(f, "csd-1", pps.data(), pps.size());
        media_status_t s = c ? AMediaCodec_configure(c, f, window, nullptr, 0) : AMEDIA_ERROR_UNKNOWN;
        AMediaFormat_delete(f);
        if (s != AMEDIA_OK || AMediaCodec_start(c) != AMEDIA_OK) { if (c) AMediaCodec_delete(c); AImageReader_delete(r); setState("decoder failed"); return; }
        std::lock_guard<std::mutex> g(lock);
        pendingReaders.push_back(r);
        codec = c; codecW = w; codecH = h; needKey = true; state = "decoding";
    }
    std::vector<AImageReader*> pendingReaders;  // newest last; render thread switches to it

    // ------------------------------------------------------------ render thread
    static GLuint shader(GLenum type, const char* src) {
        GLuint s = glCreateShader(type); glShaderSource(s, 1, &src, nullptr); glCompileShader(s); return s;
    }
    void createBlit() {
        const char* vs = R"(#version 300 es
out vec2 vUv;
void main(){ vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2)); vUv=p; gl_Position=vec4(p*2.0-1.0,0.0,1.0); })";
        const char* fs = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES uFrame; in vec2 vUv; out vec4 o;
void main(){ o=vec4(pow(texture(uFrame,vUv).rgb,vec3(2.2)),1.0); })";
        blitProgram = glCreateProgram();
        glAttachShader(blitProgram, shader(GL_VERTEX_SHADER, vs)); glAttachShader(blitProgram, shader(GL_FRAGMENT_SHADER, fs));
        glLinkProgram(blitProgram);
        glGenVertexArrays(1, &emptyVao); glGenFramebuffers(1, &mipFbo);
        glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &anisotropy);
        if (anisotropy < 1) anisotropy = 1;
    }
    void resolveMips() {
        int w = codecW, h = codecH;
        if (w <= 0 || h <= 0) return;
        if (!mipTex || w != mipW || h != mipH) {
            if (mipTex) glDeleteTextures(1, &mipTex);
            glGenTextures(1, &mipTex); glBindTexture(GL_TEXTURE_2D, mipTex);
            int levels = 1; while ((w >> levels) > 0 || (h >> levels) > 0) levels++;
            glTexStorage2D(GL_TEXTURE_2D, levels, GL_SRGB8_ALPHA8, w, h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */, anisotropy);
            mipW = w; mipH = h;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, mipFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mipTex, 0);
        glViewport(0, 0, w, h); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
        glUseProgram(blitProgram); glBindVertexArray(emptyVao);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
        glUniform1i(glGetUniformLocation(blitProgram, "uFrame"), 1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0); glBindFramebuffer(GL_FRAMEBUFFER, 0); glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mipTex); glGenerateMipmap(GL_TEXTURE_2D);
    }
    // Bind the newest decoded frame to `texture`; returns true when a frame is available.
    bool update(EGLDisplay display) {
        AImageReader* r = nullptr;
        {
            std::lock_guard<std::mutex> g(lock);
            if (!pendingReaders.empty()) {
                r = pendingReaders.back();
                for (size_t i = 0; i + 1 < pendingReaders.size(); i++) AImageReader_delete(pendingReaders[i]);
                pendingReaders.clear();
            }
        }
        if (r) { dropImage(display); if (reader) AImageReader_delete(reader); reader = r; }
        if (!reader || !getClientBuffer || !createImage || !targetTexture) return hasFrame;
        AImage* img = nullptr;
        if (AImageReader_acquireLatestImage(reader, &img) != AMEDIA_OK || !img) return hasFrame;
        AHardwareBuffer* hb = nullptr; AImage_getHardwareBuffer(img, &hb);
        if (!hb) { AImage_delete(img); return hasFrame; }
        EGLClientBuffer cb = getClientBuffer(hb);
        const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        EGLImageKHR ei = createImage(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
        if (ei == EGL_NO_IMAGE_KHR) { AImage_delete(img); return hasFrame; }
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
        targetTexture(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)ei);
        dropImage(display);
        current = img; eglImage = ei; hasFrame = true;
        resolveMips();
        int64_t ns = 0; AImage_getTimestamp(img, &ns);
        int64_t pts = ns / 1000; double t = now();
        std::lock_guard<std::mutex> g(lock);
        displayed++;
        for (auto& a : arrivals) if (a.first == pts) {
            double ms = (t - a.second) * 1000.0;
            latencyMs = latencyMs ? latencyMs * 0.9 + ms * 0.1 : ms; latencyMaxMs = std::max(latencyMaxMs * 0.995, ms);
            break;
        }
        return true;
    }
    void dropImage(EGLDisplay display) {
        if (eglImage != EGL_NO_IMAGE_KHR && destroyImage) destroyImage(display, eglImage);
        eglImage = EGL_NO_IMAGE_KHR;
        if (current) AImage_delete(current);
        current = nullptr;
    }
    std::string json() {
        std::lock_guard<std::mutex> g(lock);
        char b[512];
        snprintf(b, sizeof b, "{\"state\":\"%s\",\"size\":[%d,%d],\"packets\":%ld,\"keyframes\":%ld,\"decoded\":%ld,\"displayed\":%ld,\"dropped\":%ld,\"bytes\":%ld,\"reconnects\":%ld,\"latencyMs\":%.1f,\"latencyMaxMs\":%.1f,\"message\":\"",
                 state.c_str(), width, height, packets, keyframes, decoded, displayed, dropped, bytes, reconnects, latencyMs, latencyMaxMs);
        std::string s = b;
        for (char c : message) if (c != '"' && c != '\\' && (unsigned char)c >= 0x20) s += c;
        return s + "\"}";
    }
};
