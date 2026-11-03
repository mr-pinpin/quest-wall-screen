#!/bin/bash
# Every test that needs no hardware, in one go. Run before deploying anything to the laptop or the headset
# (AGENTS.md); exit status 0 only if all pass. Needs g++ and python3 (VPS: both present).
#   bash bench/run_offline_tests.sh
cd "$(dirname "$0")/.."
T=$(mktemp -d); fail=0
run() { local name=$1; shift; if out=$("$@" 2>&1); then echo "ok    $name: $(echo "$out" | tail -1)"; else echo "FAIL  $name"; echo "$out" | tail -20 | sed 's/^/      /'; fail=1; fi; }
LAYOUTS=$(ls bench/results/*layouts*.jsonl 2>/dev/null | tr "\n" " ")
ASSETS=$(ls -d /root/.cache/pinpin-assets/out/DamagedHelmet-* 2>/dev/null | grep -v old | tr "\n" " ")  # converted models, when present
run "budget arbiter (room_lab/budget.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/budget_test.cpp -o $T/budget && $T/budget"
run "VT headset logic + recorded layouts" bash -c "g++ -std=c++17 -O1 -Wall -Wno-misleading-indentation -I room_lab bench/vt_headset_test.cpp -o $T/vt && $T/vt $LAYOUTS"
for f in $LAYOUTS; do run "vt_check recorded $(basename $f)" python3 bench/vt_check.py recorded "$f"; done
run "vt_check synthetic packer model" python3 bench/vt_check.py synthetic --runs 500 --seed 1
run "VT replay: recorded sessions through vt::Resident" bash -c "g++ -std=c++17 -Wall -I room_lab bench/vt_replay.cpp -o $T/replay && $T/replay $LAYOUTS"
run "gate.py self-test" python3 bench/gate_test.py
run "3D models: json, glb, progressive ktx2, asset budget" bash -c "g++ -std=c++17 -Wall -I room_lab bench/model_test.cpp -o $T/model && $T/model $ASSETS"
run "laptop discovery (room_lab/hosts.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/hosts_test.cpp -o $T/hosts && $T/hosts"
run "verdict.py self-test (pass/drop rules)" python3 bench/verdict.py --self-test
run "bench scripts compile" python3 -m py_compile bench/stress.py bench/gate.py bench/headset_hold.py bench/vt_check.py bench/verdict.py bench/stress/headset_sim.py
mv "$T" "$T.done" 2>/dev/null   # scratch binaries stay in /tmp
[ $fail = 0 ] && echo "all offline tests passed" || echo "OFFLINE TESTS FAILED"
exit $fail
