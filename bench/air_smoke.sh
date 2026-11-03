#!/bin/bash
# Post-deploy smoke test for the Air (run it at the end of every deploy, inside the deploy's bench-lock slot):
# a scratch helper display with the stress pattern must capture at >= MIN fps (default 25) through ScreenCaptureKit,
# measured by pinpin-atlas-vis as a PinPin Host plugin (it needs the Host's Screen Recording grant). The Air display
# is woken for the test (caffeinate -u): with it asleep, virtual displays drop out of ScreenCaptureKit.
#   bash bench/air_smoke.sh [MIN]      # exit 0 = pass; prints the fps samples
MIN=${1:-25}
out=$(ssh -o BatchMode=yes pinpin-air "MIN=$MIN bash -s" <<'AIR'
P="$HOME/Library/Application Support/PinPin Host/plugins"; ID=49
caffeinate -u -t 60 & CP=$!
curl -s -m40 -X POST "127.0.0.1:57710/displays?id=$ID&w=1920&h=1200&scale=1" >/dev/null; sleep 4
DID=$(curl -s -m10 127.0.0.1:57710/displays | python3 -c "import json,sys; l=[d[\"display\"] for d in json.load(sys.stdin) if d[\"screen\"]==$ID]; print(l[0] if l else \"\")")
spec() { printf "{\"enabled\": %s, \"exec\": \"~/.local/share/pinpin-atlas/pinpin-atlas-vis\", \"args\": [\"--test\", \"$ID:%s:1920x1200\", \"--port\", \"57740\", \"--demand-port\", \"57741\", \"--stat-secs\", \"5\"], \"log\": \"/tmp/atlas-vis.log\", \"truncateLog\": true, \"restartDelay\": 1.5}" "$1" "$DID" > "$P/atlas-vis.json"; }
if [ -n "$DID" ]; then
  (nohup "$HOME/Applications/PinPin Stress Pattern.app/Contents/MacOS/stresspattern" $DID > /tmp/smoke-pattern.log 2>&1 &); sleep 3
  spec true; sleep 22; grep -E "capture: $ID|not found" /tmp/atlas-vis.log | tail -3; spec false
  pkill -f stresspattern; sleep 2
else echo "display not created"; fi
curl -s -m40 -X POST "127.0.0.1:57710/remove?id=$ID" >/dev/null; kill $CP 2>/dev/null
AIR
)
echo "$out"
fps=$(echo "$out" | sed -n "s/.*capture: 49 \([0-9.]*\) fps.*/\1/p" | sort -n | sed -n 2p)
if [ -n "$fps" ] && python3 -c "import sys; sys.exit(0 if float(\"$fps\") >= $MIN else 1)"; then echo "SMOKE PASS: median $fps fps >= $MIN"; exit 0; fi
echo "SMOKE FAIL: median ${fps:-none} fps < $MIN"; exit 1
