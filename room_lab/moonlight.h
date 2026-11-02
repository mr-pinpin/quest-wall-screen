// Game-grade source for the wall panel: moonlight-common-c streaming from the Mini's Sunshine
// (UDP video with FEC, encrypted control, HEVC) straight into VideoStream's hardware decoder.
// The session (RTSP URL, input key) is prepared by moonlight_host.py and pushed to files/moonlight/session.json.
#pragma once
extern "C" {
#include "Limelight.h"
}
#include <cstdarg>
#include <fstream>
#include <sstream>
#include "video.h"

struct MoonlightSource {
    VideoStream* video = nullptr;
    std::thread thread;
    std::atomic<bool> running{false};
    std::string host, url, appVersion, gfeVersion, stage = "idle", error;
    int width = 2560, height = 1440, fps = 72, bitrate = 60000, codecSupport = 1, rikeyid = 0;
    bool hevc = true;
    uint8_t rikey[16] = {};
    std::vector<uint8_t> frame;
    static MoonlightSource* self;
    std::mutex m;  // guards stage/error, written from library threads
    void set(std::string* f, const std::string& v) { std::lock_guard<std::mutex> g(m); *f = v; }
    std::string get(const std::string& f) { std::lock_guard<std::mutex> g(m); return f; }

    static std::string field(const std::string& j, const char* key) {
        std::string k = std::string("\"") + key + "\":";
        size_t p = j.find(k); if (p == std::string::npos) return "";
        p += k.size(); while (p < j.size() && j[p] == ' ') p++;
        if (p < j.size() && j[p] == '"') { size_t e = j.find('"', p + 1); return j.substr(p + 1, e - p - 1); }
        size_t e = j.find_first_of(",}", p); return j.substr(p, e - p);
    }
    bool load(const std::string& dir) {
        std::ifstream f(dir + "/moonlight/session.json"); if (!f) return false;
        std::stringstream ss; ss << f.rdbuf(); std::string j = ss.str();
        host = field(j, "host"); url = field(j, "rtspSessionUrl"); appVersion = field(j, "appVersion"); gfeVersion = field(j, "gfeVersion");
        width = atoi(field(j, "width").c_str()); height = atoi(field(j, "height").c_str()); fps = atoi(field(j, "fps").c_str());
        bitrate = atoi(field(j, "bitrateKbps").c_str()); codecSupport = atoi(field(j, "serverCodecModeSupport").c_str());
        hevc = field(j, "hevc") == "true"; rikeyid = atoi(field(j, "rikeyid").c_str());
        std::string k = field(j, "rikey");
        for (int i = 0; i < 16 && (size_t)i * 2 + 1 < k.size(); i++) rikey[i] = (uint8_t)strtol(k.substr(i * 2, 2).c_str(), nullptr, 16);
        return !host.empty() && !url.empty() && width > 0;
    }

    // ---- moonlight-common-c callbacks (single active connection, so a static instance pointer is enough)
    static int drSetup(int format, int w, int h, int, void*, int) {
        self->hevc = (format & VIDEO_FORMAT_MASK_H265) != 0; self->width = w; self->height = h;
        self->video->setState(self->hevc ? "moonlight hevc" : "moonlight h264");
        return 0;
    }
    static int drSubmit(PDECODE_UNIT du) {
        auto& buf = self->frame; buf.clear();
        for (PLENTRY e = du->bufferList; e; e = e->next) buf.insert(buf.end(), (uint8_t*)e->data, (uint8_t*)e->data + e->length);
        {
            std::lock_guard<std::mutex> g(self->video->lock);
            self->video->hostLatencyMs = self->video->hostLatencyMs * 0.9 + du->frameHostProcessingLatency / 10.0 * 0.1;
            uint32_t rtt = 0, var = 0; if (LiGetEstimatedRttInfo(&rtt, &var)) self->video->rttMs = rtt;
        }
        bool ok = self->video->submit(buf.data(), buf.size(), du->frameType == FRAME_TYPE_IDR, (int64_t)du->presentationTimeUs,
                                      self->width, self->height, self->hevc);
        return ok ? DR_OK : DR_NEED_IDR;
    }
    static int arInit(int, const POPUS_MULTISTREAM_CONFIGURATION, void*, int) { return 0; }
    static void arSample(char*, int) {}
    static void clStageStarting(int s) { self->set(&self->stage, std::string("starting ") + LiGetStageName(s)); }
    static void clStageFailed(int s, int err) { self->set(&self->error, std::string(LiGetStageName(s)) + " failed " + std::to_string(err)); }
    static void clStarted() { self->set(&self->stage, "streaming"); }
    static void clTerminated(int err) { self->set(&self->stage, "terminated " + std::to_string(err)); self->running = false; }
    static void clLog(const char* fmt, ...) { va_list a; va_start(a, fmt); __android_log_vprint(ANDROID_LOG_INFO, "PinPinMoonlight", fmt, a); va_end(a); }

