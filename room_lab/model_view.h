// A headset-rendered 3D model (MODELS flag; docs/3d-content.md). files/model.json:
//   {"url": "http://192.168.0.50:8099/DamagedHelmet/model.glb", "pos": [0, 1.2, -1.0], "scale": 0.3, "yaw": 0}
// (pos in the app's local space, metres). A background thread downloads the GLB, then the base-colour KTX2 next
// to it, both paced by an assets::Bucket whose rate is the BUDGET arbiter's asset share. The mesh shows untextured
// grey as soon as it is parsed; the texture's mip levels arrive smallest first and each completed level is
// uploaded and becomes the base level, so the model sharpens progressively ("coarse first, hold until fine").
// Drawn in the eye buffers after the screens' depth-only rectangles: a model in front of a screen covers it, one
// behind it is hidden (the screens themselves are compositor layers under the eye buffers).
#pragma once
#include "assets.h"
#include "gltf.h"
#include "ktx2.h"
#include <GLES3/gl3.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ModelView {
    // ---- download (background thread)
    std::string url, error, state = "off";
    float pos[3] = {0, 1.2f, -1.0f}, scale = 0.3f, yaw = 0;
    std::thread net; std::atomic<bool> stop{false}; std::atomic<int> kbps{20000};
    std::mutex lock;                       // guards everything below shared with the render thread
    gltf::Mesh mesh; bool meshReady = false;
    std::vector<uint8_t> tex; uint64_t texRecv = 0; ktx2::Header th; bool thReady = false;  // tex: sized once from Content-Length
    uint64_t bytes = 0, total = 0;
    double startedAt = 0, meshAt = 0, firstLevelAt = 0, fullAt = 0;

    static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    // GET url, delivering the body in pieces; paced by the bucket. Returns "" or an error.
    std::string get(const std::string& u, assets::Bucket& b, const std::function<void(const uint8_t*, size_t, uint64_t)>& onData) {
        if (u.compare(0, 7, "http://")) return "only http:// URLs";
        size_t hs = 7, ps = u.find('/', hs); std::string hostport = u.substr(hs, ps - hs), path = ps == std::string::npos ? "/" : u.substr(ps);
        std::string host = hostport, port = "80"; size_t c = hostport.find(':'); if (c != std::string::npos) { host = hostport.substr(0, c); port = hostport.substr(c + 1); }
        addrinfo hints{}, *ai = nullptr; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &ai) || !ai) return "resolve " + host;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        timeval tv{5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen); freeaddrinfo(ai);
        if (rc < 0) { close(fd); return "connect " + hostport; }
        std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + hostport + "\r\n\r\n";
        send(fd, req.data(), req.size(), 0);
        std::string head; uint8_t buf[16384]; uint64_t length = 0; bool inBody = false;
        while (!stop) {
            uint64_t allow = b.allow(now());
            if (inBody && allow == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            size_t want = inBody ? (size_t)std::min<uint64_t>(sizeof buf, allow) : sizeof buf;
            ssize_t r = recv(fd, buf, want, 0);
            if (r <= 0) break;
            b.take((uint64_t)r);
            if (!inBody) {
                head.append((const char*)buf, (size_t)r);
                size_t e = head.find("\r\n\r\n"); if (e == std::string::npos) continue;
                if (head.compare(9, 3, "200")) { close(fd); return "HTTP " + head.substr(9, 3); }
                size_t cl = head.find("Content-Length:"); if (cl == std::string::npos) cl = head.find("content-length:");
                if (cl != std::string::npos) length = strtoull(head.c_str() + cl + 15, nullptr, 10);
                inBody = true;
                if (head.size() > e + 4) onData((const uint8_t*)head.data() + e + 4, head.size() - e - 4, length);
            } else onData(buf, (size_t)r, length);
        }
        close(fd);
        return stop ? "stopped" : "";
    }

    void start(const std::string& config) {
        json::Value c = json::parse(config);
        url = c["url"].str(); scale = (float)c["scale"].num(0.3); yaw = (float)c["yaw"].num(0);
        for (int i = 0; i < 3; i++) pos[i] = (float)c["pos"][(size_t)i].num(pos[i]);
        if (url.empty()) { state = "no url"; return; }
        stop = false; startedAt = now(); state = "loading model";
        net = std::thread([this] {
            assets::Bucket b;
            auto setErr = [this](const std::string& e) { std::lock_guard<std::mutex> g(lock); error = e; state = "error"; };
            std::vector<uint8_t> glb;
            b.kbps = kbps;
            std::string e = get(url, b, [&](const uint8_t* d, size_t n, uint64_t len) { glb.insert(glb.end(), d, d + n); b.kbps = kbps; std::lock_guard<std::mutex> g(lock); bytes = glb.size(); total = len; });
            if (!e.empty()) { setErr(e); return; }
            gltf::Mesh m = gltf::load(glb.data(), glb.size());
            if (!m.error.empty()) { setErr(m.error); return; }
            std::string texUri = m.baseColorUri;
            { std::lock_guard<std::mutex> g(lock); mesh = std::move(m); meshReady = true; meshAt = now(); state = texUri.empty() ? "ready" : "loading texture"; }
            if (texUri.empty()) return;
            std::string texUrl = url.substr(0, url.rfind('/') + 1) + texUri;
            e = get(texUrl, b, [&](const uint8_t* d, size_t n, uint64_t len) {
                b.kbps = kbps;
                std::lock_guard<std::mutex> g(lock);
                if (tex.empty() && len) tex.resize(len);           // one allocation; the render thread reads up to texRecv
                if (texRecv + n <= tex.size()) memcpy(tex.data() + texRecv, d, n);
                else { tex.resize(texRecv); tex.insert(tex.end(), d, d + n); }  // no Content-Length
                texRecv += n; bytes = glb.size() + texRecv; total = glb.size() + std::max<uint64_t>(len, texRecv);
                if (!thReady) { ktx2::Header h; if (ktx2::parse(tex.data(), texRecv, h)) { th = h; thReady = ktx2::supported(h.vkFormat); if (!thReady) error = "unsupported KTX2 format"; } else if (!h.error.empty()) error = h.error; }
            });
            if (!e.empty()) { setErr(e); return; }
            std::lock_guard<std::mutex> g(lock); state = "ready";
        });
    }
    void shutdown() { stop = true; if (net.joinable()) net.join(); }

    // ---- GL (render thread)
    GLuint prog = 0, vao = 0, vbo[3] = {}, ibo = 0, texId = 0; GLint uMvp = -1, uModel = -1, uTex = -1, uHasTex = -1;
    GLsizei indexCount = 0; int uploadedFinest = -1, texLevels = 0;
    GLuint quadVao = 0, quadVbo = 0;

    // GL internal format for a KTX2 vkFormat (ASTC enums from KHR_texture_compression_astc_ldr, core in GLES 3.2).
    static GLenum glFormat(uint32_t vk) {
        switch (vk) {
            case ktx2::VK_R8G8B8A8_UNORM: return GL_RGBA8;
            case ktx2::VK_ETC2_R8G8B8_SRGB: return GL_COMPRESSED_SRGB8_ETC2;
            case ktx2::VK_ETC2_R8G8B8A8_SRGB: return GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC;
            case ktx2::VK_ASTC_4x4_SRGB: return 0x93D0;  // GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR
            case ktx2::VK_ASTC_6x6_SRGB: return 0x93D4;  // ..._6x6_KHR
            case ktx2::VK_ASTC_8x8_SRGB: return 0x93D7;  // ..._8x8_KHR
            default: return GL_SRGB8_ALPHA8;
        }
    }
    GLuint compile(GLenum t, const char* s) { GLuint sh = glCreateShader(t); glShaderSource(sh, 1, &s, nullptr); glCompileShader(sh); return sh; }
    void initGL() {
        const char* vs = R"(#version 300 es
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNrm; layout(location=2) in vec2 aUv;
uniform mat4 uMvp; uniform mat4 uModel; out vec3 vN; out vec2 vUv;
void main(){ gl_Position = uMvp * vec4(aPos, 1.0); vN = mat3(uModel) * aNrm; vUv = aUv; })";
        const char* fs = R"(#version 300 es
