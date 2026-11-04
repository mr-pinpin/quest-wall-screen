#!/bin/bash
# Run the assistant eval on the Air: point the service's house at the SCRATCH server, create display 60, run the
# suite, then ALWAYS revert the house to real, remove display 60, release the lock.
set -u
SPEC="$HOME/Library/Application Support/PinPin Host/plugins/assistant.json"
L=$HOME/.local/share/pinpin-bench.lock

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

taken=
for i in $(seq 1 18); do
  if mkdir "$L" 2>/dev/null; then echo "work-42 eval" > "$L/owner"; taken=1; echo "LOCK taken ($i)"; break; fi
  echo "held by $(cat "$L/owner" 2>/dev/null) — retry $i"; sleep 12
done
[ -n "$taken" ] || { echo "LOCK FAIL"; exit 9; }

CAFF=
cleanup() {
  [ -n "$CAFF" ] && kill "$CAFF" 2>/dev/null
  setbase ""            # revert to the real house
  sleep 5               # let PinPin Host restart the plugin from the changed spec
  # the app-build case assigns an app to screen 60 via the apps server (LIVE house, not scratch) — unassign it
  TOKEN=$(cat ~/.local/share/pinpin-apps/token 2>/dev/null)
  curl -s -m8 -X POST "http://192.168.0.50:57830/api/screens/60/app" -H "Authorization: Bearer $TOKEN" \
    -H 'Content-Type: application/json' -d '{"app":null}' >/dev/null 2>&1
  curl -s -m5 -X POST "http://127.0.0.1:57710/remove?id=60" >/dev/null 2>&1
  [ -d "$L" ] && mv "$L" "$L.done-$(date +%s)"
  echo "CLEANUP: house reverted to real ($(curl -s -m4 http://127.0.0.1:57840/health >/dev/null 2>&1 && echo up)), display 60 removed, lock released"
}
trap cleanup EXIT

caffeinate -u -t 1200 & CAFF=$!
setbase "http://192.168.0.50:57801"   # point house at scratch
sleep 6                               # host restarts the plugin with the new env
echo "health after switch: $(curl -s -m3 http://127.0.0.1:57840/health)"
curl -s -m5 -X POST "http://127.0.0.1:57710/displays?id=60&w=1600&h=1000&scale=2" >/dev/null; sleep 2
echo "=== EVAL ==="
python3 /tmp/assistant_eval.py
