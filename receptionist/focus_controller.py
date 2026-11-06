#!/usr/bin/env python3
"""focus_controller.py — pause/down-quality video on web screens the user isn't looking at (K15).

Policy (work-df): a web screen the user isn't looking at gets its video down-qualitied ("low",
cuts VTDecoder — a true fps cap isn't reachable from the page), or PAUSED after 60 s unseen if
muted; it resumes (play + full quality) instantly when looked at. Flag-gated, off by default.

Lever (work-ad): POST {APPS}/api/screens/<id>/video {state: probe|pause|play|low|full}, Bearer
apps token. probe is the detector. pause marks what it paused; play resumes only those (a
user-paused video stays paused). Idempotent.

Visibility source is pluggable:
  - PINPIN_VIS_URL -> GET returns {"<screen id>": <vis 0..1 or bool>}  (atlas :57721 feedback, work-c4)
  - else house.selected (coarse: only selected screens count as "seen")
  - else None -> treat everything as seen (safe no-op; never pauses)
"""
import json, os, sys, time, urllib.request, urllib.error

APPS = os.environ.get("PINPIN_APPS_BASE", "http://100.64.243.30:57830")   # Mini, Tailscale
HOUSE = os.environ.get("PINPIN_HOUSE_BASE", "http://100.64.243.30:57800")
VIS_FILE = os.environ.get("PINPIN_VIS_FILE", "/tmp/pinpin-atlas-vis.json")  # atlas vis tap (work-c4)
VIS_STALE_S = float(os.environ.get("PINPIN_VIS_STALE_S", "2"))            # older -> unknown, assume visible
VIS_URL = os.environ.get("PINPIN_VIS_URL")                                # alternative: atlas vis over HTTP
ENABLED = os.environ.get("PINPIN_FOCUS_ENABLED", "") == "1"              # must opt in to act
DRY = os.environ.get("PINPIN_FOCUS_DRY", "") == "1"                       # log decisions, never POST act
POLL_S = float(os.environ.get("PINPIN_FOCUS_POLL_S", "1.5"))             # vis poll (cheap) -> near-instant restore
PROBE_S = float(os.environ.get("PINPIN_FOCUS_PROBE_S", "10"))            # how often to probe a screen for dropping
UNSEEN_PAUSE_S = float(os.environ.get("PINPIN_FOCUS_PAUSE_S", "60"))
HYST_UNSEEN_S = float(os.environ.get("PINPIN_FOCUS_HYST_S", "5"))         # drop quality only after >=5s unseen
STATUS_URL = os.environ.get("PINPIN_STATUS_URL", APPS + "/api/status/component")
ONLY = [s.strip() for s in os.environ.get("PINPIN_FOCUS_SCREENS", "").split(",") if s.strip()]


def _token():
    p = os.path.expanduser("~/.local/share/pinpin-apps/token")
    try:
        with open(p) as f:
            return f.read().strip()
    except OSError:
        import subprocess
        r = subprocess.run(["sudo", "-n", "cat", "/root/.config/pinpin-apps/token"],
                           capture_output=True, timeout=5)
        return r.stdout.decode().strip() if r.returncode == 0 else ""


