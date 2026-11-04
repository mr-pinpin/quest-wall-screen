#!/usr/bin/env python3
"""Offline tests for the receptionist (no live sessions, no network, no tmux)."""
import json, os, sys, tempfile, time, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import recept_core as rc
import pinpin_receptionist as svc

PASS = 0
FAIL = 0


def check(name, cond):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  ok  -", name)
    else:
        FAIL += 1
        print("  FAIL-", name)


# ---------- recept_core: transcript topic/activity ----------
def test_topic():
    d = tempfile.mkdtemp()
    tp = os.path.join(d, "t.jsonl")
    with open(tp, "w") as f:
        f.write(json.dumps({"type": "user", "message": {"content": "hello there"}}) + "\n")
        f.write(json.dumps({"type": "ai-title", "aiTitle": "Old title"}) + "\n")
        f.write(json.dumps({"type": "assistant", "timestamp": "2026-11-04T08:00:00.0Z",
                            "message": {"content": [{"type": "text", "text": "hi"}]}}) + "\n")
        f.write(json.dumps({"type": "last-prompt", "lastPrompt": "what is the weather"}) + "\n")
        f.write(json.dumps({"type": "ai-title", "aiTitle": "Newest title"}) + "\n")
    topic, last_prompt, ts = rc._topic_and_activity(tp)
    check("latest aiTitle wins", topic == "Newest title")
    check("lastPrompt captured", last_prompt == "what is the weather")
    check("assistant timestamp captured", ts == "2026-11-04T08:00:00.0Z")
    shutil.rmtree(d)


# ---------- recept_core: list_sessions joins registry + transcript, filters dead/socketless ----------
def test_index():
    root = tempfile.mkdtemp()
    sess = os.path.join(root, "sessions"); os.makedirs(sess)
    proj = os.path.join(root, "projects"); os.makedirs(proj)
    sockf = os.path.join(root, "live.sock"); open(sockf, "w").close()
    cwd = "/home/x/work"
    slug = rc._cwd_slug(cwd)
    os.makedirs(os.path.join(proj, slug))
    sid = "11111111-2222-3333-4444-555555555555"
    with open(os.path.join(proj, slug, sid + ".jsonl"), "w") as f:
        f.write(json.dumps({"type": "ai-title", "aiTitle": "My topic"}) + "\n")
    # alive session (this pid) with a present socket
    json.dump({"pid": os.getpid(), "sessionId": sid, "cwd": cwd, "status": "idle",
               "kind": "interactive", "name": "work-zz", "messagingSocketPath": sockf,
               "statusUpdatedAt": 123}, open(os.path.join(sess, "a.json"), "w"))
    # dead pid -> filtered
    json.dump({"pid": 999999, "sessionId": "dead", "cwd": cwd, "name": "work-dead",
               "messagingSocketPath": sockf}, open(os.path.join(sess, "b.json"), "w"))
    # alive but socket missing -> filtered
    json.dump({"pid": os.getpid(), "sessionId": "nos", "cwd": cwd, "name": "work-nosock",
               "messagingSocketPath": "/nope/x.sock"}, open(os.path.join(sess, "c.json"), "w"))
    old = (rc.SESS_DIR, rc.PROJECTS)
    rc.SESS_DIR, rc.PROJECTS = sess, proj
    try:
        out = rc.list_sessions()
        names = [s["name"] for s in out]
        check("alive+socketed session listed", "work-zz" in names)
        check("dead session filtered", "work-dead" not in names)
        check("socketless session filtered", "work-nosock" not in names)
        s = next(s for s in out if s["name"] == "work-zz")
        check("topic joined from transcript", s["topic"] == "My topic")
        check("tmux name derived", s["tmux"] == "claude-" + sid)
    finally:
        rc.SESS_DIR, rc.PROJECTS = old
        shutil.rmtree(root)


# ---------- recept_core: deliver() frames verbatim and never interprets the body ----------
def test_deliver_verbatim():
    calls = []
    import subprocess
    orig = subprocess.run
    subprocess.run = lambda *a, **k: calls.append(a[0]) or type("R", (), {"returncode": 0})()
    try:
        rc.deliver("abc-123", "turn OFF; rm -rf /  (should be literal)")
    finally:
        subprocess.run = orig
    # first call: send-keys -l <framed>, framed must be exactly prefix + user text, unchanged
    first = calls[0]
    check("delivery targets the session tmux", "claude-abc-123" in first)
    check("uses -l literal (no interpretation)", "-l" in first)
    framed = first[-1]
    check("verbatim prefix exactly '[via headset] '", framed == "[via headset] turn OFF; rm -rf /  (should be literal)")
    check("second call is a bare Enter", calls[1][-1] == "Enter")


# ---------- service: audit log append-only, correct shape ----------
def test_audit():
    d = tempfile.mkdtemp()
    old = svc.AUDIT_LOG
    svc.AUDIT_LOG = os.path.join(d, "r.log")
    try:
        svc.audit("user", "route", "sid1", "secret words")
        svc.audit("wife", "refused-engineering", "sid2", "hi")
        lines = open(svc.AUDIT_LOG).read().strip().splitlines()
        check("two audit lines appended", len(lines) == 2)
        r0 = json.loads(lines[0])
        check("audit has time/speaker/action/session", all(k in r0 for k in ("t", "speaker", "action", "session")))
        check("audit stores a hash, not the text", "secret words" not in lines[0] and r0["text_sha256"])
    finally:
        svc.AUDIT_LOG = old
        shutil.rmtree(d)


# ---------- service: confirm gate TTL ----------
def test_confirm_gate():
    svc._pending.clear()
    check("route blocked before propose", not svc.confirm_ok("sX"))
    svc.propose("sX")
    check("route allowed right after propose", svc.confirm_ok("sX"))
    svc.clear_pending("sX")
    check("route blocked after clear (one-shot)", not svc.confirm_ok("sX"))
    svc.propose("sY")
    svc._pending["sY"] = time.time() - (svc._PENDING_TTL + 1)
    check("propose expires after TTL", not svc.confirm_ok("sY"))


# ---------- service: block-list ----------
def test_block():
    old = svc.BLOCKED
    svc.BLOCKED = {"work-df", "deadbeef"}
    try:
        check("blocked by name", svc._is_blocked({"name": "work-df", "session_id": "x"}))
        check("blocked by id", svc._is_blocked({"name": "other", "session_id": "deadbeef"}))
        check("not blocked otherwise", not svc._is_blocked({"name": "work-c4", "session_id": "y"}))
    finally:
        svc.BLOCKED = old


if __name__ == "__main__":
    for t in (test_topic, test_index, test_deliver_verbatim, test_audit, test_confirm_gate, test_block):
        print(t.__name__)
        t()
    print("\n%d passed, %d failed" % (PASS, FAIL))
    sys.exit(1 if FAIL else 0)
