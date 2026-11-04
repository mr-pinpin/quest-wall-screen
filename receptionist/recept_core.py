#!/usr/bin/env python3
"""Receptionist core primitives: session index, tmux delivery, transcript reply-tail.

Read-only except deliver()/spawn(), which shell out to `sudo -n tmux`. No secrets here.
Runs as pinpin-claude on the VPS. Pure-stdlib so it can live in the plugin/service.
"""
import json, os, re, subprocess, time, glob

HOME = os.path.expanduser("~")
SESS_DIR = os.path.join(HOME, ".claude", "sessions")
PROJECTS = os.path.join(HOME, ".claude", "projects")


def _alive(pid):
    try:
        os.kill(int(pid), 0)
        return True
    except (OSError, ValueError):
        return False


def _cwd_slug(cwd):
    # ~/.claude/projects dir name is the cwd with / and . turned into -
    return re.sub(r"[/.]", "-", cwd)


def _transcript_path(cwd, session_id):
    p = os.path.join(PROJECTS, _cwd_slug(cwd), session_id + ".jsonl")
    return p if os.path.exists(p) else None


def _tail_lines(path, max_bytes=262144):
    """Last chunk of a file, split to whole lines (cheap, avoids reading huge transcripts)."""
    try:
        sz = os.path.getsize(path)
        with open(path, "rb") as f:
            if sz > max_bytes:
                f.seek(sz - max_bytes)
                f.readline()  # drop partial line
            return f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return []


def _topic_and_activity(path):
    """Return (topic, last_prompt, last_assistant_iso) scanning the tail of a transcript.

    aiTitle and last-prompt are regenerated and re-appended through the session, so the
    latest of each sits near the end of the file and is captured by the tail window."""
    topic = None
    last_prompt = None
    last_text_ts = None
    for line in _tail_lines(path):
        line = line.strip()
        if not line:
            continue
        try:
            o = json.loads(line)
        except ValueError:
            continue
        t = o.get("type")
        if t == "ai-title":
            topic = o.get("aiTitle") or topic
        elif t == "last-prompt":
            last_prompt = o.get("lastPrompt") or last_prompt
        elif t == "assistant":
            for b in (o.get("message", {}).get("content") or []):
                if isinstance(b, dict) and b.get("type") == "text" and b.get("text", "").strip():
                    last_text_ts = o.get("timestamp") or last_text_ts
    return topic, last_prompt, last_text_ts


def list_sessions(include_self=None):
    """Live sessions, most-recently-active first."""
    out = []
    for jf in glob.glob(os.path.join(SESS_DIR, "*.json")):
        try:
            d = json.load(open(jf))
        except (ValueError, OSError):
            continue
        pid = d.get("pid")
        if not pid or not _alive(pid):
            continue
        if not os.path.exists(d.get("messagingSocketPath", "")):
            continue
        sid = d.get("sessionId")
        cwd = d.get("cwd", "")
        tpath = _transcript_path(cwd, sid) if sid else None
        topic, last_prompt, last_text_ts = _topic_and_activity(tpath) if tpath else (None, None, None)
        out.append({
            "name": d.get("name"),
            "session_id": sid,
            "cwd": cwd,
            "status": d.get("status"),
            "kind": d.get("kind"),
            "updated_at": d.get("statusUpdatedAt") or d.get("updatedAt"),
            "tmux": "claude-" + sid if sid else None,
            "topic": topic,
            "last_prompt": last_prompt,
            "last_assistant": last_text_ts,
            "is_self": (d.get("name") == include_self),
        })
    out.sort(key=lambda s: s.get("updated_at") or 0, reverse=True)
    return out


def resolve(name_or_id):
    for s in list_sessions():
        if s["name"] == name_or_id or s["session_id"] == name_or_id:
            return s
    return None


def deliver(session_id, text):
    """Type `text` into the session's tmux input and submit. Returns send epoch-ms watermark."""
    target = "claude-" + session_id
    watermark = time.time()
    framed = "[via headset] " + text  # VERBATIM: user's words unchanged, only this prefix
    # send the literal text (-l), then a separate Enter so send-keys never interprets the body
    subprocess.run(["sudo", "-n", "tmux", "send-keys", "-t", target, "-l", framed],
                   check=True, timeout=10)
    time.sleep(0.15)
    subprocess.run(["sudo", "-n", "tmux", "send-keys", "-t", target, "Enter"],
                   check=True, timeout=10)
    return watermark


def spawn_engineering(brief, session_id=None):
    """Start a new engineering session exactly like the existing ones (user-only)."""
    import uuid
    sid = session_id or str(uuid.uuid4())
    inner = (
        "runuser -u pinpin-claude -- /usr/bin/env "
        "HOME=/home/pinpin-claude PATH=/home/pinpin-claude/.local/bin:/usr/local/bin:/usr/bin:/bin "
        "claude --dangerously-skip-permissions --session-id %s %s"
        % (sid, _shquote(brief or "New session from the headset receptionist."))
    )
    subprocess.run(["sudo", "-n", "tmux", "new-session", "-d", "-s", "claude-" + sid, inner],
                   check=True, timeout=15)
    return sid


def _shquote(s):
    return "'" + s.replace("'", "'\\''") + "'"


def kill_session(session_id):
    """Tear down a session's tmux (used for scratch cleanup)."""
    subprocess.run(["sudo", "-n", "tmux", "kill-session", "-t", "claude-" + session_id],
                   timeout=10)


def _iso_to_epoch(iso):
    if not iso:
        return 0.0
    try:
        return time.mktime(time.strptime(iso[:19], "%Y-%m-%dT%H:%M:%S"))  # UTC-ish; only for ordering
    except ValueError:
        return 0.0


def tail_reply(session_id, since_epoch, quiet_s=6.0, hard_cap_s=120.0, poll_s=0.5):
    """Yield new assistant text blocks appearing after `since_epoch` (wall clock).

    Ends after `quiet_s` with no new text once the session is idle, or `hard_cap_s`."""
    s = resolve(session_id)
    if not s or not s["session_id"]:
        return
    tpath = _transcript_path(s["cwd"], s["session_id"])
    if not tpath:
        return
    seen = set()
    start = time.time()
    last_new = time.time()
    while True:
        now = time.time()
        if now - start > hard_cap_s:
            break
        produced = False
        for line in _tail_lines(tpath):
            line = line.strip()
            if not line:
                continue
            try:
                o = json.loads(line)
            except ValueError:
                continue
            if o.get("type") != "assistant":
                continue
            ts = _iso_to_epoch(o.get("timestamp"))
            if ts < since_epoch - 2:  # small skew allowance
                continue
            uid = o.get("uuid")
            if uid in seen:
                continue
            for b in (o.get("message", {}).get("content") or []):
                if isinstance(b, dict) and b.get("type") == "text":
                    txt = b.get("text", "")
                    if txt.strip():
                        seen.add(uid)
                        last_new = time.time()
                        produced = True
                        yield txt
        # stop condition: session idle and quiet
        cur = resolve(session_id)
        idle = (cur and cur.get("status") == "idle")
        if idle and (time.time() - last_new) > quiet_s and not produced:
            break
        time.sleep(poll_s)


if __name__ == "__main__":
    import sys
    if len(sys.argv) >= 2 and sys.argv[1] == "ls":
        for s in list_sessions():
            print(f"{s['name'] or '?':10} {s['status'] or '':5} {s['session_id'][:8]}  {s['topic'] or ''}"[:140])
    else:
        print(json.dumps(list_sessions(), indent=2))
