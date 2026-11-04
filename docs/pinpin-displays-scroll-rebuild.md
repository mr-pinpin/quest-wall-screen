# Morning tasks: PinPin Displays rebuild (scroll + click fix) + a static wallpaper

Status: scroll+click binary DEPLOYED 2026-11-04 as a PinPin Host plugin child (Accessibility granted,
one instance on :57710). **Native CLICK VERIFIED working** (2026-11-04): a `c <screen> <u> <v>` over
pinpin-displays UDP :57711 closed a TextEdit window on a scratch display (window count 1→0), so
synthetic clicks register on native apps on the extra displays — the user's original "clicks don't
register on screen 4" bug is fixed. **Scroll also VERIFIED** (2026-11-04): a `w <screen> …` wave on a
long web page moved window.scrollY 0→1200, so the native scroll registers too. Both click and scroll
confirmed on the extra displays. Owner: work-42.

> **SUPERSEDED (2026-11-04):** Section A below assumed pinpin-displays is a standalone ad-hoc app that
> re-prompts for Accessibility on rebuild. It is actually a **PinPin Host plugin**, and the Host is its
> SOLE owner (see AGENTS.md). Running `install-pinpin-displays.sh` in its old form **bootstrapped a
> second launchd owner** of :57710 → three owners fought, crash-looped, and churned the displays,
> spiking the Air to load 246 (incident 2026-11-04; stabilised by work-df to one Host-child instance).
> The correct model: a Host child's grant is charged to the Host's permanent identity, so **rebuilds do
> NOT re-prompt**. Upgrade path = build the binary + let the Host restart the plugin
> (`POST http://127.0.0.1:57790/restart?name=<displays-plugin>`), never `launchctl bootstrap` the
> agent and never `open` the app. The installer has been fixed to do exactly this and to retire the
> stale agent. K19 tracks making the Host plugin start the single, reboot-surviving path.

## A. (historical) Rebuild PinPin Displays — one Accessibility click

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
