// Always-on dictation logic (room_lab/dictation.h). g++ -std=c++17 -I room_lab bench/dictation_test.cpp && ./a.out
// Simulated timelines at 50 Hz: phrases of speech, voice.h's segmenter (cut at a 0.35 s pause once 2 s long, 12 s
// max), a transcription worker (one at a time, in order, with latency), and an assistant whose replies take time.
#include "dictation.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dictsm;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }

struct Phrase { float a, b; std::string text; };
struct Sim {
    std::vector<Phrase> speech;
    float latency = 0.8f, coldLatency = 0.8f, reply = 3.0f, dt = 0.02f;
    bool replyNever = false, loseTexts = false; float stuckVoiceFrom = -1, stuckVoiceTo = -1;
    std::vector<std::pair<float, Owner>> owners;  // (time, owner) changes of the mic owner, applied in order
    Listener L; Mic mic;
    struct Sent { float t; std::string text; Owner route; };
    std::vector<Sent> sent; std::vector<State> states; float maxBusy = 0, maxFinishing = 0, maxHearing = 0;
    bool listenedWhileBusy = false;

    bool speaking(float t) const {
        if (t >= stuckVoiceFrom && t < stuckVoiceTo) return true;
        for (auto& p : speech) if (t >= p.a && t < p.b) return true;
        return false;
    }
    void run(float T) {
        // segmenter state
        float segStart = 0, quiet = 0; std::string segText; bool segVoiced = false;
        struct Job { float done; std::string text; }; std::vector<Job> jobs; float workerFree = 0; bool cold = true;
        std::vector<bool> landed(speech.size(), false);
        float replyAt = -1, busyStart = -1, finStart = -1, hearStart = -1; size_t ownerK = 0; Owner route = Owner::Always;
        auto cut = [&](float t) {
            if (segVoiced) {
                L.segment(t);
                float start = std::max(t, workerFree), lat = cold ? coldLatency : latency; cold = false;
                workerFree = start + lat; jobs.push_back({workerFree, segText});
            }
            segText.clear(); segVoiced = false; segStart = t; quiet = 0;
        };
        for (int i = 0; i * dt < T; i++) {
            float t = i * dt;
            while (ownerK < owners.size() && owners[ownerK].first <= t) route = owners[ownerK++].second;
            bool sp = speaking(t);
            for (size_t k = 0; k < speech.size(); k++) if (!landed[k] && t >= speech[k].b) { landed[k] = true; segText += (segText.empty() ? "" : " ") + speech[k].text; }  // a phrase's words land when it ends
            if (sp) { segVoiced = true; quiet = 0; } else quiet += dt;
            L.voice(t, sp, route);
            float len = t - segStart;
            if ((len >= 2.0f && quiet >= 0.35f) || len >= 12.0f) cut(t);
            while (!jobs.empty() && jobs.front().done <= t) { if (!loseTexts) L.text(t, jobs.front().text); jobs.erase(jobs.begin()); }
            if (replyAt >= 0 && t >= replyAt) { L.replyDone(t); replyAt = -1; }
            Out o = L.tick(t);
            if (o.cutNow) cut(t);
            for (auto& s : o.sends) { sent.push_back({t, s.text, s.route}); if (!replyNever) replyAt = t + reply; }
            states.push_back(o.state);
            // watchdog bookkeeping: how long each waiting condition lasted
            if (!o.sends.empty()) busyStart = t;  // one reply at a time: measure each
            if (L.busy) { if (busyStart < 0) busyStart = t; maxBusy = std::max(maxBusy, t - busyStart); if (sp && o.state == State::Hearing) listenedWhileBusy = true; } else busyStart = -1;
            bool fin = !L.utts.empty() && L.utts.front().ended;
            if (fin) { if (finStart < 0) finStart = t; maxFinishing = std::max(maxFinishing, t - finStart); } else finStart = -1;
            if (L.current()) { if (hearStart < 0) hearStart = L.current()->start; maxHearing = std::max(maxHearing, t - L.current()->start); }
        }
    }
};

