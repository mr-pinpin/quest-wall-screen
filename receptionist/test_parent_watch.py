#!/usr/bin/env python3
"""Tests for parent_watch.py (Host-plugin orphan guard).

Unit tests for the orphan predicate and the enable/standalone gating, plus a REAL integration test
that spawns a fake-Host process which itself spawns a watched child, kills the fake Host, and asserts
the child exits (reparented -> orphan guard fires).
Run: python3 -m unittest receptionist.test_parent_watch
"""
import os
import subprocess
import sys
import tempfile
import time
import unittest

import parent_watch as pw

HERE = os.path.dirname(os.path.abspath(__file__))


class OrphanPredicateTests(unittest.TestCase):
    def test_same_parent_not_orphaned(self):
        self.assertFalse(pw._orphaned(100, 100))

    def test_reparented_to_init_is_orphaned(self):
        self.assertTrue(pw._orphaned(100, 1))

    def test_reparented_to_other_is_orphaned(self):
        self.assertTrue(pw._orphaned(100, 200))

    def test_started_as_init_never_orphaned(self):
        # ppid was 1 at start (no real Host parent): never report orphaned, even if it "changes".
        self.assertFalse(pw._orphaned(1, 1))
        self.assertFalse(pw._orphaned(1, 5))


class WatchGatingTests(unittest.TestCase):
    def test_disabled_returns_none(self):
        self.assertIsNone(pw.watch_parent(enabled=False))

    def test_env_flag_off_by_default(self):
        os.environ.pop("PINPIN_ORPHAN_EXIT", None)
        self.assertIsNone(pw.watch_parent())   # no flag -> not watching

    def test_standalone_ppid1_not_watched(self):
        # enabled, but started with ppid==1 -> nothing to watch.
        self.assertIsNone(pw.watch_parent(enabled=True, _getppid=lambda: 1))

    def test_enabled_with_real_parent_starts_thread(self):
        calls = []
        t = pw.watch_parent(poll_s=0.01, enabled=True, _getppid=lambda: 4242,
                            _exit=lambda code: None)
        self.addCleanup(lambda: None)
        self.assertIsNotNone(t)
        self.assertTrue(t.is_alive())

    def test_exit_called_when_parent_changes(self):
        # Drive the loop: ppid returns the start value a few times, then 1 -> _exit(0) must fire.
        seq = [500, 500, 1]
        exited = []

        def gp():
            return seq.pop(0) if seq else 1

        def fake_exit(code):
            exited.append(code)
            raise SystemExit  # stop the loop thread cleanly after the first exit

        t = pw.watch_parent(poll_s=0.005, enabled=True, _getppid=gp, _exit=fake_exit)
        deadline = time.time() + 2
        while t.is_alive() and time.time() < deadline:
            time.sleep(0.02)
        self.assertEqual(exited, [0])


# Child that enables the guard and writes a marker file when it exits as an orphan.
_CHILD = r'''
import sys, time
sys.path.insert(0, %(here)r)
import parent_watch as pw
marker = %(marker)r
def _mark(msg):
    with open(marker, "w") as f:
        f.write(msg)
pw.watch_parent(poll_s=0.1, enabled=True, on_orphan=_mark)
while True:
    time.sleep(0.05)
'''

# Fake Host: spawn the watched child, print its pid, then idle until killed.
_FAKEHOST = r'''
import subprocess, sys, time
child = subprocess.Popen([sys.executable, "-c", %(child_src)r])
print(child.pid, flush=True)
time.sleep(60)
'''


class IntegrationKillHostTest(unittest.TestCase):
    def test_child_exits_when_fake_host_killed(self):
        d = tempfile.mkdtemp()
        marker = os.path.join(d, "orphaned.marker")
        child_src = _CHILD % {"here": HERE, "marker": marker}
        fakehost_src = _FAKEHOST % {"child_src": child_src}
        host = subprocess.Popen([sys.executable, "-c", fakehost_src], stdout=subprocess.PIPE, text=True)
        try:
            child_pid = int(host.stdout.readline().strip())
            # child is alive and watching
            os.kill(child_pid, 0)
            time.sleep(0.3)
            # kill the fake Host -> the child is reparented -> guard fires
            host.kill()
            host.wait(timeout=5)
            deadline = time.time() + 6
            while time.time() < deadline and not os.path.exists(marker):
                time.sleep(0.1)
            self.assertTrue(os.path.exists(marker), "orphan guard did not fire after Host was killed")
        finally:
            if host.stdout:
                host.stdout.close()
            if host.poll() is None:
                host.kill()
            try:
                os.kill(child_pid, 9)   # reap if somehow still around
            except (OSError, NameError):
                pass
            import shutil
            shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
