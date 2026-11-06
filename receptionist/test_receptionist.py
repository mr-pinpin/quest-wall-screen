#!/usr/bin/env python3
"""Offline tests for the receptionist (no live sessions, no network, no tmux)."""
import json, os, sys, tempfile, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import recept_core as rc
import pinpin_receptionist as svc

PASS = 0
FAIL = 0


def check(name, cond):
    global PASS, FAIL
    if cond:
        PASS += 1; print("  ok  -", name)
    else:
        FAIL += 1; print("  FAIL-", name)


def test_topic():
    d = tempfile.mkdtemp()
    tp = os.path.join(d, "t.jsonl")
    with open(tp, "w") as f:
        f.write(json.dumps({"type": "user", "message": {"content": "hello"}}) + "\n")
        f.write(json.dumps({"type": "ai-title", "aiTitle": "Old title"}) + "\n")
        f.write(json.dumps({"type": "assistant", "timestamp": "2026-11-04T08:00:00.0Z",
                            "message": {"content": [{"type": "text", "text": "hi"}]}}) + "\n")
        f.write(json.dumps({"type": "last-prompt", "lastPrompt": "the weather"}) + "\n")
        f.write(json.dumps({"type": "ai-title", "aiTitle": "Newest title"}) + "\n")
    topic, last_prompt, ts = rc._topic_and_activity(tp)
    check("latest aiTitle wins", topic == "Newest title")
    check("lastPrompt captured", last_prompt == "the weather")
    check("assistant timestamp captured", ts == "2026-11-04T08:00:00.0Z")
    shutil.rmtree(d)


def _fixture_index():
    """Build a temp sessions/projects tree with a couple of live sessions; return (root, restore)."""
    root = tempfile.mkdtemp()
    sess = os.path.join(root, "sessions"); os.makedirs(sess)
    proj = os.path.join(root, "projects"); os.makedirs(proj)
    sockf = os.path.join(root, "live.sock"); open(sockf, "w").close()
    cwd = "/home/x/work"; slug = rc._cwd_slug(cwd); os.makedirs(os.path.join(proj, slug))
    for name, sid, topic in (("work-df", "d" * 8 + "-x", "Captain"), ("work-c4", "c" * 8 + "-y", "Protocol")):
        with open(os.path.join(proj, slug, sid + ".jsonl"), "w") as f:
            f.write(json.dumps({"type": "ai-title", "aiTitle": topic}) + "\n")
        json.dump({"pid": os.getpid(), "sessionId": sid, "cwd": cwd, "status": "idle",
                   "kind": "interactive", "name": name, "messagingSocketPath": sockf,
                   "statusUpdatedAt": 1}, open(os.path.join(sess, name + ".json"), "w"))
    old = (rc.SESS_DIR, rc.PROJECTS)
    rc.SESS_DIR, rc.PROJECTS = sess, proj
    return root, lambda: setattr_tuple(old)


def setattr_tuple(old):
    rc.SESS_DIR, rc.PROJECTS = old


def test_index():
    root, restore = _fixture_index()
    try:
        names = [s["name"] for s in rc.list_sessions()]
        check("work-df listed", "work-df" in names)
        check("work-c4 listed", "work-c4" in names)
        check("tmux name derived", rc.resolve("work-df")["tmux"].startswith("claude-"))
    finally:
        restore(); shutil.rmtree(root)


def test_deliver_verbatim():
    calls = []
    import subprocess
    orig = subprocess.run
    subprocess.run = lambda *a, **k: calls.append(a[0]) or type("R", (), {"returncode": 0})()
    try:
        rc.deliver("abc-123", "turn OFF; rm -rf /  (literal)")
    finally:
        subprocess.run = orig
    first = calls[0]
    check("delivery targets the session tmux", "claude-abc-123" in first)
    check("uses -l literal (no interpretation)", "-l" in first)
    check("verbatim prefix exactly '[via headset] '",
          first[-1] == "[via headset] turn OFF; rm -rf /  (literal)")
    check("second call is a bare Enter", calls[1][-1] == "Enter")


def test_detect_switch():
    root, restore = _fixture_index()
    try:
        check("'talk to work-c4' -> work-c4", svc.detect_switch("talk to work-c4") == "work-c4")
        check("'switch to work c4' (spoken) -> work-c4", svc.detect_switch("switch to work c4") == "work-c4")
        check("'connect me to work-df' -> work-df", svc.detect_switch("connect me to work-df") == "work-df")
        check("plain sentence -> no switch", svc.detect_switch("tell them dinner is at seven") is None)
        check("'talk to the oven' (no such session) -> None", svc.detect_switch("talk to the oven") is None)
    finally:
        restore(); shutil.rmtree(root)


def test_audit():
    d = tempfile.mkdtemp()
    old = svc.AUDIT_LOG; svc.AUDIT_LOG = os.path.join(d, "r.log")
    try:
        svc.audit("route", "sid1", "secret words")
        svc.audit("switch", "sid2", "")
        lines = open(svc.AUDIT_LOG).read().strip().splitlines()
        check("two audit lines appended", len(lines) == 2)
        r0 = json.loads(lines[0])
        check("audit has time/action/session", all(k in r0 for k in ("t", "action", "session")))
        check("audit stores a hash, not the text", "secret words" not in lines[0] and r0["text_sha256"])
    finally:
        svc.AUDIT_LOG = old; shutil.rmtree(d)


