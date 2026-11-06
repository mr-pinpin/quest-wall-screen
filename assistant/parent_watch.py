#!/usr/bin/env python3
"""Orphan guard for PinPin Host plugin children (work-df card, 2026-11-06; rule corrected by work-09).

A Host plugin child should exit when its LOGIN SESSION is gone, so a GUI/WindowServer crash doesn't leave
stray children that keep running and then DUPLICATE the fresh children the next login's Host spawns (seen
2026-11-06: two pinpin-displays, two focus_controller after the 11-05 WindowServer crash).

IMPORTANT — do NOT key on the parent pid. PinPin Host deliberately leaves plugins running across its own
restarts: build-host.sh uses `launchctl kickstart -k` (SIGTERM stops only the Host), and the next Host
ADOPTS the running copies via their pid files. In between every plugin has ppid 1 BY DESIGN. Exiting on
"ppid changed / ==1" would kill-and-restart every plugin on each Host rebuild and break adoption.

The rule that separates a real orphan from an adoptable plugin is the login session: WindowServer restarts
only with a new session, so a process that started BEFORE the currently running WindowServer belongs to a
session that is gone. We exit only when our own start precedes WindowServer's start (minus a slack). Unknown
times are never stale (fail safe: keep running). This agrees with pinpin-displays' dw_stale() (K30) and the
Host startup sweep (K31b).

Flag-gated (ships off): enabled only when PINPIN_ORPHAN_EXIT=1 (or watch_parent(enabled=True)).
"""
import os
import subprocess
import sys
import threading
import time

SLACK_S = 2.0        # tolerance when comparing start times (ps lstart has ~1 s resolution)


def _proc_start_epoch(pid):
    """Epoch seconds when `pid` started, via ps lstart (works for root-owned procs, unlike proc_pidinfo
    from a user process — work-09). None on any failure/unknown."""
    try:
        out = subprocess.run(["ps", "-o", "lstart=", "-p", str(int(pid))],
                             capture_output=True, text=True, timeout=5).stdout.strip()
        if not out:
            return None
        # e.g. "Thu Nov  6 18:58:11 2026" — strptime collapses the double space before a single-digit day.
        return time.mktime(time.strptime(out, "%a %b %d %H:%M:%S %Y"))
    except Exception:
        return None


def _windowserver_start_epoch():
    """Start epoch of the current WindowServer (the login session's anchor), or None."""
    try:
        out = subprocess.run(["pgrep", "-x", "WindowServer"], capture_output=True, text=True, timeout=5).stdout.split()
        if not out:
            return None
        return _proc_start_epoch(int(out[0]))
    except Exception:
        return None


def _stale(own_start, ws_start, slack=SLACK_S):
    """True if our session is gone: we started before the current WindowServer (minus slack). Unknown
    times (None) are never stale — we must never kill an adoptable plugin on uncertainty."""
    if own_start is None or ws_start is None:
        return False
    return own_start < ws_start - slack


def watch_parent(poll_s=10.0, enabled=None, slack_s=SLACK_S, on_orphan=None,
                 _own_start=None, _ws_start=_windowserver_start_epoch, _exit=os._exit):
    """Start the session-orphan watcher. Returns the (started, daemon) Thread, or None if not watching.

    enabled: None -> read PINPIN_ORPHAN_EXIT env. on_orphan(msg): optional hook run just before exit.
    _own_start (epoch or callable) and _ws_start (callable) are injectable for tests.
    """
    if enabled is None:
        enabled = os.environ.get("PINPIN_ORPHAN_EXIT", "") == "1"
    if not enabled:
        return None
    if _own_start is None:                 # not injected -> read our real start
        own = _proc_start_epoch(os.getpid())
    else:                                  # injected (tests): honour it, even if it reports None
        own = _own_start() if callable(_own_start) else _own_start
    if own is None:
        return None                        # can't know our own age -> never watch (fail safe)

    def _loop():
        while True:
            time.sleep(poll_s)
            ws = _ws_start()
            if _stale(own, ws, slack_s):
                # distinctive, greppable line so a Host sweep / telemetry can tell a clean self-exit from
                # a crash (symmetric with pinpin-displays' "session watch: ... exiting", per work-09):
                msg = "parent_watch: login session gone (own start %.0f < WindowServer start %.0f), exiting" % (
                    own, ws)
                if on_orphan is not None:
                    try:
                        on_orphan(msg)
                    except Exception:
                        pass
                else:
                    try:
                        sys.stderr.write(msg + "\n")
                        sys.stderr.flush()
                    except Exception:
                        pass
                _exit(0)

    t = threading.Thread(target=_loop, name="parent-watch", daemon=True)
    t.start()
    return t
