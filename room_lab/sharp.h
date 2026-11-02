// Build-to-lossless refinement (docs/sharp-text.md): exact pixels for tiles of the source screen that
// have stopped changing, composited over the video in VideoStream::resolveMips() before mipmaps are built.
// Network thread: TCP from the source Mac's pinpin-sharp sender, raw-deflate RGB tiles + invalidations.
// Render thread: uploads tiles into an RGB texture and keeps a per-tile mask; the blit shader uses a tile's
// exact pixels only while its mask is set. Without a sender, or with any mismatch, the panel shows the video.
// Configured by files/sharp.json: {"host": "...", "port": 57700, "source": "LAPTOP"}.
#pragma once
#include <GLES3/gl3.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>
#undef far  // zconf.h defines DOS-era far/near as empty macros; room_lab.cpp uses `far` as a name
#undef near
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct SharpLayer {
    std::string host, source;
    int port = 57700;
    std::atomic<bool> stop{false};
    std::thread net;

    std::mutex lock;  // guards everything below shared with the render thread
    std::string state = "off";
    int width = 0, height = 0, tile = 0, tilesX = 0, tilesY = 0;
    std::vector<uint8_t> mask;                          // 255 = exact pixels uploaded and still current
    struct Tile { int index; std::vector<uint8_t> rgb; };
    std::vector<Tile> pending;                          // received, not yet uploaded
    bool maskDirty = false, reset = false;
    long tilesIn = 0, invalidations = 0, bytes = 0, connects = 0;

    // render-thread state
    GLuint tex = 0, maskTex = 0;
    int texW = 0, texH = 0, maskW = 0, maskH = 0;

    static std::string field(const std::string& j, const char* key) {
        std::string k = std::string("\"") + key + "\":";
        size_t p = j.find(k); if (p == std::string::npos) return "";
        p += k.size(); while (p < j.size() && j[p] == ' ') p++;
        if (p < j.size() && j[p] == '"') { size_t e = j.find('"', p + 1); return j.substr(p + 1, e - p - 1); }
        size_t e = j.find_first_of(",}", p); return j.substr(p, e - p);
    }
    void start(const std::string& dir) {
        std::ifstream f(dir + "/sharp.json"); if (!f) return;
        std::stringstream ss; ss << f.rdbuf(); std::string j = ss.str();
        host = field(j, "host"); source = field(j, "source");
        int p = atoi(field(j, "port").c_str()); if (p > 0) port = p;
        if (host.empty()) return;
        net = std::thread([this] { run(); });
    }
    void shutdown() { stop = true; if (net.joinable()) net.join(); }
    void setState(const std::string& s) { std::lock_guard<std::mutex> g(lock); state = s; }

    // ------------------------------------------------------------ network thread
    static bool readExact(int fd, uint8_t* p, size_t n) {
        while (n) { ssize_t r = recv(fd, p, n, 0); if (r <= 0) return false; p += r; n -= (size_t)r; }
        return true;
    }
    int connectSender() {
        int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return -1;
        int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int rcv = 8 << 20; setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
        timeval tv{3, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1 || connect(fd, (sockaddr*)&a, sizeof a) < 0) { close(fd); return -1; }
        timeval idle{6, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof idle);  // sender pings every 2 s
        return fd;
    }
    void clear() {
        std::lock_guard<std::mutex> g(lock);
        std::fill(mask.begin(), mask.end(), 0); pending.clear(); maskDirty = true;
    }
    void run() {
        std::vector<uint8_t> body, raw;
        while (!stop) {
            setState("connecting");
            int fd = connectSender();
            if (fd < 0) { setState("sender unreachable"); sleepCheck(2000); continue; }
            uint8_t h[11];
            if (!readExact(fd, h, sizeof h) || memcmp(h, "PPSH", 4) != 0 || h[4] != 1) { close(fd); setState("bad hello"); sleepCheck(2000); continue; }
            {
                std::lock_guard<std::mutex> g(lock);
                width = h[5] | h[6] << 8; height = h[7] | h[8] << 8; tile = h[9] | h[10] << 8;
                tilesX = tile ? (width + tile - 1) / tile : 0; tilesY = tile ? (height + tile - 1) / tile : 0;
                mask.assign((size_t)tilesX * tilesY, 0); pending.clear(); maskDirty = true; reset = true;
                connects++; state = "connected";
            }
            while (!stop && tile > 0) {
                uint8_t f[5];
                if (!readExact(fd, f, 5)) break;
                uint32_t n = f[1] | f[2] << 8 | f[3] << 16 | (uint32_t)f[4] << 24;
                if (n > (32u << 20)) break;
                body.resize(n);
                if (n && !readExact(fd, body.data(), n)) break;
                { std::lock_guard<std::mutex> g(lock); bytes += 5 + n; }
                if (f[0] == 1 && n >= 2) invalidate(body);
                else if (f[0] == 2 && n >= 6 && !tileIn(body, raw)) break;
            }
            close(fd);
            clear();  // lost sender: back to video everywhere
            setState("reconnecting");
            sleepCheck(1000);
        }
    }
    void invalidate(const std::vector<uint8_t>& b) {
        size_t count = b[0] | b[1] << 8;
        std::lock_guard<std::mutex> g(lock);
        for (size_t i = 0; i < count && 2 + i * 2 + 1 < b.size(); i++) {
            int idx = b[2 + i * 2] | b[3 + i * 2] << 8;
            if (idx < 0 || (size_t)idx >= mask.size()) continue;
            mask[idx] = 0; maskDirty = true; invalidations++;
            for (size_t k = 0; k < pending.size();) if (pending[k].index == idx) pending.erase(pending.begin() + k); else k++;
        }
    }
    bool tileIn(const std::vector<uint8_t>& b, std::vector<uint8_t>& raw) {
        int idx = b[0] | b[1] << 8;
        uint32_t rawLen = b[2] | b[3] << 8 | b[4] << 16 | (uint32_t)b[5] << 24;
        if (rawLen > (uint32_t)tile * tile * 3) return false;
        raw.resize(rawLen);
        if (b.size() - 6 == rawLen) memcpy(raw.data(), b.data() + 6, rawLen);  // sent stored
        else {
            z_stream z{}; if (inflateInit2(&z, -15) != Z_OK) return false;
            z.next_in = const_cast<uint8_t*>(b.data() + 6); z.avail_in = (uInt)(b.size() - 6);
            z.next_out = raw.data(); z.avail_out = rawLen;
            int r = inflate(&z, Z_FINISH); inflateEnd(&z);
            if (r != Z_STREAM_END || z.avail_out != 0) return false;
        }
        std::lock_guard<std::mutex> g(lock);
        if (idx < 0 || (size_t)idx >= mask.size()) return true;
        for (size_t k = 0; k < pending.size();) if (pending[k].index == idx) pending.erase(pending.begin() + k); else k++;
        pending.push_back({idx, raw}); tilesIn++;
        return true;
    }
    void sleepCheck(int ms) { for (int i = 0; i < ms / 50 && !stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

    // ------------------------------------------------------------ render thread
    // Upload new tiles and the mask; true when the layer applies to a video of this size from `videoSource`.
    bool apply(int videoW, int videoH, const std::string& videoSource) {
        std::vector<Tile> up; std::vector<uint8_t> m; bool newMask = false; int w, h, t, tx, ty;
        {
            std::lock_guard<std::mutex> g(lock);
            if (state != "connected" || width != videoW || height != videoH || (!source.empty() && source != videoSource)) return false;
            w = width; h = height; t = tile; tx = tilesX; ty = tilesY;
            if (reset) { reset = false; texW = 0; }
            up.swap(pending);
            for (auto& u : up) mask[u.index] = 255;
            if (maskDirty || !up.empty()) { m = mask; newMask = true; maskDirty = false; }
        }
        if (!tex || texW != w || texH != h) {
            if (tex) glDeleteTextures(1, &tex);
            glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, w, h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            texW = w; texH = h;
        }
        if (!maskTex || maskW != tx || maskH != ty) {
            if (maskTex) glDeleteTextures(1, &maskTex);
            glGenTextures(1, &maskTex); glBindTexture(GL_TEXTURE_2D, maskTex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, tx, ty);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            maskW = tx; maskH = ty; newMask = true;
            std::lock_guard<std::mutex> g(lock); m = mask;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, tex);
        for (auto& u : up) {
            int x0 = (u.index % tx) * t, y0 = (u.index / tx) * t, cw = std::min(t, w - x0);
            int ch = cw > 0 ? (int)u.rgb.size() / (cw * 3) : 0;
            if (ch > 0) glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, cw, ch, GL_RGB, GL_UNSIGNED_BYTE, u.rgb.data());
        }
        if (newMask && !m.empty()) { glBindTexture(GL_TEXTURE_2D, maskTex); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tx, ty, GL_RED, GL_UNSIGNED_BYTE, m.data()); }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        return true;
    }
    std::string json() {
        std::lock_guard<std::mutex> g(lock);
        long exact = 0; for (uint8_t v : mask) exact += v ? 1 : 0;
        char b[384];
        snprintf(b, sizeof b, "{\"state\":\"%s\",\"host\":\"%s\",\"source\":\"%s\",\"size\":[%d,%d],\"tile\":%d,\"exactTiles\":%ld,\"tiles\":%zu,\"received\":%ld,\"invalidations\":%ld,\"bytes\":%ld,\"connects\":%ld}",
                 state.c_str(), host.c_str(), source.c_str(), width, height, tile, exact, mask.size(), tilesIn, invalidations, bytes, connects);
        return b;
    }
};
