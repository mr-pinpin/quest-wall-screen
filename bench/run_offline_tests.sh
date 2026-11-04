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
run "watchdog (fake logs)" python3 bench/watchdog_test.py
run "3D models: json, glb, progressive ktx2, asset budget" bash -c "g++ -std=c++17 -Wall -I room_lab bench/model_test.cpp -o $T/model && $T/model $ASSETS"
run "LOD by viewer pose (vt.h projectedPx, pose paths)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/lod_test.cpp -o $T/lod && $T/lod"
run "hero selection (vt.h Heroes)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/hero_test.cpp -o $T/hero && $T/hero"
run "keyframe requests (vt.h KeyReq)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/keyreq_test.cpp -o $T/keyreq && $T/keyreq"
run "settings explanations + AMBI under VT (bench/ui_v044_test.cpp)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/ui_v044_test.cpp -o $T/ui44 && $T/ui44"
run "shoulder menu (room_lab/shoulder.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/shoulder_test.cpp -o $T/shoulder && $T/shoulder"
run "laptop discovery (room_lab/hosts.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/hosts_test.cpp -o $T/hosts && $T/hosts"
run "depth wall fitting (room_lab/wallfit.h, synthetic depth)" bash -c "g++ -std=c++17 -O2 -Wall -I room_lab bench/wallfit_test.cpp -o $T/wallfit && $T/wallfit"
run "house file: schema, migration, sync server (house/test_house.py)" python3 house/test_house.py
run "verdict.py self-test (pass/drop rules)" python3 bench/verdict.py --self-test
run "video LOD ladder along pose paths (bench/ladder_sim.cpp)" bash -c "g++ -std=c++17 -O1 -I room_lab bench/ladder_sim.cpp -o $T/ladder && $T/ladder"
run "bench scripts compile" python3 -m py_compile bench/stress.py bench/gate.py bench/headset_hold.py bench/vt_check.py bench/verdict.py bench/stress/headset_sim.py bench/watchdog.py house/server.py house/migrate.py
mv "$T" "$T.done" 2>/dev/null   # scratch binaries stay in /tmp
[ $fail = 0 ] && echo "all offline tests passed" || echo "OFFLINE TESTS FAILED"
exit $fail
