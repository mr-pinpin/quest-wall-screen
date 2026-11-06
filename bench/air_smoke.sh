#!/bin/bash
# Post-deploy smoke test for the Air (run it at the end of every deploy, inside the deploy's bench-lock slot):
# a scratch helper display with the stress pattern must capture at >= MIN fps (default 25) through ScreenCaptureKit,
# measured by pinpin-atlas-vis as a PinPin Host plugin (it needs the Host's Screen Recording grant). The Air display
# is woken for the test (caffeinate -u): with it asleep, virtual displays drop out of ScreenCaptureKit.
#   bash bench/air_smoke.sh [MIN]      # exit 0 = pass; prints the fps samples
MIN=${1:-25}
out=$(ssh -o BatchMode=yes pinpin-air "MIN=$MIN bash -s" <<'AIR'
P="$HOME/Library/Application Support/PinPin Host/plugins"; ID=48  # not 29 or 49: macOS remembers a display identity's wallpaper, and those two kept the still wallpaper that stops the pattern from reaching capture (2026-11-03)
caffeinate -u -t 60 & CP=$!
# K30: a stale pinpin-displays (dead displays after a WindowServer crash, or an orphan without its Host) fails here
# in one request instead of 22 s of "display not found". Older binaries have no /health: skipped.
if bad=$(curl -s -m5 127.0.0.1:57710/health | python3 -c "import json,sys
try: h=json.load(sys.stdin)
except ValueError: sys.exit(1)
p=[\"dead displays %s\" % h[\"dead\"]] if h.get(\"dead\") else []
p+=[\"orphan (ppid 1)\"] if h.get(\"orphan\") else []
print(\", \".join(p)); sys.exit(0 if p else 1)"); then
  echo "pinpin-displays unhealthy: $bad"; kill $CP 2>/dev/null; exit 0
fi
# scratch display pool (AGENTS.md): 48 is created once and never removed; every create/remove raises colorsync's
# display polling for good (K24). POST /displays always recreates, so only when 48 is missing or another size.
if ! curl -s -m10 127.0.0.1:57710/displays | python3 -c "import json,sys; sys.exit(0 if any(d[\"screen\"]==$ID and d[\"w\"]==1920 and d[\"h\"]==1200 for d in json.load(sys.stdin)) else 1)"; then
  curl -s -m40 -X POST "127.0.0.1:57710/displays?id=$ID&w=1920&h=1200&scale=1" >/dev/null; sleep 4
fi
DID=$(curl -s -m10 127.0.0.1:57710/displays | python3 -c "import json,sys; l=[d[\"display\"] for d in json.load(sys.stdin) if d[\"screen\"]==$ID]; print(l[0] if l else \"\")")
spec() { printf "{\"enabled\": %s, \"exec\": \"~/.local/share/pinpin-atlas/pinpin-atlas-vis\", \"args\": [\"--test\", \"$ID:%s:1920x1200\", \"--port\", \"57740\", \"--demand-port\", \"57741\", \"--stat-secs\", \"5\"], \"log\": \"/tmp/atlas-vis.log\", \"truncateLog\": true, \"restartDelay\": 1.5}" "$1" "$DID" > "$P/atlas-vis.json"; }
if [ -n "$DID" ]; then
  (nohup "$HOME/Applications/PinPin Stress Pattern.app/Contents/MacOS/stresspattern" $DID > /tmp/smoke-pattern.log 2>&1 &); sleep 3
  spec true; sleep 22; grep -E "capture: $ID|not found" /tmp/atlas-vis.log | tail -3; spec false
  pkill -f stresspattern; sleep 2
else echo "display not created"; fi
kill $CP 2>/dev/null  # display 48 stays (the pool): the pattern window is closed above, so it is idle
AIR
)
echo "$out"
fps=$(echo "$out" | sed -n "s/.*capture: 48 \([0-9.]*\) fps.*/\1/p" | sort -n | sed -n 2p)
if [ -n "$fps" ] && python3 -c "import sys; sys.exit(0 if float(\"$fps\") >= $MIN else 1)"; then echo "SMOKE PASS: median $fps fps >= $MIN"; exit 0; fi
echo "SMOKE FAIL: median ${fps:-none} fps < $MIN"; exit 1
