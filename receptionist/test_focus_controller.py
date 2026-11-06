#!/usr/bin/env python3
"""Unit tests for focus_controller.py (K15 video focus) — pure logic, no network.

Covers: decide_unseen policy, the tick() state machine (restore-on-seen, hysteresis,
pause-vs-low, probe throttle, DRY vs ACTIVE), get_vis source selection, web_screens union.
Run: python3 -m unittest receptionist.test_focus_controller  (or: python3 receptionist/test_focus_controller.py)
"""
import json
import os
import tempfile
import time
import unittest
from unittest import mock

import focus_controller as fc


def probe(*videos):
    """Build a probe reply. Each video is a dict like {'playing':True,'paused':False,'muted':True}."""
    return {"videos": list(videos)}


def vid(playing=True, paused=False, muted=True):
    return {"playing": playing, "paused": paused, "muted": muted}


class DecideUnseenTests(unittest.TestCase):
    """decide_unseen(unseen_for, probe) — the core policy, no state/time."""

    def test_below_hysteresis_holds(self):
        # Unseen for less than HYST_UNSEEN_S: never act (anti-flap), even with a playing muted video.
        self.assertIsNone(fc.decide_unseen(fc.HYST_UNSEEN_S - 0.1, probe(vid())))

    def test_no_playing_videos_is_noop(self):
        self.assertIsNone(fc.decide_unseen(fc.UNSEEN_PAUSE_S + 10, probe()))
        # A paused video is not "playing" -> nothing to do.
        self.assertIsNone(fc.decide_unseen(fc.UNSEEN_PAUSE_S + 10, probe(vid(playing=True, paused=True))))
        # playing:False is also excluded.
        self.assertIsNone(fc.decide_unseen(fc.UNSEEN_PAUSE_S + 10, probe(vid(playing=False))))

    def test_muted_and_long_unseen_pauses(self):
        self.assertEqual(fc.decide_unseen(fc.UNSEEN_PAUSE_S, probe(vid(muted=True))), ("pause",))
        self.assertEqual(fc.decide_unseen(fc.UNSEEN_PAUSE_S + 100, probe(vid(muted=True), vid(muted=True))),
                         ("pause",))

    def test_audible_never_pauses(self):
        # Audible video, even after a long time unseen -> low quality, never pause.
        self.assertEqual(fc.decide_unseen(fc.UNSEEN_PAUSE_S + 1000, probe(vid(muted=False))), ("low",))

    def test_mixed_muted_and_audible_is_low(self):
        # If ANY playing video is audible, we must not pause (all() muted is False) -> low.
        self.assertEqual(
            fc.decide_unseen(fc.UNSEEN_PAUSE_S + 5, probe(vid(muted=True), vid(muted=False))), ("low",))

    def test_muted_but_not_yet_pause_threshold_is_low(self):
        # Past hysteresis but before the 60s pause threshold -> low, not pause.
        mid = (fc.HYST_UNSEEN_S + fc.UNSEEN_PAUSE_S) / 2.0
        self.assertEqual(fc.decide_unseen(mid, probe(vid(muted=True))), ("low",))


class FakeClock:
    def __init__(self, t=1000.0):
        self.t = t

    def __call__(self):
        return self.t


