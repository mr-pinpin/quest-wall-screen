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