precision mediump float; in vec3 vN; in vec2 vUv; uniform sampler2D uTex; uniform int uHasTex; out vec4 o;
void main(){ vec3 base = uHasTex == 1 ? texture(uTex, vUv).rgb : vec3(0.55);
  float l = 0.35 + 0.65 * max(dot(normalize(vN), normalize(vec3(0.3, 0.8, 0.5))), 0.0);
  o = vec4(base * l, 1.0); })";
        prog = glCreateProgram(); glAttachShader(prog, compile(GL_VERTEX_SHADER, vs)); glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs)); glLinkProgram(prog);
        uMvp = glGetUniformLocation(prog, "uMvp"); uModel = glGetUniformLocation(prog, "uModel"); uTex = glGetUniformLocation(prog, "uTex"); uHasTex = glGetUniformLocation(prog, "uHasTex");
        glGenVertexArrays(1, &quadVao); glGenBuffers(1, &quadVbo);
    }
    // Uploads whatever arrived since the last frame: the mesh once, then each newly completed mip level.
    void update() {
        std::lock_guard<std::mutex> g(lock);
        if (meshReady && !vao) {
            if (!prog) initGL();
            glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(3, vbo); glGenBuffers(1, &ibo);
            auto attr = [&](int i, const std::vector<float>& v, int n) {
                glBindBuffer(GL_ARRAY_BUFFER, vbo[i]);
                if (v.empty()) { glDisableVertexAttribArray(i); glVertexAttrib4f(i, 0, 1, 0, 0); return; }
                glBufferData(GL_ARRAY_BUFFER, v.size() * 4, v.data(), GL_STATIC_DRAW);
                glEnableVertexAttribArray(i); glVertexAttribPointer(i, n, GL_FLOAT, GL_FALSE, 0, nullptr);
            };
            attr(0, mesh.pos, 3); attr(1, mesh.nrm, 3); attr(2, mesh.uv, 2);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.idx.size() * 4, mesh.idx.data(), GL_STATIC_DRAW);
            indexCount = (GLsizei)mesh.idx.size();
            glBindVertexArray(0);
        }
        if (thReady && !texId) {
            glGenTextures(1, &texId); glBindTexture(GL_TEXTURE_2D, texId);
            texLevels = (int)th.levels;
            GLenum fmt = glFormat(th.vkFormat);
            glTexStorage2D(GL_TEXTURE_2D, texLevels, fmt, th.width, th.height);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, texLevels - 1); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, texLevels - 1);
            uploadedFinest = texLevels;
        }
        if (texId) {
            int fin = ktx2::finestComplete(th, texRecv);
            if (fin >= 0 && fin < uploadedFinest) {
                glBindTexture(GL_TEXTURE_2D, texId);
                GLenum cfmt = glFormat(th.vkFormat); bool compressed = cfmt != GL_RGBA8 && cfmt != GL_SRGB8_ALPHA8;
                for (int i = uploadedFinest - 1; i >= fin; i--) {
                    const uint8_t* d = tex.data() + th.level[i].offset;
                    if (compressed) glCompressedTexSubImage2D(GL_TEXTURE_2D, i, 0, 0, ktx2::levelWidth(th, i), ktx2::levelHeight(th, i), cfmt, (GLsizei)th.level[i].length, d);
                    else glTexSubImage2D(GL_TEXTURE_2D, i, 0, 0, ktx2::levelWidth(th, i), ktx2::levelHeight(th, i), GL_RGBA, GL_UNSIGNED_BYTE, d);
                }
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, fin);  // sample only what is resident
                if (uploadedFinest == texLevels) firstLevelAt = now();
                if (fin == 0) fullAt = now();
                uploadedFinest = fin;
            }
        }
    }
    void modelMatrix(float m[16]) const {
        float c = std::cos(yaw * 0.0174533f), s = std::sin(yaw * 0.0174533f);
        float r[16] = {c * scale, 0, -s * scale, 0, 0, scale, 0, 0, s * scale, 0, c * scale, 0, pos[0], pos[1], pos[2], 1};
        memcpy(m, r, sizeof r);
    }
    static void mul4(const float* a, const float* b, float* o) { for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) { float s = 0; for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k]; o[c * 4 + r] = s; } }
    // Screens as depth only (corners in local space, 4 per quad), so models behind them are hidden.
    void drawDepthQuads(const float* vp, const std::vector<float>& corners) {
        if (!prog || corners.empty()) return;
        std::vector<float> tri;
        for (size_t q = 0; q + 12 <= corners.size(); q += 12) for (int k : {0, 1, 2, 0, 2, 3}) tri.insert(tri.end(), &corners[q + k * 3], &corners[q + k * 3] + 3);
        glUseProgram(prog); glUniformMatrix4fv(uMvp, 1, GL_FALSE, vp);
        float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; glUniformMatrix4fv(uModel, 1, GL_FALSE, id);
        glBindVertexArray(quadVao); glBindBuffer(GL_ARRAY_BUFFER, quadVbo); glBufferData(GL_ARRAY_BUFFER, tri.size() * 4, tri.data(), GL_STREAM_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr); glDisableVertexAttribArray(1); glDisableVertexAttribArray(2);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)tri.size() / 3); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindVertexArray(0);
    }
    void draw(const float* vp) {
        if (!vao || !indexCount) return;
        float m[16], mvp[16]; modelMatrix(m); mul4(vp, m, mvp);
        glUseProgram(prog); glUniformMatrix4fv(uMvp, 1, GL_FALSE, mvp); glUniformMatrix4fv(uModel, 1, GL_FALSE, m);
        bool tex = texId && uploadedFinest < texLevels;
        glUniform1i(uHasTex, tex ? 1 : 0); glUniform1i(uTex, 3); glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, tex ? texId : 0); glActiveTexture(GL_TEXTURE0);
        glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
        glBindVertexArray(vao); glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr); glBindVertexArray(0);
        glEnable(GL_BLEND);
    }
    std::string json() {
        std::lock_guard<std::mutex> g(lock);
        char b[512];
        snprintf(b, sizeof b, "{\"state\":\"%s\",\"error\":\"%s\",\"bytes\":%llu,\"total\":%llu,\"kbps\":%d,\"triangles\":%d,\"texLevel\":%d,\"texLevels\":%d,\"meshMs\":%.0f,\"firstLevelMs\":%.0f,\"fullMs\":%.0f}",
                 state.c_str(), error.c_str(), (unsigned long long)bytes, (unsigned long long)total, kbps.load(), indexCount / 3,
                 texId ? (uploadedFinest < texLevels ? uploadedFinest : -1) : -1, texLevels,
                 meshAt ? (meshAt - startedAt) * 1000 : -1.0, firstLevelAt ? (firstLevelAt - startedAt) * 1000 : -1.0, fullAt ? (fullAt - startedAt) * 1000 : -1.0);
        return b;
    }
};