class TickTests(unittest.TestCase):
    """tick(state) state machine with video()/get_vis()/web_screens() faked (no network)."""

    def setUp(self):
        self.clock = FakeClock(1000.0)
        self._patchers = [mock.patch.object(fc.time, "time", self.clock)]
        for p in self._patchers:
            p.start()
        # record every video() call as (sid, state); probe returns a per-sid configured reply
        self.calls = []
        self.probe_reply = {}  # sid -> probe dict (default: one playing muted video)

        def fake_video(sid, state):
            self.calls.append((str(sid), state))
            if state == "probe":
                return self.probe_reply.get(str(sid), probe(vid(muted=True)))
            return {"ok": True, "state": state}

        self.vis = {}            # sid -> seen bool (set per test); None-sentinel handled separately
        self.vis_is_none = False
        self.screens = ["2"]

        fc.video = fake_video
        fc.get_vis = lambda: (None if self.vis_is_none else dict(self.vis))
        fc.web_screens = lambda: list(self.screens)
        fc.post_status = lambda n: self.calls.append(("STATUS", n))
        self._saved = (fc.ENABLED, fc.DRY)
        fc.ENABLED, fc.DRY = True, False   # ACTIVE by default; individual tests flip

    def tearDown(self):
        for p in self._patchers:
            p.stop()
        fc.ENABLED, fc.DRY = self._saved
        # restore the real module functions so other test classes aren't affected
        import importlib
        importlib.reload(fc)

    def acts(self):
        """video() calls that actually change state (exclude probes and STATUS)."""
        return [c for c in self.calls if c[1] in ("low", "pause", "play", "full")]

    def test_vis_none_treats_all_seen_never_acts(self):
        self.vis_is_none = True
        st = {}
        fc.tick(st)
        self.assertEqual(self.acts(), [])

    def test_seen_updates_last_seen_no_action(self):
        self.vis = {"2": True}
        st = {}
        fc.tick(st)
        self.assertEqual(st["2"]["last_seen"], 1000.0)
        self.assertIsNone(st["2"]["acted"])
        self.assertEqual(self.acts(), [])

    def test_unseen_below_hysteresis_holds(self):
        self.vis = {"2": False}
        st = {"2": {"last_seen": 1000.0 - (fc.HYST_UNSEEN_S - 1), "acted": None, "last_probe": 0.0}}
        fc.tick(st)
        self.assertEqual(self.acts(), [])     # not unseen long enough yet
        self.assertEqual([c for c in self.calls if c[1] == "probe"], [])  # not even probed

    def test_unseen_long_muted_pauses(self):
        self.vis = {"2": False}
        self.probe_reply["2"] = probe(vid(muted=True))
        st = {"2": {"last_seen": 1000.0 - (fc.UNSEEN_PAUSE_S + 5), "acted": None, "last_probe": 0.0}}
        fc.tick(st)
        self.assertIn(("2", "pause"), self.calls)
        self.assertEqual(st["2"]["acted"], ("pause",))

    def test_unseen_audible_lowers(self):
        self.vis = {"2": False}
        self.probe_reply["2"] = probe(vid(muted=False))
        st = {"2": {"last_seen": 1000.0 - (fc.UNSEEN_PAUSE_S + 5), "acted": None, "last_probe": 0.0}}
        fc.tick(st)
        self.assertIn(("2", "low"), self.calls)
        self.assertNotIn(("2", "pause"), self.calls)
        self.assertEqual(st["2"]["acted"], ("low",))

    def test_restore_on_seen_after_acted(self):
        self.vis = {"2": True}
        st = {"2": {"last_seen": 500.0, "acted": ("low",), "last_probe": 500.0}}
        fc.tick(st)
        self.assertEqual(self.acts(), [("2", "play"), ("2", "full")])
        self.assertIsNone(st["2"]["acted"])

    def test_probe_throttle_after_acted(self):
        # Already acted and within PROBE_S of last probe: no re-probe, no new act.
        self.vis = {"2": False}
        st = {"2": {"last_seen": 1000.0 - (fc.UNSEEN_PAUSE_S + 5), "acted": ("low",),
                    "last_probe": 1000.0 - (fc.PROBE_S / 2)}}
        fc.tick(st)
        self.assertEqual([c for c in self.calls if c[1] == "probe"], [])
        self.assertEqual(self.acts(), [])

    def test_no_duplicate_act_when_want_unchanged(self):
        # Acted ("low",), probe still says low is wanted, but PROBE_S has elapsed so it re-probes;
        # want == acted -> must NOT POST the same action again.
        self.vis = {"2": False}
        self.probe_reply["2"] = probe(vid(muted=False))
        st = {"2": {"last_seen": 1000.0 - (fc.UNSEEN_PAUSE_S + 5), "acted": ("low",),
                    "last_probe": 1000.0 - (fc.PROBE_S + 1)}}
        fc.tick(st)
        self.assertIn(("2", "probe"), self.calls)      # it did re-probe
        self.assertEqual(self.acts(), [])              # but no repeated low

    def test_probe_404_skips(self):
        import urllib.error

        def video_404(sid, state):
            self.calls.append((str(sid), state))
            if state == "probe":
                raise urllib.error.HTTPError("u", 404, "no window", {}, None)
            return {"ok": True}

        fc.video = video_404
        self.vis = {"2": False}
        st = {"2": {"last_seen": 1000.0 - (fc.UNSEEN_PAUSE_S + 5), "acted": None, "last_probe": 0.0}}
        fc.tick(st)
        self.assertEqual(self.acts(), [])
        self.assertIsNone(st["2"]["acted"])

    def test_dry_mode_never_posts_act(self):
        fc.DRY = True   # ENABLED stays True; DRY must suppress the POSTs in _apply
        self.vis = {"2": False}
        self.probe_reply["2"] = probe(vid(muted=True))
        st = {"2": {"last_seen": 1000.0 - (fc.UNSEEN_PAUSE_S + 5), "acted": None, "last_probe": 0.0}}
        fc.tick(st)
        # probe still happens (it's read-only), but no pause/low POST
        self.assertIn(("2", "probe"), self.calls)
        self.assertEqual(self.acts(), [])
        # bookkeeping still records intent so DRY logs are meaningful
        self.assertEqual(st["2"]["acted"], ("pause",))

    def test_active_posts_status_count(self):
        self.vis = {"2": False, "3": False}
        self.screens = ["2", "3"]
        self.probe_reply = {"2": probe(vid(muted=True)), "3": probe(vid(muted=True))}
        st = {s: {"last_seen": 1000.0 - (fc.UNSEEN_PAUSE_S + 5), "acted": None, "last_probe": 0.0}
              for s in ("2", "3")}
        fc.tick(st)
        self.assertIn(("STATUS", 2), self.calls)   # both lowered/paused -> count 2


