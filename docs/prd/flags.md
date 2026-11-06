# PRD: flag registry and benchmark-decided defaults

**Problem.** The settings list has grown to about 29 switches; the user can't see a difference from most of them.

## Requirements
- [ ] `room_lab/flags.json`: for each flag, `default`, `state` (on/off/hidden), `category` (perf/visual-taste/experimental),
      `reason` (plain sentence), `evidence` (commit hashes, bench result paths), like Apple Maps' feed config where a
      disabled feed carries an explanation
- [ ] The menu shows the reason as the hint; hidden flags are greyed out or omitted; the code stays
- [ ] Benchmark campaign (stand-in + Air, never the user's worn headset): 3x alternating A/B against the v0.37 defaults, medians,
      verdict.py. No measurable gain → off/hidden with that reason
- [ ] Visual-taste flags are marked "user's choice"

## Backend / Host-plugin flags (env, not headset settings)

These gate behaviour in the Mac-side services, set in each plugin's JSON `env`, not in `room_lab/flags.json`.

- **`PINPIN_ORPHAN_EXIT`** (focus_controller, pinpin_assistant) — `1` enables the orphan guard
  (`parent_watch.watch_parent()`): the plugin child exits when its **login session** is gone — i.e. when it
  started before the currently running WindowServer (minus 2 s slack) — instead of surviving as a stray from
  a dead session. It deliberately does NOT key on the parent pid: PinPin Host leaves plugins running across
  its own restarts (`kickstart -k`) and the next Host adopts them via pid files, so ppid=1 is normal during a
  Host rebuild. _Reason:_ the 2026-11-05 WindowServer crash killed the Host but left 2 children running
  (pinpin-displays 558, focus_controller 6503); on relogin the Host respawned fresh copies, so each was a
  duplicate. _Evidence:_ commits `ffd4fd7` (guard + tests), `86f5a1f` (telemetry log line), session-rule
  correction (this commit). _State:_ on for focus_controller and pinpin_assistant 2026-11-06; pinpin-displays
  has its own equivalent (K30 session-watch, dw_stale); Host startup sweep is K31b (same rule).
