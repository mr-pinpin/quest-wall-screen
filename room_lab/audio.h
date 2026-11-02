// Stream audio: Sunshine's Opus packets -> libopus -> ring buffer -> AAudio low-latency output.
// Wi-Fi delivers audio in bursts, so the ring rides at about kTargetMs and absorbs bursts up to kMaxMs. Past
// that it drops just the excess back to the midpoint (small, rare skips) rather than flushing to the target.
// Playback does not start until kTargetMs is buffered, and restarts that way after running dry.
#pragma once
#include <aaudio/AAudio.h>
#include <opus_multistream.h>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AudioOut {
    static constexpr int kMaxMs = 160, kTargetMs = 50;
    OpusMSDecoder* dec = nullptr;
    AAudioStream* stream = nullptr;
    int channels = 2, rate = 48000, frameSamples = 240;
    std::vector<int16_t> pcm, ring;  // ring is interleaved samples
    size_t head = 0, count = 0;      // read position and fill, in frames (one frame = `channels` samples)
    bool primed = false;             // false until kTargetMs is buffered
    std::mutex lock;
    std::atomic<long> packets{0}, underruns{0}, dropped{0}, decodeErrors{0};
    std::string state = "idle";

    static aaudio_data_callback_result_t onData(AAudioStream*, void* user, void* out, int32_t n) {
        auto* a = (AudioOut*)user; auto* o = (int16_t*)out; size_t cap = a->ring.size() / a->channels;
        std::lock_guard<std::mutex> g(a->lock);
        if (!a->primed && a->count >= (size_t)a->rate * kTargetMs / 1000) a->primed = true;
        int32_t have = a->primed ? (int32_t)std::min<size_t>(a->count, n) : 0;
        for (int32_t i = 0; i < have; i++) {
            size_t f = (a->head + i) % cap;
            for (int c = 0; c < a->channels; c++) o[i * a->channels + c] = a->ring[f * a->channels + c];
        }
        if (have < n) { memset(o + have * a->channels, 0, (n - have) * a->channels * sizeof(int16_t)); if (a->primed) { a->underruns++; a->primed = false; } }
        a->head = (a->head + have) % cap; a->count -= have;
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    static void onError(AAudioStream*, void* user, aaudio_result_t err) {
        // The output device changed or vanished (e.g. headphones); reopen from a fresh thread per AAudio rules.
        auto* a = (AudioOut*)user; a->state = std::string("device error ") + AAudio_convertResultToText(err);
        std::thread([a] { a->closeStream(); a->openStream(); }).detach();
    }
    bool openStream() {
        AAudioStreamBuilder* b = nullptr; if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { state = "no builder"; return false; }
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(b, channels);
        AAudioStreamBuilder_setSampleRate(b, rate);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(b, onData, this);
        AAudioStreamBuilder_setErrorCallback(b, onError, this);
        aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream); AAudioStreamBuilder_delete(b);
        if (r != AAUDIO_OK) { stream = nullptr; state = std::string("open ") + AAudio_convertResultToText(r); return false; }
        AAudioStream_setBufferSizeInFrames(stream, AAudioStream_getFramesPerBurst(stream) * 2);
        if ((r = AAudioStream_requestStart(stream)) != AAUDIO_OK) { state = std::string("start ") + AAudio_convertResultToText(r); return false; }
        state = "playing"; return true;
    }
    void closeStream() { if (stream) { AAudioStream_requestStop(stream); AAudioStream_close(stream); stream = nullptr; } }

    int init(const OPUS_MULTISTREAM_CONFIGURATION* cfg) {
        stop();
        channels = cfg->channelCount; rate = cfg->sampleRate; frameSamples = cfg->samplesPerFrame;
        int err = 0;
        dec = opus_multistream_decoder_create(rate, channels, cfg->streams, cfg->coupledStreams, cfg->mapping, &err);
        if (!dec) { state = "opus init " + std::to_string(err); return -1; }
        pcm.assign((size_t)frameSamples * channels, 0);
        ring.assign((size_t)rate * kMaxMs / 1000 * channels * 2, 0); head = count = 0; primed = false;
        packets = underruns = dropped = decodeErrors = 0;
        return openStream() ? 0 : -1;
    }
    void sample(const char* data, int len) {
        if (!dec) return;
        // data == nullptr means a lost packet: let opus conceal it.
        int n = opus_multistream_decode(dec, (const unsigned char*)data, data ? len : 0, pcm.data(), frameSamples, 0);
        if (n <= 0) { decodeErrors++; return; }
        packets++;
        std::lock_guard<std::mutex> g(lock);
        size_t cap = ring.size() / channels, maxFrames = (size_t)rate * kMaxMs / 1000, target = (size_t)rate * kTargetMs / 1000;
        if (count + n > maxFrames) {  // burst overflow: trim back to midway between target and max
            size_t skip = count + n - (target + maxFrames) / 2; skip = std::min(skip, count);
            head = (head + skip) % cap; count -= skip; dropped += (long)skip;
        }
        for (int i = 0; i < n; i++) {
            size_t f = (head + count + i) % cap;
            for (int c = 0; c < channels; c++) ring[f * channels + c] = pcm[(size_t)i * channels + c];
        }
        count += n;
    }
    void stop() {
        closeStream();
        if (dec) { opus_multistream_decoder_destroy(dec); dec = nullptr; }
        state = "idle";
    }
    std::string json() {
        size_t buffered; { std::lock_guard<std::mutex> g(lock); buffered = count; }
        return "{\"state\":\"" + state + "\",\"channels\":" + std::to_string(channels) + ",\"packets\":" + std::to_string(packets.load()) +
               ",\"bufferedMs\":" + std::to_string(rate ? (int)(buffered * 1000 / rate) : 0) + ",\"underruns\":" + std::to_string(underruns.load()) +
               ",\"droppedFrames\":" + std::to_string(dropped.load()) + ",\"decodeErrors\":" + std::to_string(decodeErrors.load()) + "}";
    }
};
