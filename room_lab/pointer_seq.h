// The main screen's pointer over Moonlight (work-ad's diagnosis, 2026-11-04): the Mini's virtual HID posts a button at
// the Mini's CURRENT cursor, and drops it when that cursor is off the streamed display. room_lab deduped position
// events, so if anything moved the Mini's cursor since our last move, a pinch landed elsewhere or vanished.
// Rules: moves stay deduped; every press, release and scroll is preceded by the absolute position, always; the
// dedupe resets when the stream (re)connects. GL-free; bench/pointer_seq_test.cpp.
#pragma once

namespace pointerseq {

struct Seq {
    int sentX = -1, sentY = -1;
    template <class Pos> void move(int x, int y, Pos pos) { if (x != sentX || y != sentY) { pos(x, y); sentX = x; sentY = y; } }
    // Before a button or scroll: the position goes out whatever we think the Mini's cursor is.
    template <class Pos> void anchor(int x, int y, Pos pos) { pos(x, y); sentX = x; sentY = y; }
    void reset() { sentX = sentY = -1; }  // stream (re)connected: the Mini's cursor is unknown
};

}  // namespace pointerseq
