// bench/displays_watch_test.c: mac/displays_watch.h, the session-watch rules of pinpin-displays, on the VPS.
//   cc -std=c11 -Wall -I mac bench/displays_watch_test.c -o /tmp/dw && /tmp/dw
#include <stdio.h>
#include <string.h>
#include "displays_watch.h"

static int fails = 0;

// steps until the rule acts (anything but WAIT), at most 20 checks 10 s apart
static int until(dw_entry *e, const unsigned *on, int n, double *t, const dw_params *P) {
    int r = DW_WAIT;
    for (int i = 0; i < 20 && r == DW_WAIT; i++) r = dw_step(e, on, n, *t += 10, P);
    return r;
}
#define CHECK(c, msg) do { if (!(c)) { fails++; printf("FAIL %s\n", msg); } } while (0)

int main(void) {
    const dw_params *P = &DW_DEFAULT;
    unsigned all[] = {1, 5, 7, 16, 17}, builtinOnly[] = {1};
    dw_entry e; memset(&e, 0, sizeof e); e.sid = 48; e.did = 17;
    double t = 1000;

    // healthy: nothing to do
    for (int i = 0; i < 10; i++) CHECK(dw_step(&e, all, 5, t += 10, P) == DW_OK, "online display: ok");

    // a blip (one or two checks) is waited out, never re-created: re-creating churns colorsync (K24)
    CHECK(dw_step(&e, builtinOnly, 1, t += 10, P) == DW_WAIT, "missing once: wait");
    CHECK(dw_step(&e, builtinOnly, 1, t += 10, P) == DW_WAIT, "missing twice: wait");
    CHECK(dw_step(&e, all, 5, t += 10, P) == DW_OK && e.missing == 0, "back: ok, counter reset");

    // the 2026-11-05 case: the display died with WindowServer -> re-created after `need` checks
    int r = 0;
    for (int i = 0; i < P->need; i++) r = dw_step(&e, builtinOnly, 1, t += 10, P);
    CHECK(r == DW_RECREATE && e.nRecreated == 1, "missing need checks: re-create");
    e.did = 18;  // the new display
    unsigned after[] = {1, 18};
    CHECK(dw_step(&e, after, 2, t += 10, P) == DW_OK, "re-created display online: ok");

    // missing again soon after: no second re-create inside minGap
    for (int i = 0; i < 10; i++) r = dw_step(&e, builtinOnly, 1, t += 10, P);
    CHECK(r == DW_WAIT && e.nRecreated == 1, "within minGap: wait, no churn");
    // after minGap: one more re-create allowed
    t += P->minGap;
    CHECK(until(&e, builtinOnly, 1, &t, P) == DW_RECREATE && e.nRecreated == 2, "after minGap: second re-create");
    // still missing: maxRecreates within the window -> give up, exit for a fresh process
    t += P->minGap;
    CHECK(until(&e, builtinOnly, 1, &t, P) == DW_EXIT, "re-creates do not stick: exit");

    // old re-creates age out of the window
    dw_entry f; memset(&f, 0, sizeof f); f.sid = 2; f.did = 5;
    double u = 10000;
    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < P->need; i++) r = dw_step(&f, builtinOnly, 1, u += 10, P);
        CHECK(r == DW_RECREATE, "a re-create per window is fine");
        f.did = 100 + k; u += P->window + 1;
    }

    // the session: an empty online list (no built-in either) or a failed call, for need checks -> exit
    int empty = 0;
    CHECK(dw_session(5, &empty, P) == DW_OK, "session with displays: ok");
    CHECK(dw_session(0, &empty, P) == DW_WAIT, "empty once: wait");
    CHECK(dw_session(-1, &empty, P) == DW_WAIT, "call failed: wait");
    CHECK(dw_session(0, &empty, P) == DW_EXIT, "empty need times: exit");
    CHECK(dw_session(1, &empty, P) == DW_OK && empty == 0, "back: reset");

    printf("displays_watch: %d failures\n", fails);
    return fails ? 1 : 0;
}
