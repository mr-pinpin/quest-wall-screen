// displays_watch.h: when pinpin-displays should re-create a virtual display, or give up and exit (flag session-watch).
// Plain C, no macOS APIs, so bench/displays_watch_test.c runs it on the VPS.
//
// 2026-11-05 17:45 UTC: WindowServer was SIGKILLed (a Mach-port leak). Every virtual display died with it, but
// pinpin-displays (orphaned: its PinPin Host parent was gone) kept listing all 8, so "already exists" skipped every
// re-create and air_smoke found no displays (work-ad). The helper now compares its list with the online display list
// every few seconds:
//   - a display missing for `need` checks in a row is re-created from its saved size, at most once per `minGap`
//     seconds per screen: each create/remove raises colorsync's polling until a reboot (K24), so never churn;
//   - when the session itself is gone (no online display at all, the built-in included, for `need` checks), or a
//     screen was re-created `maxRecreates` times within `window` seconds and is still missing, re-creating from this
//     process cannot work (its WindowServer connection is dead): it exits, so its owner (PinPin Host) starts a fresh
//     one in the live session, which re-creates the saved set and frees :57710.
//   - a process that started before the running WindowServer belongs to a session that is gone (WindowServer
//     restarts only with a new session): it exits (pinpin-displays) or is swept (PinPin Host, K31). A plugin that
//     merely lost its Host parent is NOT stale: Host restarts leave plugins running for the next Host to adopt.
//     dw_stale is that one rule, shared by both.
#ifndef DISPLAYS_WATCH_H
#define DISPLAYS_WATCH_H

typedef struct {
    int sid;              // screen id (1..64)
    unsigned did;         // its CGDirectDisplayID now
    int missing;          // consecutive checks it was not online
    double recreatedAt[4];  // times of the last re-creates (ring), 0 = none
    int nRecreated;       // total re-creates of this screen
} dw_entry;

typedef struct {
    int need;             // consecutive missing checks before acting
    double minGap;        // seconds between two re-creates of one screen
    int maxRecreates;     // re-creates within `window` before giving up (exit)
    double window;
} dw_params;

static const dw_params DW_DEFAULT = {3, 300.0, 2, 900.0};

enum { DW_OK = 0, DW_WAIT = 1, DW_RECREATE = 2, DW_EXIT = 3 };

static inline int dw_online(unsigned did, const unsigned *online, int n) {
    for (int i = 0; i < n; i++) if (online[i] == did) return 1;
    return 0;
}

static inline int dw_recent(const dw_entry *e, double now, double window) {
    int k = 0;
    for (int i = 0; i < 4; i++) if (e->recreatedAt[i] > 0 && now - e->recreatedAt[i] <= window) k++;
    return k;
}

// One check of one screen. On DW_RECREATE the caller re-creates it and stores the new id in e->did.
static inline int dw_step(dw_entry *e, const unsigned *online, int n, double now, const dw_params *p) {
    if (dw_online(e->did, online, n)) { e->missing = 0; return DW_OK; }
    e->missing++;
    if (e->missing < p->need) return DW_WAIT;
    if (dw_recent(e, now, p->window) >= p->maxRecreates) return DW_EXIT;  // re-creating does not stick: dead connection
    for (int i = 0; i < 4; i++)
        if (e->recreatedAt[i] > 0 && now - e->recreatedAt[i] < p->minGap) return DW_WAIT;
    e->recreatedAt[e->nRecreated % 4] = now; e->nRecreated++; e->missing = 0;
    return DW_RECREATE;
}

// The session as a whole: no online display at all (not even the built-in) for `need` checks -> exit.
// `*empty` counts consecutive empty checks across calls; n < 0 means the online list call itself failed.
static inline int dw_session(int n, int *empty, const dw_params *p) {
    if (n > 0) { *empty = 0; return DW_OK; }
    return ++*empty >= p->need ? DW_EXIT : DW_WAIT;
}

// 1 when a process started at procStart (epoch s) predates the WindowServer started at wsStart: its session is gone.
// Unknown times (0) are never stale; 2 s of slack for clock granularity.
static inline int dw_stale(long procStart, long wsStart) {
    return procStart > 0 && wsStart > 0 && wsStart > procStart + 2;
}

#endif