def _get(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.load(r)


def video(sid, state):
    body = json.dumps({"state": state}).encode()
    req = urllib.request.Request("%s/api/screens/%s/video" % (APPS, sid), data=body, method="POST",
                                 headers={"Authorization": "Bearer " + _token(),
                                          "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.load(r)


_last_status = [None]   # last posted count, so we only POST on change


def post_status(n_lowered):
    if _last_status[0] == n_lowered:
        return
    _last_status[0] = n_lowered
    body = json.dumps({"component": "video-focus", "ok": True,
                       "text": "active, %d screen%s lowered" % (n_lowered, "" if n_lowered == 1 else "s")}).encode()
    try:
        req = urllib.request.Request(STATUS_URL, data=body, method="POST",
                                     headers={"Authorization": "Bearer " + _token(),
                                              "Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=6).read()
    except Exception as e:
        print("status post failed:", e, flush=True)


def web_screens():
    if ONLY:
        return ONLY
    ids = set()
    try:                                               # house web screens
        for s in _get(HOUSE + "/house").get("screens", []):
            if (s.get("source") or {}).get("kind", "") in ("atlas", "web", "app", "chrome"):
                ids.add(str(s.get("id")))
    except Exception:
        pass
    try:                                               # plus whatever the atlas vis tap currently reports
        if VIS_FILE and os.path.exists(VIS_FILE):      # (authoritative active set; house may be incomplete)
            with open(VIS_FILE) as f:
                ids |= set(json.load(f).get("screens", {}).keys())
    except Exception:
        pass
    return sorted(ids, key=lambda x: (len(x), x))      # non-web ones 404 at probe time and are skipped


def get_vis():
    """Return {screen_id: seen_bool}, or None if no fresh visibility source (-> treat all as seen)."""
    # 1) atlas vis tap file (work-c4): {"t": unix, "screens": {"1": {"vis","pri","px"}, ...}}
    if VIS_FILE and os.path.exists(VIS_FILE):
        try:
            with open(VIS_FILE) as f:
                d = json.load(f)
            if time.time() - float(d.get("t", 0)) > VIS_STALE_S:
                return None                      # stale -> unknown, assume visible (work-c4's rule)
            return {str(k): float(v.get("vis", 0)) > 0.05 for k, v in d.get("screens", {}).items()}
        except Exception:
            return None
    # 2) explicit HTTP vis endpoint (alternative)
    if VIS_URL:
        try:
            raw = _get(VIS_URL)
            return {str(k): (float(v) > 0.05 if isinstance(v, (int, float)) else bool(v))
                    for k, v in raw.items()}
        except Exception:
            return None
    # 3) coarse fallback: only house.selected screens count as seen
    try:
        doc = _get(HOUSE + "/house")
        sel = doc.get("selected")
        ids = [str(e.get("id")) for e in sel] if isinstance(sel, list) else (
            [str(sel.get("id"))] if isinstance(sel, dict) else [])
        if ids:
            return {i: True for i in ids}   # only these are "seen"; others unseen
    except Exception:
        pass
    return None


def decide_unseen(unseen_for, probe):
    """Action for an UNSEEN screen. Hysteresis: do nothing until unseen >=HYST_UNSEEN_S (anti-flap).
    Then pause if muted & unseen >=60 s, else low quality. Never pauses audible video."""
    if unseen_for < HYST_UNSEEN_S:
        return None                                   # too briefly unseen -> hold (no flapping)
    playing = [v for v in (probe.get("videos") or []) if v.get("playing") and not v.get("paused")]
    if not playing:
        return None
    if unseen_for >= UNSEEN_PAUSE_S and all(v.get("muted") for v in playing):
        return ("pause",)                             # muted + long unseen -> pause
    return ("low",)                                   # audible, or not yet 60 s -> low quality (never pause audible)


def _apply(sid, want, note):
    print("%s screen %s -> %s (%s)" % (time.strftime("%H:%M:%S"), sid, "/".join(want), note), flush=True)
    if ENABLED and not DRY:
        for s in want:
            try:
                video(sid, s)
            except Exception as e:
                print("  act %s failed: %s" % (s, e), flush=True)


def tick(state):
    vis = get_vis()
    now = time.time()
    for sid in web_screens():
        st = state.setdefault(sid, {"last_seen": now, "acted": None, "last_probe": 0.0})
        seen = True if vis is None else vis.get(sid, False)
        if seen:
            st["last_seen"] = now
            if st["acted"] in (("low",), ("pause",)):      # restore INSTANTLY on first visible frame
                _apply(sid, ("play", "full"), "seen -> restore")
                st["acted"] = None
            continue
        unseen_for = now - st["last_seen"]
        if unseen_for < HYST_UNSEEN_S:
            continue                                        # anti-flap: hold until >=5s unseen
        if st["acted"] in (("low",), ("pause",)) and now - st["last_probe"] < PROBE_S:
            continue                                        # already dropped; don't re-probe every poll
        try:
            probe = video(sid, "probe")
        except urllib.error.HTTPError as e:
            if e.code == 404:
                continue                                    # no runner window on this screen
            continue
        except Exception:
            continue
        st["last_probe"] = now
        want = decide_unseen(unseen_for, probe) or ()
        if want and st["acted"] != want:
            _apply(sid, want, "unseen %ds, videos=%d" % (int(unseen_for), len(probe.get("videos") or [])))
            st["acted"] = want
    # /status line so the user can see it (only when actually active)
    if ENABLED and not DRY:
        post_status(sum(1 for s in state.values() if s.get("acted") in (("low",), ("pause",))))


def main():
    mode = "ACTIVE" if (ENABLED and not DRY) else ("DRY" if DRY else "DISABLED (observe only)")
    src = (VIS_FILE if os.path.exists(VIS_FILE) else None) or VIS_URL or (
        "house.selected" if get_vis() is not None else "none(all-seen)")
    print("focus_controller: mode=%s vis=%s poll=%gs pause_after=%gs" % (mode, src, POLL_S, UNSEEN_PAUSE_S),
          flush=True)
    state = {}
    while True:
        try:
            tick(state)
        except Exception as e:
            print("tick error:", e, flush=True)
        time.sleep(POLL_S)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "once":
        tick({})
    else:
        main()