int main() {
    // 1. The user's case: "place some type of... [breath] ...gold rectangle" is one message, for every breath up to 1.5 s.
    for (float breath : {0.4f, 0.8f, 1.2f, 1.5f}) {
        Sim s; float b = 2.3f + breath;
        s.speech = {{0.5f, 2.3f, "Place some type of..."}, {b, b + 0.9f, "gold rectangle."}};
        s.run(15);
        char m[160]; snprintf(m, sizeof m, "breath %.1f s: one message 'Place some type of... gold rectangle.'", breath);
        check(s.sent.size() == 1 && s.sent[0].text == "Place some type of... gold rectangle.", m);
        if (breath == 1.2f && !s.sent.empty()) printf("  user's case (1.2 s breath): sent %.1f s after the last word: \"%s\"\n", s.sent[0].t - (b + 0.9f), s.sent[0].text.c_str());
    }
    { Sim s; s.speech = {{0.5f, 2.3f, "Place some type of."}, {3.8f, 4.7f, "gold rectangle."}}; s.run(15);
      check(s.sent.size() == 1, "a period after 'of' (the recogniser guessing) does not end the sentence early"); }
    // ... and a longer think mid-sentence (3 s after "of") still does not cut.
    { Sim s; s.speech = {{0.5f, 2.3f, "place some type of"}, {5.3f, 6.2f, "gold rectangle"}}; s.run(15);
      check(s.sent.size() == 1, "3 s pause mid-sentence: still one message"); }

    // 2. Two requests 4.5 s apart: two messages, in order.
    { Sim s; s.speech = {{0.5f, 2.0f, "Make the screen bigger."}, {6.5f, 8.0f, "Now turn on the lights."}}; s.run(25);
      check(s.sent.size() == 2 && s.sent[0].text == "Make the screen bigger." && s.sent[1].text == "Now turn on the lights.", "two utterances, two messages in order"); }

    // 3. Sentence end: a question ends after ~2.2 s instead of 3.5 s.
    { Sim s; s.latency = s.coldLatency = 0.3f; s.speech = {{0.5f, 2.0f, "What time is it?"}}; s.run(10);
      check(s.sent.size() == 1 && s.sent[0].t - 2.0f < 2.6f && s.sent[0].t - 2.0f >= 2.15f, "question: sent 2.2-2.6 s after the last word");
      if (!s.sent.empty()) printf("  question: sent %.2f s after the last word\n", s.sent[0].t - 2.0f); }

    // 4. End phrase "send": goes as soon as its transcript is in, with "send" stripped.
    { Sim s; s.latency = s.coldLatency = 0.5f; s.speech = {{0.5f, 2.5f, "Turn off the lights."}, {2.9f, 3.3f, "Send."}}; s.run(10);
      check(s.sent.size() == 1 && s.sent[0].text == "Turn off the lights.", "end phrase stripped");
      check(!s.sent.empty() && s.sent[0].t - 3.3f < 1.5f, "end phrase sends without waiting for the silence");
      if (!s.sent.empty()) printf("  \"... Send.\": sent %.2f s after the last word\n", s.sent[0].t - 3.3f); }
    { Listener L; std::string a = "please send it", b = "send the email to Bob", c = "Send", d = "the ascend";
      check(L.stripEnd(a) && a == "please", "'send it' stripped"); check(!L.stripEnd(b), "'send' mid-sentence is content");
      check(L.stripEnd(c) && c.empty(), "bare 'send' -> nothing to send"); check(!L.stripEnd(d), "'ascend' is not 'send'"); }

    // 5. The assistant is replying: the mic keeps listening, what is said is queued and sent in order after each reply.
    { Sim s; s.reply = 10; s.latency = s.coldLatency = 0.6f;
      s.speech = {{0.5f, 2.0f, "First request."}, {7.0f, 8.5f, "Second one."}, {13.0f, 14.5f, "Third one."}}; s.run(60);
      check(s.sent.size() == 3, "three messages");
      if (s.sent.size() == 3) {
          check(s.sent[0].text == "First request." && s.sent[1].text == "Second one." && s.sent[2].text == "Third one.", "in order");
          check(s.sent[1].t >= s.sent[0].t + 10 - 0.03f && s.sent[2].t >= s.sent[1].t + 10 - 0.03f, "each after the previous reply");
          printf("  during replies: sent at %.1f, %.1f, %.1f s (replies take 10 s)\n", s.sent[0].t, s.sent[1].t, s.sent[2].t);
      }
      check(s.listenedWhileBusy, "hearing speech while a reply streams"); }

    // 6. Watchdogs: nothing waits forever.
    { Sim s; s.replyNever = true; s.speech = {{0.5f, 2.0f, "One."}, {8, 9, "Two."}}; s.run(200);
      check(s.sent.size() == 2 && s.sent[1].t - s.sent[0].t >= 90 && s.sent[1].t - s.sent[0].t < 91, "a reply that never ends frees the queue after 90 s");
      check(s.L.replyTimeouts >= 1, "reply timeout counted"); }
    { Sim s; s.loseTexts = true; s.speech = {{0.5f, 2.0f, "lost"}, {20, 21, "also lost"}}; s.run(40);
      check(s.sent.empty() && s.L.utts.empty() && s.L.closeTimeouts == 2, "transcripts never arrive: each utterance given up after 12 s");
      check(s.maxFinishing <= 12.05f, "finishing never longer than closeWaitS"); }
    { Sim s; s.stuckVoiceFrom = 1; s.stuckVoiceTo = 70; s.speech = {{1, 3, "hello"}}; s.run(80);
      check(s.L.forcedCuts >= 1 && s.maxHearing <= 45.05f, "voice detector stuck on: utterance cut at 45 s"); }
    { Sim s; s.coldLatency = 7; s.speech = {{0.5f, 2.0f, "Cold model."}}; s.run(20);
      check(s.sent.size() == 1 && s.sent[0].text == "Cold model.", "a cold model's 7 s first transcript still makes it"); }

    // A cut that voice.h finds silent (its own voiced test is stricter than ours): the utterance does not wait 12 s for it.
    { Listener L; L.voice(0, true, Owner::Always); L.voice(0.5f, false, Owner::Always);
      Out o; float t = 0.5f; for (; t < 5; t += 0.02f) { L.voice(t, false, Owner::Always); o = L.tick(t); if (o.cutNow) { L.unvoiced(t); break; } }
      bool cleared = true; for (; t < 6; t += 0.02f) { L.voice(t, false, Owner::Always); if (L.tick(t).cutNow) cleared = false; }
      check(cleared && L.utts.empty(), "a silent forced cut: no endless cut requests, the empty utterance is dropped"); }

    // 7. One owner of the mic.
    { Mic m; m.setAlwaysOn(true, 0);
      check(m.tick(0) == Owner::Always, "always-on holds the mic");
      check(m.acquire(Owner::Button, 1) && m.owner == Owner::Button, "the Claude button preempts always-on");
      check(!m.acquire(Owner::Fist, 1.5f) && m.owner == Owner::Button, "a fist cannot take it from the button (no more fights)");
      m.touch(Owner::Button, 3); check(m.tick(5.5f) == Owner::Button, "a renewed lease holds");
      m.release(Owner::Button, 6); check(m.owner == Owner::Always, "released: back to always-on");
      check(m.acquire(Owner::Fist, 7), "fist over always-on");
      check(m.tick(10.5f) == Owner::Always && m.expiries == 1, "an owner that stops renewing expires back to always-on");
      m.setAlwaysOn(false, 11); check(m.tick(11) == Owner::None, "always-on off: free");
      for (int k = 0; k < 1000; k++) { Owner o = (Owner)(1 + rand() % 3); if (rand() % 2) m.acquire(o, 12 + k * 0.1f); else m.release(o, 12 + k * 0.1f); m.tick(12 + k * 0.1f); }
      check(m.owner == Owner::None || m.owner == Owner::Always || m.owner == Owner::Fist || m.owner == Owner::Button, "always one well-defined owner"); }
    { Sim s; s.owners = {{0, Owner::Always}, {1.4f, Owner::Button}}; s.speech = {{0.5f, 1.2f, "hello there"}, {1.5f, 2.5f, "screen two, open mail"}}; s.run(15);
      check(s.sent.size() == 2 && s.sent[0].route == Owner::Always && s.sent[1].route == Owner::Button && s.sent[1].text == "screen two, open mail",
            "the mic changing hands splits the utterance: each part to its own owner"); }

    // 8. Fuzz: 30 simulated minutes of random speech, latencies and replies; every phrase is sent exactly once, in order,
    // and no waiting state outlives its watchdog.
    { srand(7); int bad = 0, phrases = 0;
      for (int run = 0; run < 6; run++) {
          Sim s; float t = 0.5f; int k = 0;
          while (t < 290) { float d = 0.3f + (rand() % 300) / 100.0f; s.speech.push_back({t, t + d, "w" + std::to_string(k++)}); t += d + ((rand() % 4) ? 0.4f + (rand() % 110) / 100.0f : 3.6f + (rand() % 400) / 100.0f); }
          s.latency = 0.2f + (rand() % 200) / 100.0f; s.reply = 1 + rand() % 20; s.run(400);
          std::string all; for (auto& x : s.sent) all += (all.empty() ? "" : " ") + x.text;
          std::string want; for (auto& p : s.speech) want += (want.empty() ? "" : " ") + p.text;
          if (all != want) { bad++; if (getenv("DBG")) printf("    got  %s\n    want %s\n", all.substr(0, 300).c_str(), want.substr(0, 300).c_str()); }
          if (getenv("DBG")) printf("    fin %.1f busy %.1f hear %.1f lat %.2f reply %.0f\n", s.maxFinishing, s.maxBusy, s.maxHearing, s.latency, s.reply);
          if (s.maxFinishing > 12.05f || s.maxBusy > 90.05f || s.maxHearing > 45.05f) bad++;
          phrases += k;
      }
      printf("  fuzz: %d phrases over 30 simulated minutes\n", phrases);
      check(bad == 0, "fuzz: every phrase sent exactly once in order, watchdogs hold"); }

    printf("dictation: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
