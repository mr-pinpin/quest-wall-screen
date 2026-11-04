// Push-to-talk dictation: headset mic -> pause-cut segments -> the Mini's speech service -> text.
// Each segment is sent exactly once and transcribed on its own, in order, so a long dictation never turns
// into re-sending an ever-growing recording (the backlog problem of the earlier web client). Results come
// back through take() in recording order.
#pragma once
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Dictation {
    static constexpr int kRate = 16000, kFrame = 320;  // 20 ms analysis frames
    static constexpr float kMinSeg = 2.0f, kMaxSeg = 12.0f, kPause = 0.35f;
    std::string host = "192.168.2.1"; int port = 8765;
    AAudioStream* in = nullptr;
    int inRate = kRate, decim = 1;
    std::atomic<bool> recording{false};
    std::mutex lock; std::condition_variable cv;
    std::vector<int16_t> seg;  // current segment, 16 kHz mono
    int quietFrames = 0; double frameEnergy = 0; int frameFill = 0; float noise = 300;
    std::deque<std::vector<int16_t>> queue;  // segments waiting for the speech service
    std::deque<std::string> results;
    std::thread worker; std::atomic<bool> quit{false};
    std::atomic<long> segments{0}, sent{0}, failures{0}, busyRetries{0}; std::atomic<float> lastMs{0}, level{0};
    std::string state = "idle", lastText, lastError;
    std::chrono::steady_clock::time_point lastWarm{};

    static aaudio_data_callback_result_t onData(AAudioStream*, void* user, void* data, int32_t n) {
        auto* d = (Dictation*)user; d->feed((const int16_t*)data, n); return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    void feed(const int16_t* x, int n) {
        if (!recording) return;
        std::lock_guard<std::mutex> g(lock);
        for (int i = 0; i + decim <= n; i += decim) {  // 48 kHz -> 16 kHz by averaging (speech needs < 8 kHz)
            int acc = 0; for (int k = 0; k < decim; k++) acc += x[i + k];
            int16_t s = (int16_t)(acc / decim); seg.push_back(s);
            frameEnergy += (double)s * s;
            if (++frameFill == kFrame) {
                float rms = (float)std::sqrt(frameEnergy / kFrame); frameEnergy = 0; frameFill = 0;
                level = rms;
                bool quiet = rms < std::max(noise * 2.5f, 250.0f);
                if (quiet) noise = noise * 0.95f + rms * 0.05f;
                quietFrames = quiet ? quietFrames + 1 : 0;
                float len = seg.size() / (float)kRate;
                if ((len >= kMinSeg && quietFrames * kFrame >= kPause * kRate) || len >= kMaxSeg) cutLocked();
            }
        }
    }
    bool cutLocked() {
        // Drop segments that are nothing but silence.
        bool voiced = false; for (size_t i = 0; i < seg.size() && !voiced; i++) voiced = std::abs(seg[i]) > std::max(noise * 4, 900.0f);
        bool kept = voiced && seg.size() > kRate / 4;
        if (kept) { queue.push_back(std::move(seg)); segments++; event({Ev::Segment, ""}); cv.notify_one(); }
        seg.clear(); quietFrames = 0;
        return kept;
    }
    // DICTQ (room_lab/dictation.h): the same segments and transcripts as ordered events, so the utterance logic can
    // tell which words belong to which pause. A segment's event comes before its transcript's. Capped: nobody may read.
    struct Ev { enum Kind { Segment, Text, Failed } kind; std::string text; };
    std::deque<Ev> events;
    void event(Ev e) { events.push_back(std::move(e)); if (events.size() > 256) events.pop_front(); }  // under lock
    bool takeEvent(Ev& e) { std::lock_guard<std::mutex> g(lock); if (events.empty()) return false; e = std::move(events.front()); events.pop_front(); return true; }
    void clearEvents() { std::lock_guard<std::mutex> g(lock); events.clear(); }
    // Cut the current segment now (an utterance just ended); false if it held no voice and was dropped.
    bool cut() { std::lock_guard<std::mutex> g(lock); return recording && cutLocked(); }
    bool open() {
        AAudioStreamBuilder* b = nullptr; if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
        AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(b, 1);
        AAudioStreamBuilder_setSampleRate(b, 48000);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(b, onData, this);
        aaudio_result_t r = AAudioStreamBuilder_openStream(b, &in); AAudioStreamBuilder_delete(b);
        if (r != AAUDIO_OK) { in = nullptr; state = std::string("mic ") + AAudio_convertResultToText(r); return false; }
        inRate = AAudioStream_getSampleRate(in); decim = std::max(1, inRate / kRate);
        if ((r = AAudioStream_requestStart(in)) != AAUDIO_OK) { state = std::string("mic start ") + AAudio_convertResultToText(r); return false; }
        return true;
    }
    void start() {
        if (!in && !open()) return;
        { std::lock_guard<std::mutex> g(lock); seg.clear(); quietFrames = 0; }
        recording = true; state = "listening";
        if (!worker.joinable()) worker = std::thread([this] { run(); });
        // The model sleeps when idle (first request ~7 s, then ~0.2 s per 4 s of speech): wake it while the user starts talking.
        auto now = std::chrono::steady_clock::now();
        if (now - lastWarm > std::chrono::seconds(30)) {
            lastWarm = now;
            std::thread([this] { std::string r; post(wav(std::vector<int16_t>(kRate / 2, 0)), r); }).detach();
        }
    }
    void stop() {
        if (!recording) return;
        recording = false;
        { std::lock_guard<std::mutex> g(lock); cutLocked(); }
        // Release the mic so the system (and other apps) get it back between dictations.
        if (in) { AAudioStream_requestStop(in); AAudioStream_close(in); in = nullptr; }
        state = "transcribing";
    }
    // Nothing recording, queued, in flight or waiting to be typed.
    bool drained() { std::lock_guard<std::mutex> g(lock); return !recording && state == "idle" && queue.empty() && results.empty(); }
    // Recording or not: every segment so far transcribed and taken (the receptionist cuts utterances without stopping).
    std::atomic<bool> inflight{false};
    bool settled() { std::lock_guard<std::mutex> g(lock); return queue.empty() && results.empty() && !inflight; }
    bool take(std::string& out) {
        std::lock_guard<std::mutex> g(lock); if (results.empty()) return false;
        out = results.front(); results.pop_front(); return true;
    }
    std::string wav(const std::vector<int16_t>& s) {
        std::string w(44, '\0'); uint32_t data = s.size() * 2, riff = 36 + data, rate = kRate, br = kRate * 2, fmt = 16;
        uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
        memcpy(&w[0], "RIFF", 4); memcpy(&w[4], &riff, 4); memcpy(&w[8], "WAVEfmt ", 8); memcpy(&w[16], &fmt, 4);
        memcpy(&w[20], &pcm, 2); memcpy(&w[22], &ch, 2); memcpy(&w[24], &rate, 4); memcpy(&w[28], &br, 4);
        memcpy(&w[32], &align, 2); memcpy(&w[34], &bits, 2); memcpy(&w[36], "data", 4); memcpy(&w[40], &data, 4);
        w.append((const char*)s.data(), data); return w;
    }
    // Minimal HTTP/1.0 POST; returns status code and body.
    int post(const std::string& body, std::string& reply) {
        int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return -1;
        timeval tv{30, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); inet_pton(AF_INET, host.c_str(), &a.sin_addr);
        if (connect(fd, (sockaddr*)&a, sizeof a) != 0) { close(fd); return -2; }
        std::string req = "POST /transcribe HTTP/1.0\r\nHost: " + host + "\r\nContent-Type: audio/wav\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body;
        for (size_t off = 0; off < req.size();) { ssize_t k = send(fd, req.data() + off, req.size() - off, 0); if (k <= 0) { close(fd); return -3; } off += k; }
        std::string r; char buf[4096]; ssize_t k; while ((k = recv(fd, buf, sizeof buf, 0)) > 0) r.append(buf, k);
        close(fd);
        int code = 0; sscanf(r.c_str(), "HTTP/%*s %d", &code);
        size_t p = r.find("\r\n\r\n"); reply = p == std::string::npos ? "" : r.substr(p + 4); return code;
    }
    static std::string textField(const std::string& j) {
        size_t p = j.find("\"text\""); if (p == std::string::npos) return "";
        p = j.find('"', j.find(':', p) + 1); std::string out;
        for (size_t i = p + 1; i < j.size() && j[i] != '"'; i++) {
            if (j[i] == '\\' && i + 1 < j.size()) {
                char c = j[++i];
                if (c == 'u' && i + 4 < j.size()) {  // \uXXXX -> UTF-8 (BMP)
                    unsigned v = strtoul(j.substr(i + 1, 4).c_str(), nullptr, 16); i += 4;
                    if (v < 0x80) out += (char)v;
                    else if (v < 0x800) { out += (char)(0xC0 | v >> 6); out += (char)(0x80 | (v & 63)); }
                    else { out += (char)(0xE0 | v >> 12); out += (char)(0x80 | (v >> 6 & 63)); out += (char)(0x80 | (v & 63)); }
                } else out += c == 'n' ? '\n' : c;
            } else out += j[i];
        }
        return out;
    }
    void run() {
        while (!quit) {
            std::vector<int16_t> s;
            { std::unique_lock<std::mutex> g(lock); cv.wait_for(g, std::chrono::milliseconds(200), [this] { return !queue.empty() || quit; });
              if (queue.empty()) { if (!recording && state == "transcribing") state = "idle"; continue; }
              s = std::move(queue.front()); queue.pop_front(); inflight = true; }
            std::string body = wav(s), reply; int code = 0;
            auto t0 = std::chrono::steady_clock::now();
            for (int attempt = 0; attempt < 40 && !quit; attempt++) {  // the service runs one inference at a time
                code = post(body, reply);
                if (code != 429) break;
                busyRetries++; std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            lastMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (code == 200) {
                std::string t = textField(reply); sent++;
                std::lock_guard<std::mutex> g(lock); event({Ev::Text, t});
                if (!t.empty()) { results.push_back(t); lastText = t; }
            } else { failures++; lastError = "http " + std::to_string(code); std::lock_guard<std::mutex> g(lock); event({Ev::Failed, ""}); }
            inflight = false;
        }
    }
    void shutdown() { quit = true; cv.notify_all(); recording = false; if (worker.joinable()) worker.join(); if (in) { AAudioStream_close(in); in = nullptr; } }
    std::string json() {
        size_t q; { std::lock_guard<std::mutex> g(lock); q = queue.size(); }
        std::string t; for (char c : lastText) if (c != '"' && c != '\\') t += c;
        return "{\"state\":\"" + state + "\",\"recording\":" + std::to_string(recording ? 1 : 0) + ",\"segments\":" + std::to_string(segments.load()) +
               ",\"queued\":" + std::to_string(q) + ",\"sent\":" + std::to_string(sent.load()) + ",\"failures\":" + std::to_string(failures.load()) +
               ",\"busyRetries\":" + std::to_string(busyRetries.load()) + ",\"lastMs\":" + std::to_string((int)lastMs.load()) +
               ",\"level\":" + std::to_string((int)level.load()) + ",\"error\":\"" + lastError + "\",\"last\":\"" + t + "\"}";
    }
};
