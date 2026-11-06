#!/bin/bash
# HEARTBEAT (room_lab/heartbeat.h) against a throwaway house/server.py seeded with house/house.json.
cd "$(dirname "$0")/.."
T=$(mktemp -d); PORT=$((20000 + RANDOM % 20000))
mkdir -p "$T/data" && cp house/house.json "$T/data/house.json"
g++ -std=c++17 -Wall -I room_lab bench/heartbeat_test.cpp -o "$T/hb" || exit 1
python3 house/server.py --data "$T/data" --port $PORT --bind 127.0.0.1 >"$T/server.log" 2>&1 & SRV=$!
for i in $(seq 50); do curl -s -o /dev/null "127.0.0.1:$PORT/house" && break; sleep 0.1; done
"$T/hb" $PORT "$T/data"; rc=$?
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
[ $rc = 0 ] || tail -20 "$T/server.log"
exit $rc