class GetVisTests(unittest.TestCase):
    """get_vis() source selection: vis file (fresh/stale), HTTP, house.selected, none."""

    def setUp(self):
        self._saved = (fc.VIS_FILE, fc.VIS_URL, fc.VIS_STALE_S, fc._get)
        self.tmp = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        self.tmp.close()
        fc.VIS_FILE = self.tmp.name
        fc.VIS_URL = None

    def tearDown(self):
        fc.VIS_FILE, fc.VIS_URL, fc.VIS_STALE_S, fc._get = self._saved
        os.unlink(self.tmp.name)

    def _write(self, obj):
        with open(self.tmp.name, "w") as f:
            json.dump(obj, f)

    def test_fresh_vis_file_thresholds(self):
        self._write({"t": time.time(), "screens": {"1": {"vis": 0.9}, "2": {"vis": 0.0}, "3": {"vis": 0.04}}})
        got = fc.get_vis()
        self.assertEqual(got, {"1": True, "2": False, "3": False})  # 0.05 threshold

    def test_stale_vis_file_is_none(self):
        fc.VIS_STALE_S = 2
        self._write({"t": time.time() - 100, "screens": {"1": {"vis": 1.0}}})
        self.assertIsNone(fc.get_vis())      # stale -> unknown -> assume visible (None)

    def test_vis_url_fallback(self):
        os.unlink(self.tmp.name)             # no file
        fc.VIS_FILE = "/nonexistent/vis.json"
        fc.VIS_URL = "http://x/vis"
        fc._get = lambda url, timeout=5: {"1": 0.8, "2": 0.0, "3": True, "4": False}
        self.assertEqual(fc.get_vis(), {"1": True, "2": False, "3": True, "4": False})
        open(self.tmp.name, "w").close()     # recreate so tearDown unlink succeeds

    def test_house_selected_fallback(self):
        os.unlink(self.tmp.name)
        fc.VIS_FILE = "/nonexistent/vis.json"
        fc.VIS_URL = None
        fc._get = lambda url, timeout=5: {"selected": [{"id": 2}, {"id": 5}]}
        self.assertEqual(fc.get_vis(), {"2": True, "5": True})   # only selected count as seen
        open(self.tmp.name, "w").close()

    def test_nothing_is_none(self):
        os.unlink(self.tmp.name)
        fc.VIS_FILE = "/nonexistent/vis.json"
        fc.VIS_URL = None

        def boom(url, timeout=5):
            raise OSError("no house")

        fc._get = boom
        self.assertIsNone(fc.get_vis())
        open(self.tmp.name, "w").close()


class WebScreensTests(unittest.TestCase):
    def setUp(self):
        self._saved = (fc.ONLY, fc.VIS_FILE, fc._get)

    def tearDown(self):
        fc.ONLY, fc.VIS_FILE, fc._get = self._saved

    def test_only_env_wins(self):
        fc.ONLY = ["7", "8"]
        self.assertEqual(fc.web_screens(), ["7", "8"])

    def test_union_house_and_vis_file_sorted(self):
        fc.ONLY = []
        fc._get = lambda url, timeout=5: {"screens": [
            {"id": 2, "source": {"kind": "web"}},
            {"id": 10, "source": {"kind": "atlas"}},
            {"id": 3, "source": {"kind": "native"}},     # excluded (not a web kind)
        ]}
        tmp = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump({"t": time.time(), "screens": {"4": {"vis": 1}, "2": {"vis": 0}}}, tmp)
        tmp.close()
        fc.VIS_FILE = tmp.name
        try:
            # house web: {2,10}; vis-file keys: {4,2}; union {2,4,10}; sorted by (len,str)
            self.assertEqual(fc.web_screens(), ["2", "4", "10"])
        finally:
            os.unlink(tmp.name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
