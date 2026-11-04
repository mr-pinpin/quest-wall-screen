#!/bin/bash
# ASSIST headset side (room_lab/assist.h) against bench/assist_mock.py.
cd "$(dirname "$0")/.."
T=$(mktemp -d); PORT=$((20000 + RANDOM % 20000))
g++ -std=c++17 -Wall -I room_lab bench/assist_test.cpp -o "$T/at" || exit 1
python3 bench/assist_mock.py $PORT & SRV=$!
for i in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.1; done
timeout 60 "$T/at" $PORT; rc=$?
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
exit $rc