    void start(VideoStream* v) {
        video = v; self = this; v->source = "moonlight " + host; set(&error, ""); set(&stage, "connecting"); running = true;
        thread = std::thread([this] {
            SERVER_INFORMATION si; LiInitializeServerInformation(&si);
            si.address = host.c_str(); si.serverInfoAppVersion = appVersion.c_str(); si.serverInfoGfeVersion = gfeVersion.c_str();
            si.rtspSessionUrl = url.c_str(); si.serverCodecModeSupport = codecSupport;
            STREAM_CONFIGURATION sc; LiInitializeStreamConfiguration(&sc);
            sc.width = width; sc.height = height; sc.fps = fps; sc.bitrate = bitrate; sc.packetSize = 1392;
            sc.streamingRemotely = STREAM_CFG_LOCAL; sc.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
            sc.supportedVideoFormats = hevc ? (VIDEO_FORMAT_H265 | VIDEO_FORMAT_H264) : VIDEO_FORMAT_H264;
            sc.clientRefreshRateX100 = fps * 100; sc.colorSpace = COLORSPACE_REC_709; sc.colorRange = COLOR_RANGE_LIMITED;
            sc.encryptionFlags = ENCFLG_AUDIO;
            memcpy(sc.remoteInputAesKey, rikey, 16); memset(sc.remoteInputAesIv, 0, 16);
            uint32_t iv = htonl((uint32_t)rikeyid); memcpy(sc.remoteInputAesIv, &iv, 4);
            CONNECTION_LISTENER_CALLBACKS cl; LiInitializeConnectionCallbacks(&cl);
            cl.stageStarting = clStageStarting; cl.stageFailed = clStageFailed; cl.connectionStarted = clStarted;
            cl.connectionTerminated = clTerminated; cl.logMessage = clLog;
            DECODER_RENDERER_CALLBACKS dr; LiInitializeVideoCallbacks(&dr);
            dr.setup = drSetup; dr.submitDecodeUnit = drSubmit; dr.capabilities = CAPABILITY_DIRECT_SUBMIT;
            AUDIO_RENDERER_CALLBACKS ar; LiInitializeAudioCallbacks(&ar);
            ar.init = arInit; ar.decodeAndPlaySample = arSample; ar.capabilities = CAPABILITY_DIRECT_SUBMIT;
            int r = LiStartConnection(&si, &sc, &cl, &dr, &ar, nullptr, 0, nullptr, 0);
            if (r != 0) { if (get(error).empty()) set(&error, "LiStartConnection " + std::to_string(r)); set(&stage, "failed"); running = false; }
        });
    }
    void stop() {
        if (thread.joinable()) { LiStopConnection(); thread.join(); }
        running = false; set(&stage, "stopped");
    }
    std::string json() {
        return "{\"stage\":\"" + get(stage) + "\",\"error\":\"" + get(error) + "\",\"host\":\"" + host + "\",\"mode\":\"" + std::to_string(width) + "x" +
               std::to_string(height) + "@" + std::to_string(fps) + "\",\"kbps\":" + std::to_string(bitrate) + "}";
    }
};
MoonlightSource* MoonlightSource::self = nullptr;
