#!/usr/bin/env python3
"""Orphan guard for PinPin Host plugin children (work-df card, 2026-11-06).

A Host plugin child should exit when its Host parent dies, so a GUI/Host crash doesn't leave orphans
(reparented to launchd, ppid=1) that keep running and then DUPLICATE the fresh children the Host
respawns on relogin (seen 2026-11-06: two pinpin-displays, two focus_controller). Call watch_parent()
once at startup: it records the parent pid, then a daemon thread polls getppid() and exits the process
the moment the original parent is gone.

Flag-gated (ships off): enabled only when PINPIN_ORPHAN_EXIT=1 (or watch_parent(enabled=True)).
A process started standalone (ppid already 1 — no Host) is never watched.
"""
import os
import sys
import threading
import time


def _orphaned(start_ppid, ppid):
    """True if the original parent is gone. If we started with ppid==1 (no real parent to lose),
    never report orphaned — there is no Host to exit for."""
    return start_ppid != 1 and ppid != start_ppid


def watch_parent(poll_s=3.0, enabled=None, on_orphan=None,
                 _getppid=os.getppid, _exit=os._exit):
    """Start the orphan watcher. Returns the Thread (started, daemon) or None if not watching.

    poll_s: how often to check. enabled: None -> read PINPIN_ORPHAN_EXIT env. on_orphan(msg): optional
    hook run just before exit (e.g. write a marker / log). _getppid/_exit are injectable for tests.
    """
    if enabled is None:
        enabled = os.environ.get("PINPIN_ORPHAN_EXIT", "") == "1"
    if not enabled:
        return None
    start_ppid = _getppid()
    if start_ppid == 1:
        return None                        # no Host parent to watch (started standalone)

    def _loop():
        while True:
            time.sleep(poll_s)
            ppid = _getppid()
            if _orphaned(start_ppid, ppid):
                # distinctive, greppable line so a Host sweep / telemetry can tell a self-exit from a
                # crash (symmetric with pinpin-displays' "session watch: Host parent <pid> ..."):
                msg = "parent_watch: Host parent %d gone, exiting (ppid now %d)" % (start_ppid, ppid)
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
