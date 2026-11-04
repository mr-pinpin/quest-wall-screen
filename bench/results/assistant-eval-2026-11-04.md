# Assistant eval — 2026-11-04

18 realistic spoken requests (typos/mishears included) on display 60 + the scratch house (:57801), each with an
automatic check: reply content, live page URL via Chrome DevTools, or a scratch-house diff. Harness:
`bench/assistant_eval.py`; run via `bench/eval_run.sh` (points the service's house at scratch, reverts after).

## Scores

| run | pass | first-word p50/p90 | complete p50/p90 |
|-----|------|--------------------|------------------|
| baseline            | 16/18 (88%) | 20.4 / 41.0 s | 21.1 / 41.8 s |
| after fixes         | 17/18 (94%) | 12.4 / 54.0 s | 13.0 / 54.7 s |
| **final (all fixes)** | **17/18 (94%)** | **10.7 / 27.1 s** | **11.3 / 27.8 s** |

Behaviourally the final run is effectively 18/18 — see room-move below.

## Fixes applied (approved by work-df)

1. **DOM/CDP clicks** — added an `eval(js)` tool over work-ad's CDP runner. web-click ("open X then click the
   link") now navigates reliably to iana.org, and faster (12–24 s vs 42 s of screenshot-guessing).
2. **YouTube playback** — `play_youtube(query)` opens the search, evals the first `/watch` href, opens it and
   nudges `video.play()`. All three YouTube cases (incl. the mishear "yakarium blue") now reach a real
   `/watch?v=…` page (were search-only). Check tightened from `youtube.com` to `/watch`.
3. **Relative moves** — move_screen gained `direction:"left"/"right"/"up"/"down"` (+ `amount_m`, default 0.3) and
   `dx_m/dy_m`; the room prompt tells the model to use `direction` for nudges, not an absolute position.
4. **Latency** — verify screenshots capped at ~900 px (was 1600): 4.5× smaller PNGs. first-word p50 20.4 → 10.7 s;
   web-open 76 → 22 s.
5. **Clamp bug** — a screen wider than its (re-measured, now smaller) wall made the x-clamp pin x to +lim; it now
   centres such a screen instead.
6. **Apps token** — `Authorization: Bearer` on every powerful apps call (open/click/eval/put_app/assign/build),
   read from a local 600 file; `/api/auth` confirmed 0 unauthenticated calls before enforcement.

## The one "failure": room-move (actually correct behaviour)

`move screen 3 a bit to the left` reads as FAIL only because the automatic check assumes there's room to move
left. The house walls have been re-measured much smaller (e.g. a wall went 2.97 → 0.90 m), so screen 3 already
sits at its wall's edge: the tool correctly **clamps to the wall limit** and the model says so ("already at the
leftmost its wall allows"). Tool + model are right; the check isn't geometry-aware. Fix for the harness later:
pick a screen/wall with headroom, or accept a clamp-at-edge as pass.

## Correct + fast already

Chat (incl. typos/arithmetic, 1–3 s), app building, house add/bigger/list-walls, and the **safety confirmation**
(close asks first and the screen stays on until an explicit yes).
