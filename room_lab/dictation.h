// Always-on dictation (kanban 2026-11-04 item 3: "keep listening while the assistant replies, queue it, never lock
// up; don't cut a sentence at a breath"). The logic above voice.h's audio segmenter, GL- and audio-free:
//  - Mic: one owner at a time (the Claude button > the fist > always-on). A higher owner preempts; when it lets go
//    the mic falls back to always-on (if on). Owners renew a lease each frame; a forgotten owner expires in leaseS.
//  - Listener: voice activity (per frame) + voice.h's segments (cut at 0.35 s pauses) + their transcripts (in order)
//    become utterances. An utterance ends on silence: never at a breath (<= breathS), after endS (3.5 s), sooner
//    (endPunctS) when the words so far end a sentence, later when this speaker already resumed after long pauses;
//    or at once when it ends with the end phrase ("send", stripped). Each ended utterance waits for its last
//    transcripts, then joins the queue; the queue goes out one message at a time, each after the previous reply.
//    Listening never stops while a reply streams. Every waiting state has a watchdog, so nothing can stick.
// Integration (room_lab.cpp): Mic decides who gets dictation results; Listener.voice() from dictation.level each
// frame, segment() from cutLocked(), text()/failed() from run(), replyDone() when the assistant's stream ends;
// Out.sends go to Out.route's target; Out.cutNow asks voice.h to cut the current segment now (a
// new Dictation::cut() that takes the lock and calls cutLocked()). bench/dictation_test.cpp.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

namespace dictsm {

enum class Owner { None = 0, Always = 1, Fist = 2, Button = 3 };  // higher number = higher priority
inline const char* name(Owner o) { return o == Owner::Always ? "always" : o == Owner::Fist ? "fist" : o == Owner::Button ? "button" : "none"; }

struct Mic {
    bool alwaysOn = false; float leaseS = 3.0f;
    Owner owner = Owner::None; float leaseUntil = 0; long preemptions = 0, expiries = 0, denied = 0;
    // Take the mic: free, already ours, or a lower owner holds it (it is preempted). Returns whether o holds it now.
    bool acquire(Owner o, float now) {
        expire(now);
        if (owner == o || owner == Owner::None || (int)o > (int)owner) {
            if (owner != o && owner != Owner::None) preemptions++;
            owner = o; leaseUntil = now + leaseS; return true;
        }
        denied++; return false;
    }
    void touch(Owner o, float now) { if (owner == o) leaseUntil = now + leaseS; }
    void release(Owner o, float now) { if (owner == o) fallBack(now); }
    void setAlwaysOn(bool on, float now) {
        alwaysOn = on;
        if (!on && owner == Owner::Always) owner = Owner::None;
        if (on && owner == Owner::None) acquire(Owner::Always, now);
    }
    Owner tick(float now) {
        expire(now);
        if (owner == Owner::Always) leaseUntil = now + leaseS;  // always-on is held by the app itself
        return owner;
    }
    void expire(float now) { if (owner != Owner::None && owner != Owner::Always && now > leaseUntil) { expiries++; fallBack(now); } }
    void fallBack(float now) { owner = Owner::None; if (alwaysOn) { owner = Owner::Always; leaseUntil = now + leaseS; } }
};

struct Params {
    float breathS = 1.5f;      // a pause this long never ends an utterance
    float endS = 3.5f;         // silence that ends one
    float endPunctS = 2.2f;    // ... when the words so far end a sentence (. ? !) and the last word is not a continuation
    float adaptMaxS = 5.0f;    // a speaker who resumed after long pauses gets up to this (1.25 x their longest)
    float maxUtterS = 45.0f;   // watchdog: an utterance (or a voice detector stuck on) is cut after this
    float closeWaitS = 12.0f;  // watchdog: wait this long for the last transcripts (a cold model takes ~7 s)
    float replyTimeoutS = 90.0f;  // watchdog: a reply that never ends frees the queue after this
    float cutPauseS = 0.6f;    // ask for a segment cut at a pause this long: voice.h's 2 s minimum would hold back a lone
                               // "send" (or a sentence's last words) by up to 2 s; 0 leaves cutting to voice.h
    std::vector<std::string> endPhrases{"send", "send it"};
};

enum class State { Idle, Hearing, Pausing, Finishing, Sending };
inline const char* name(State s) { return s == State::Hearing ? "hearing" : s == State::Pausing ? "pausing" : s == State::Finishing ? "finishing" : s == State::Sending ? "sending" : "idle"; }

struct Send { std::string text; Owner route; };
struct Out {
    std::vector<Send> sends;   // send each now, in order
    bool cutNow = false;       // ask voice.h to cut its current segment (an utterance just ended mid-segment)
    State state = State::Idle;
    std::string caption;       // the words of the utterance in progress
    float pauseFrac = 0;       // silence so far / the silence that would end it (for a HUD ring)
    int queued = 0;            // finished utterances waiting for the reply
};

struct Listener {
    Params p;
    struct Utt {
        long id; Owner route; float start, lastVoice, endedAt = -1; std::string text;
        int segsOut = 0;              // segments assigned and not yet transcribed
        bool voicedSinceSeg = false;  // voice heard that no segment carries yet
        float longestPause = 0; bool ended = false, endPhrase = false;
    };
    std::deque<Utt> utts;  // in order: the last may be in progress, the rest ended and waiting for transcripts
    std::deque<long> segOwner;  // per segment in flight, its utterance id (-1: no voice, transcript dropped)
    std::deque<Send> ready;
    bool busy = false, voiceWas = false; float busySince = 0; long nextId = 1;
    long sent = 0, replyTimeouts = 0, closeTimeouts = 0, forcedCuts = 0, droppedTexts = 0, lateTexts = 0;

