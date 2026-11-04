# Captain's technical diary (work-df)

Times are UTC. Hourly entries are short; a daily entry is written at about 20:57.

## 2026-11-04, the night shift, 01:30–09:55

- **What went right:**
  - Four agents ran in parallel all night: c4 (headset), 09→34 (geometry, benchmarks), 42 (assistant), and ad (apps, new tonight).
  - The headset went from v0.45 to v0.64.
  - Shipped: the Claude chat per screen, now warm at ~1.4 s to first word; computer use through CDP; apps with a sandbox; house.json plus consolidation; the mesh scan; staying awake on the charger (219/221 min).
- **What went wrong, and the lessons:**
  - Two agents installed at once, because the locks didn't match. Now one lock, and an owner guard in code.
  - The "night prox ban" I imposed left the agents waiting. The user said wake the headset, don't wait, so don't over-correct with blanket bans.
  - The user's screens "moved to another room" twice. The first time was Meta relocalising in the dark (lights fixed it). The second was a scene query racing relocalisation. Make placement robust in code (re-query, cache), not by telling the user what to do.
  - An agent scan shrank real walls, because it overwrote scene extents. Measurements must never overwrite the source of truth: keep a separate "fit" field.
  - The scan commit frame drifts (the floor at +6/+16/+26 cm). Found by an automatic consolidation score, not by eye. Automated quality verdicts pay off.
  - A forced debug flag (GEOMETRY) was left on for the user. Every force file must lose to the user's own toggle.
  - The laptop's animated wallpaper plus colorsync ate ~1.3 cores and quietly skewed the benchmarks. Record the environment's load per run.
- **On coordination:**
  - The user wants a captain who delegates rather than codes, who is never idle, and who is honest about failures.
  - They are their own single entry point to me, and I delegate non-engineering topics to spawned sessions.
- **Next:**
  - the receptionist, so the user's headset voice reaches me (by 10:50);
  - a proper kanban;
  - the user's kanban: dictation always on, pointer smoothing, controls always visible, the select outline.
