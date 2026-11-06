#!/usr/bin/env python3
"""Tests for parent_watch.py (Host-plugin session-orphan guard).

The guard exits only when the LOGIN SESSION is gone (own start < WindowServer start - slack), NOT on a
parent-pid change — because PinPin Host leaves plugins running across its own restarts (kickstart -k) and
the next Host adopts them. These tests pin that behaviour: adoption (same session) must NOT exit; a
WindowServer restart (new session) MUST exit; unknown times must never exit.
Run: python3 -m unittest receptionist.test_parent_watch
"""
import os
import time
import unittest

import parent_watch as pw


class StalePredicateTests(unittest.TestCase):
    def test_started_after_windowserver_not_stale(self):
        # Normal: a plugin starts after WindowServer (login -> WindowServer -> Host -> plugin).
        self.assertFalse(pw._stale(own_start=1000.0, ws_start=900.0))

    def test_started_before_windowserver_is_stale(self):
        # Real orphan: we predate the current WindowServer -> our session is gone.
        self.assertTrue(pw._stale(own_start=1000.0, ws_start=2000.0))

    def test_within_slack_not_stale(self):
        # Close starts (same session, resolution noise) stay within slack -> not stale.
        self.assertFalse(pw._stale(own_start=1000.0, ws_start=1001.0, slack=2.0))

    def test_unknown_times_never_stale(self):
        self.assertFalse(pw._stale(None, 2000.0))
        self.assertFalse(pw._stale(1000.0, None))
        self.assertFalse(pw._stale(None, None))


class WatchGatingTests(unittest.TestCase):
    def test_disabled_returns_none(self):
        self.assertIsNone(pw.watch_parent(enabled=False))

    def test_env_flag_off_by_default(self):
        os.environ.pop("PINPIN_ORPHAN_EXIT", None)
        self.assertIsNone(pw.watch_parent())

    def test_unknown_own_start_not_watched(self):
        # If we can't determine our own age, never watch (fail safe).
        self.assertIsNone(pw.watch_parent(enabled=True, _own_start=lambda: None,
                                          _ws_start=lambda: 2000.0))

    def test_enabled_starts_thread(self):
        t = pw.watch_parent(poll_s=0.01, enabled=True, _own_start=lambda: 1000.0,
                            _ws_start=lambda: 900.0, _exit=lambda c: None)
        self.assertIsNotNone(t)
        self.assertTrue(t.is_alive())


class LoopBehaviourTests(unittest.TestCase):
    def _run_until_done(self, t, timeout=2.0):
        deadline = time.time() + timeout
        while t.is_alive() and time.time() < deadline:
            time.sleep(0.01)

    def test_adoption_same_session_does_not_exit(self):
        # WindowServer steady and older than us (a Host restart / adoption): must NEVER exit.
        exited = []
        t = pw.watch_parent(poll_s=0.01, enabled=True, _own_start=lambda: 1000.0,
                            _ws_start=lambda: 900.0, _exit=lambda c: exited.append(c))
        time.sleep(0.2)   # let it poll many times
        self.assertEqual(exited, [])
        self.assertTrue(t.is_alive())

    def test_windowserver_restart_triggers_exit(self):
        # WindowServer restarts to a time AFTER our start -> session gone -> exit(0).
        ws_seq = [900.0, 900.0, 5000.0]   # steady, steady, then a fresh (newer) WindowServer
        exited = []

        def ws():
            return ws_seq.pop(0) if ws_seq else 5000.0

        def fake_exit(code):
            exited.append(code)
            raise SystemExit   # stop the daemon loop after the first exit

        t = pw.watch_parent(poll_s=0.005, enabled=True, _own_start=lambda: 1000.0,
                            _ws_start=ws, _exit=fake_exit)
        self._run_until_done(t)
        self.assertEqual(exited, [0])

    def test_unknown_windowserver_never_exits(self):
        # If WindowServer start can't be read, never exit (fail safe), even forever.
        exited = []
        t = pw.watch_parent(poll_s=0.01, enabled=True, _own_start=lambda: 1000.0,
                            _ws_start=lambda: None, _exit=lambda c: exited.append(c))
        time.sleep(0.2)
        self.assertEqual(exited, [])

    def test_on_orphan_hook_runs_before_exit(self):
        msgs = []

        def fake_exit(code):
            raise SystemExit

        t = pw.watch_parent(poll_s=0.005, enabled=True, _own_start=lambda: 1000.0,
                            _ws_start=lambda: 5000.0, on_orphan=msgs.append, _exit=fake_exit)
        self._run_until_done(t)
        self.assertTrue(msgs and "exiting" in msgs[0])


class RealProcessStartTests(unittest.TestCase):
    """_proc_start_epoch / _windowserver_start_epoch against real processes on this host (ps-based)."""

    def test_own_start_is_readable_and_past(self):
        s = pw._proc_start_epoch(os.getpid())
        self.assertIsNotNone(s, "ps lstart should give our own start time")
        self.assertLessEqual(s, time.time() + 2)

    def test_bogus_pid_is_none(self):
        self.assertIsNone(pw._proc_start_epoch(2 ** 31 - 1))


if __name__ == "__main__":
    unittest.main(verbosity=2)