    Utt* current() { return !utts.empty() && !utts.back().ended ? &utts.back() : nullptr; }
    Utt* byId(long id) { for (auto& u : utts) if (u.id == id) return &u; return nullptr; }

    static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\n"), b = s.find_last_not_of(" \t\n");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }
    // The words without trailing punctuation, lower case.
    static std::string bare(const std::string& s) {
        std::string t = lower(trim(s));
        while (!t.empty() && (std::ispunct((unsigned char)t.back()) || (unsigned char)t.back() >= 0x80 || t.back() == ' ')) t.pop_back();
        return t;
    }
    static std::string lastWord(const std::string& s) { std::string t = bare(s); size_t k = t.find_last_of(' '); return k == std::string::npos ? t : t.substr(k + 1); }
    static bool sentenceEnd(const std::string& text) {
        std::string t = trim(text); if (t.empty()) return false;
        if (t.size() >= 3 && t.compare(t.size() - 3, 3, "...") == 0) return false;       // trailing off
        if (t.size() >= 3 && t.compare(t.size() - 3, 3, "\xE2\x80\xA6") == 0) return false;  // ...
        char c = t.back(); if (c != '.' && c != '?' && c != '!') return false;
        static const char* cont[] = {"of", "the", "a", "an", "and", "or", "but", "to", "some", "with", "for", "in", "on", "at", "my", "your", "this", "that", "like", "um", "uh", "so", "then", "type"};
        std::string w = lastWord(t); for (auto* x : cont) if (w == x) return false;
        return true;
    }
    float threshold(const Utt& u) const {
        float s = sentenceEnd(u.text) ? p.endPunctS : p.endS;
        s = std::max(s, std::min(p.adaptMaxS, u.longestPause * 1.25f));
        return std::max(s, p.breathS + 0.5f);
    }
    static std::vector<std::string> words(const std::string& s) {
        std::vector<std::string> w; size_t i = 0;
        while (i < s.size()) { size_t j = s.find(' ', i); if (j == std::string::npos) j = s.size(); if (j > i) w.push_back(s.substr(i, j - i)); i = j + 1; }
        return w;
    }
    // Strip a trailing end phrase ("... gold rectangle. Send." -> "... gold rectangle."); true if there was one.
    bool stripEnd(std::string& text) const {
        std::vector<std::string> w = words(text);
        for (auto& e : p.endPhrases) {
            std::vector<std::string> ew = words(e);
            if (ew.empty() || ew.size() > w.size()) continue;
            bool match = true;
            for (size_t k = 0; k < ew.size() && match; k++) match = bare(w[w.size() - ew.size() + k]) == ew[k];
            if (!match) continue;
            w.resize(w.size() - ew.size()); text.clear();
            for (auto& x : w) text += (text.empty() ? "" : " ") + x;
            while (!text.empty() && (text.back() == ',' || text.back() == ' ')) text.pop_back();
            return true;
        }
        return false;
    }

