# Morning tasks: PinPin Displays rebuild (scroll + click fix) + a static wallpaper

Status: staged 2026-11-04, waiting on the user. Owner: work-42.

## A. Rebuild PinPin Displays — one Accessibility click

The Air's running **PinPin Displays** is behind `main`. Rebuilding it picks up two committed changes:
- the **scroll** `"w <id> <u> <v> <dy> <dx>"` UDP handler (headset wave-to-scroll + the assistant's computer-use
  scroll share it) — `mac/pinpin-displays.m`;
- a **click fix**: synthetic `"c"` clicks now use a real event source, a click state, a hover-warp first, and a
  60 ms hold, so clicks register on native apps on the extra displays (the old click was silently ignored by some
  apps). Web screens (Chrome `--app`) use CDP input instead and don't depend on this.

(The old **still-wallpaper** idea is SUPERSEDED — see B. It used NSWorkspace.setDesktopImageURL, which does NOT
stop macOS's aerial video wallpaper, so rebuilding for it is pointless.)

Rebuilding changes the app's ad-hoc signature, so macOS asks for **Accessibility** again (PinPin Displays needs it
to post mouse/scroll events). That is a click only the user can make — hence this is a morning task.

### Steps (on the Air)
1. Deploy the staged build:
   ```sh
   ssh pinpin-air
   cp ~/.local/share/pinpin-displays/src/pinpin-displays.m.staged-scroll ~/.local/share/pinpin-displays/src/pinpin-displays.m
   sh ~/.local/share/pinpin-displays/install-pinpin-displays.sh
   ```
2. Grant Accessibility: if the dialog appears, **Open System Settings → Privacy & Security → Accessibility**, find
   **PinPin Displays** and turn it **ON** (add `~/Applications/PinPin Displays.app` with **+** if missing). Screen
   Recording is not needed here.
3. Verify: `curl -s http://127.0.0.1:57710/displays` returns the list. Native-app click test: open TextEdit on a
   scratch display and `echo -n "c 61 5000 5000" | nc -u -w1 127.0.0.1 57711` should click there. Scroll: the
   headset wave (SCROLL flag) scrolls the pointed screen.

## B. Set a STATIC wallpaper (fixes the pegged colorsync / load)

The virtual displays run macOS's **aerial (video) wallpaper**; `WallpaperVideoExtension` + `colorsync` then burn
~100% CPU color-managing every animated frame, keeping the Air's load around 4 and slowing benchmarks. This can't
be turned off from the CLI (confirmed 2026-11-04: NSWorkspace and editing `com.apple.wallpaper/Store/Index.plist`
both fail; the aerial persists). The one-step fix is in the UI:
- **System Settings → Wallpaper → pick any STILL picture** (not the Sequoia "Dynamic"/aerial one). That turns off
  `WallpaperVideoExtension` system-wide and colorsync settles to ~0. Leave Screen Saver / Lock Screen as they are.

No rebuild is needed for B.
