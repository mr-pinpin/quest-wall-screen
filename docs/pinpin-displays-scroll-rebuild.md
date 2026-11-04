# Morning task: rebuild PinPin Displays (scroll + still-wallpaper) — one Accessibility click

Status: staged 2026-11-04, waiting on the user. Owner: work-42 (scroll), plus the still-wallpaper change already on main.

## Why this needs the user

The Air's running **PinPin Displays** is behind `main`. Rebuilding it picks up three committed changes:
- the **scroll** `"w <id> <u> <v> <dy> <dx>"` UDP handler (headset wave-to-scroll + the assistant's computer-use
  scroll share it) — `mac/pinpin-displays.m`;
- a **click fix**: synthetic `"c"` clicks now use a real event source, a click state, a hover-warp first, and a
  60 ms hold, so clicks register on native apps on the extra displays (the old click was silently ignored by some
  apps). Web screens (Chrome `--app`) use CDP input instead and don't depend on this;
- the **still-wallpaper** change (flag-gated; no effect unless `~/.local/share/pinpin-displays/still-wallpaper`
  exists).

Rebuilding changes the app's ad-hoc signature, so macOS asks for **Accessibility** again (PinPin Displays needs it
to post mouse/scroll events). That is a click only the user can make — hence this is a morning task, batched so it
is one click. Everything else is pre-staged on the Air.

## Steps (on the Air)

1. Deploy the staged build (two commands):
   ```sh
   ssh pinpin-air
   cp ~/.local/share/pinpin-displays/src/pinpin-displays.m.staged-scroll ~/.local/share/pinpin-displays/src/pinpin-displays.m
   sh ~/.local/share/pinpin-displays/install-pinpin-displays.sh
   ```
   The installer rebuilds `~/Applications/PinPin Displays.app`, reloads the login agent, and prints the display
   list at the end.

2. Grant Accessibility (the one click):
   - If a dialog appears — *"'PinPin Displays' would like to control this computer using accessibility
     features"* — click **Open System Settings**.
   - Go to **System Settings → Privacy & Security → Accessibility**.
   - Find **PinPin Displays** in the list and turn its toggle **ON**. If it is not listed, click **＋**, choose
     `~/Applications/PinPin Displays.app`, then turn it **ON**. (Authenticate with Touch ID / password if asked.)
   - Screen Recording is **not** needed here (that belongs to PinPin Host); only Accessibility.

3. Verify:
   - `curl -s http://127.0.0.1:57710/displays` returns the JSON display list (not empty, not refused).
   - **Click (native app):** create a scratch display, open TextEdit on it, and send a click on a menu/button —
     e.g. `curl -s -X POST 'http://127.0.0.1:57710/displays?id=61&w=1200&h=800&scale=1'`, open TextEdit and drag its
     window there, then `echo -n "c 61 5000 5000" | nc -u -w1 127.0.0.1 57711` should place the caret / activate the
     control under that point. Remove it after: `curl -s -X POST 'http://127.0.0.1:57710/remove?id=61'`.
   - **Scroll:** in the headset with the **SCROLL** flag on, the open-hand wave scrolls the screen you point at. (Or
     `echo -n "w 2 5000 5000 120 0" | nc -u -w1 127.0.0.1 57711` scrolls display id 2.)
   - Web screens (Chrome `--app`) get clicks/scroll/type via CDP (work-ad's runner), independent of this rebuild.

## Notes

- Low risk: the scroll handler only adds a new `"w"` message; existing `"m"`/`"c"` are unchanged. The
  still-wallpaper code does nothing unless its flag file is present.
- If the display list is empty after the rebuild, Accessibility isn't granted yet — redo step 2, then
  `launchctl kickstart -k gui/$(id -u)/io.mr-pinpin.displays`.
- Rollback: the previous source is in git; rebuild from an earlier `mac/pinpin-displays.m` if needed.