def test_reply_cleaner():
    c = svc.ReplyCleaner()
    md = ("Here is the plan:\n"
          "| Screen | Where |\n"
          "|--------|-------|\n"
          "| 3 | kitchen |\n"
          "- first item\n"
          "* second item\n"
          "### A heading\n")
    out = c.feed(md) + c.flush()
    check("table separator row dropped", "---" not in out)
    check("table row flattened to cells", "Screen: Where" in out and "3: kitchen" in out)
    check("no pipes remain", "|" not in out)
    check("list markers stripped", "first item" in out and "- first item" not in out and "* second item" not in out)
    check("heading hashes stripped", "A heading" in out and "###" not in out)
    # streaming: a line split across feeds is held until its newline
    c2 = svc.ReplyCleaner()
    check("partial line buffered (no premature emit)", c2.feed("hello wo") == "")
    check("line completes on newline", c2.feed("rld\n").strip() == "hello world")


def test_focus_policy():
    import focus_controller as fc
    P = lambda muted: {"videos": [{"playing": True, "paused": False, "muted": muted}]}
    check("hysteresis: <5s unseen holds", fc.decide_unseen(3, P(True)) is None)
    check("unseen >=5s audible -> low", fc.decide_unseen(7, P(False)) == ("low",))
    check("unseen >=5s muted <60s -> low", fc.decide_unseen(7, P(True)) == ("low",))
    check("unseen >=60s muted -> pause", fc.decide_unseen(70, P(True)) == ("pause",))
    check("never pause audible (even >60s)", fc.decide_unseen(70, P(False)) == ("low",))
    check("no playing video -> no action", fc.decide_unseen(70, {"videos": []}) is None)


def test_history():
    d = tempfile.mkdtemp()
    old = svc.CONV_DIR; svc.CONV_DIR = os.path.join(d, "conv")
    try:
        check("unknown conv -> empty", svc.history("nope", 50)["messages"] == [])
        svc.record("c1", "user", "play jazz", "work-df")
        svc.record("c1", "assistant", "Okay, playing jazz.", "work-df")
        svc.record("c1", "", "", "work-df")   # empty ignored
        h = svc.history("c1", 50)
        check("records in order", [m["role"] for m in h["messages"]] == ["user", "assistant"])
        check("message has role/text/t/dest", all(k in h["messages"][0] for k in ("role", "text", "t", "dest")))
        check("empty message skipped", len(h["messages"]) == 2)
        for i in range(10):
            svc.record("c1", "user", "m%d" % i)
        check("limit returns last N", len(svc.history("c1", 3)["messages"]) == 3)
        check("last message is newest", svc.history("c1", 1)["messages"][0]["text"] == "m9")
    finally:
        svc.CONV_DIR = old; shutil.rmtree(d)


def test_block():
    old = svc.BLOCKED; svc.BLOCKED = {"work-df", "deadbeef"}
    try:
        check("blocked by name", svc._is_blocked({"name": "work-df", "session_id": "x"}))
        check("blocked by id", svc._is_blocked({"name": "other", "session_id": "deadbeef"}))
        check("not blocked otherwise", not svc._is_blocked({"name": "work-c4", "session_id": "y"}))
    finally:
        svc.BLOCKED = old


def test_heartbeat_compare():
    import room_tools as rt
    doc = {"screens": [{"id": 1, "on": 1, "x": -0.374, "y": 0.475, "w": 2.4, "h": 1.5},
                       {"id": 3, "on": 0, "x": 0.728, "y": 0.177, "w": 0.9, "h": 0.5625},
                       {"id": 4, "on": 1, "x": 0, "y": 0, "w": 0.6, "h": 0.4}]}
    beat = {"screens": [{"id": 1, "on": 1, "x": -0.371, "y": 0.475, "w": 2.4, "h": 1.5},
                        {"id": 3, "on": 1, "x": 0.728, "y": 0.177, "w": 1.05, "h": 0.5625}]}
    c = rt.compare(beat, doc)
    check("screen 1 within 1 cm: ok", c[1]["ok"])
    check("screen 3 still on and 1.05 m: both reported", not c[3]["ok"] and c[3]["diff"]["on"] == [0, 1] and c[3]["diff"]["w"] == [0.9, 1.05])
    check("screen 4 (on) not reported", not c[4]["ok"] and "missing" in c[4]["diff"])
    check("an off screen not reported is right", rt.compare({"screens": []}, {"screens": [{"id": 2, "on": 0}]})[2]["ok"])
    beat["screens"][1].update(on=0, w=0.9)
    check("screen 3 applied: ok", rt.compare(beat, doc)[3]["ok"])


def test_unreachable():
    import room_tools as rt, socket, threading
    old = (rt.HOUSE, rt.TIMEOUT)

    def run(argv):
        try:
            rt.main(argv); return 0, ""
        except SystemExit as e:
            return e.code, str(e.code)
    try:
        # Nothing listening: connection refused.
        s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
        rt.HOUSE = "http://127.0.0.1:%d" % port
        for argv in (["status"], ["house_info"]):
            code, msg = run(argv)
            check("refused %s: one line, nonzero" % argv[0], code and msg.startswith("house server unreachable (") and "\n" not in msg and len(msg) < 120)
        # Accepts but never answers: timeout.
        srv = socket.socket(); srv.bind(("127.0.0.1", 0)); srv.listen(4); rt.TIMEOUT = 0.5
        rt.HOUSE = "http://127.0.0.1:%d" % srv.getsockname()[1]
        code, msg = run(["status"])
        check("silent server: timeout reported in one line", code and msg.startswith("house server unreachable (") and "timed out" in msg)
        srv.close()
    finally:
        rt.HOUSE, rt.TIMEOUT = old


if __name__ == "__main__":
    for t in (test_topic, test_index, test_deliver_verbatim, test_detect_switch,
              test_reply_cleaner, test_focus_policy, test_history, test_audit, test_block, test_heartbeat_compare, test_unreachable):
        print(t.__name__); t()
    print("\n%d passed, %d failed" % (PASS, FAIL))
    sys.exit(1 if FAIL else 0)
