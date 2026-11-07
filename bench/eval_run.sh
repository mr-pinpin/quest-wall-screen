#!/bin/bash
# Run the assistant eval on the Air: point the service's house at the SCRATCH server, create display 60, run the
# suite, then ALWAYS revert the house to real, remove display 60, release the lock.
set -u
SPEC="$HOME/Library/Application Support/PinPin Host/plugins/assistant.json"
BL="${BENCHLOCK:-/tmp/benchlock.py}"             # benchlock.py, shipped next to assistant_eval.py by nightly-eval.sh
export BENCHLOCK_LOCAL="$HOME/.local/share"      # act on the Air's own lock dir directly (no ssh back to self)

setbase() {  # $1 = base url or "" to remove
  python3 - "$SPEC" "$1" <<'PY'
import json, sys
p, base = sys.argv[1], sys.argv[2]
d = json.load(open(p))
d.setdefault("env", {})
if base:
    d["env"]["PINPIN_HOUSE_BASE"] = base
else:
    d["env"].pop("PINPIN_HOUSE_BASE", None)
json.dump(d, open(p, "w"))
PY
}

# Take the bench lock EXCLUSIVE via benchlock.py, NOT raw `mkdir`: benchlock moves aside a lock left by a CRASHED
# holder (older than STALE_S, 30 min) instead of failing forever on it, writes its own owner file, and on release
# only ever frees OUR lock. --wait 240 s lets a legitimate live holder finish; --max 1800 s caps our own hold.
[ -f "$BL" ] || { echo "LOCK FAIL: benchlock.py not at $BL"; exit 9; }
TOK=$(python3 "$BL" take exclusive --who "work-42 eval" --max 1800 --wait 240) || { echo "LOCK FAIL (${TOK:-timed out waiting for a live holder})"; exit 9; }
echo "LOCK taken ($TOK)"

CAFF=
cleanup() {
  [ -n "$CAFF" ] && kill "$CAFF" 2>/dev/null
  setbase ""            # revert to the real house
  sleep 5               # let PinPin Host restart the plugin from the changed spec
  # the app-build case assigns an app to screen 60 via the apps server (LIVE house, not scratch) — unassign it
  TOKEN=$(cat ~/.local/share/pinpin-apps/token 2>/dev/null)
  curl -s -m8 -X POST "http://192.168.0.50:57830/api/screens/60/app" -H "Authorization: Bearer $TOKEN" \
    -H 'Content-Type: application/json' -d '{"app":null}' >/dev/null 2>&1
  # fixed-pool rule (K24): never POST /remove (ramps colorsync); the app-unassign above clears content
  [ -n "${TOK:-}" ] && python3 "$BL" release "$TOK" 2>/dev/null   # frees only our own lock (benchlock checks $L/id)
  echo "CLEANUP: house reverted to real ($(curl -s -m4 http://127.0.0.1:57840/health >/dev/null 2>&1 && echo up)), display 60 removed, lock released"
}
trap cleanup EXIT

caffeinate -u -t 1200 & CAFF=$!
setbase "http://192.168.0.50:57801"   # point house at scratch
sleep 6                               # host restarts the plugin with the new env
echo "health after switch: $(curl -s -m3 http://127.0.0.1:57840/health)"
curl -s -m5 http://127.0.0.1:57710/displays 2>/dev/null | grep -q '"screen":60' || \
  curl -s -m5 -X POST "http://127.0.0.1:57710/displays?id=60&w=1600&h=1000&scale=2" >/dev/null   # fixed pool: create once, never remove (K24)
sleep 2
echo "=== EVAL ==="
python3 /tmp/assistant_eval.py