    // Per frame: is the user speaking (voice.h's level over its noise floor)? route = the mic's owner now.
    void voice(float now, bool speaking, Owner route) {
        Utt* u = current();
        if (u && u->route != route) { end(*u, now); u = nullptr; }  // the mic changed hands: what was said so far goes to the old owner
        if (speaking) {
            if (!u) { utts.push_back({nextId++, route, now, now}); u = &utts.back(); }
            if (!voiceWas) u->longestPause = std::max(u->longestPause, now - u->lastVoice);
            u->lastVoice = now; u->voicedSinceSeg = true;
        }
        voiceWas = speaking;
    }
    // voice.h queued a segment for transcription.
    void segment(float) {
        for (auto& u : utts) if (u.voicedSinceSeg) { u.voicedSinceSeg = false; u.segsOut++; segOwner.push_back(u.id); return; }
        segOwner.push_back(-1);
    }
    // The next segment's transcript (in order). Empty text is fine.
    void text(float now, const std::string& t) {
        if (segOwner.empty()) { droppedTexts++; return; }
        long id = segOwner.front(); segOwner.pop_front();
        Utt* u = id < 0 ? nullptr : byId(id);
        if (!u) { if (id >= 0) lateTexts++; else droppedTexts++; return; }
        u->segsOut--;
        std::string s = trim(t); if (s.empty()) return;
        u->text += (u->text.empty() ? "" : " ") + s;
        if (stripEnd(u->text)) { u->endPhrase = true; if (!u->ended) end(*u, now); }
    }
    void failed(float now) { text(now, ""); }
    void replyDone(float) { busy = false; }

    void end(Utt& u, float now) { if (!u.ended) { u.ended = true; u.endedAt = now; } }

    Out tick(float now) {
        Out o;
        if (Utt* u = current()) {
            float silent = now - u->lastVoice, th = threshold(*u);
            if (!voiceWas && silent >= th) end(*u, now);
            else if (now - u->start >= p.maxUtterS) { end(*u, now); forcedCuts++; }
            if (!u->ended && !voiceWas && p.cutPauseS > 0 && silent >= p.cutPauseS && u->voicedSinceSeg) o.cutNow = true;
            if (!u->ended) { o.pauseFrac = voiceWas ? 0 : std::min(1.0f, silent / th); o.caption = u->text; }
        }
        for (auto& e : utts) if (e.ended && e.voicedSinceSeg) o.cutNow = true;  // ended mid-segment: its words must go out now
        // Ended utterances join the queue in order once their transcripts are in (or the watchdog gives up).
        while (!utts.empty() && utts.front().ended) {
            Utt& f = utts.front();
            bool complete = f.segsOut <= 0 && !f.voicedSinceSeg;
            if (!complete && now - f.endedAt < p.closeWaitS) break;
            if (!complete) closeTimeouts++;
            std::string t = trim(f.text);
            if (!t.empty()) ready.push_back({t, f.route});
            utts.pop_front();
        }
        if (busy && now - busySince > p.replyTimeoutS) { busy = false; replyTimeouts++; }
        if (!busy && !ready.empty()) { o.sends.push_back(ready.front()); ready.pop_front(); busy = true; busySince = now; sent++; }
        o.queued = (int)ready.size();
        Utt* u = current();
        o.state = u ? (voiceWas ? State::Hearing : State::Pausing) : !utts.empty() ? State::Finishing : busy ? State::Sending : State::Idle;
        return o;
    }
};

}  // namespace dictsm
