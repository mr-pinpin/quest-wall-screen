#!/bin/bash
# Every test that needs no hardware, in one go. Run before deploying anything to the laptop or the headset
# (AGENTS.md); exit status 0 only if all pass. Needs g++ and python3 (VPS: both present).
#   bash bench/run_offline_tests.sh
cd "$(dirname "$0")/.."
T=$(mktemp -d); fail=0
# every test under a memory and time cap: a runaway test must fail, not OOM-kill the VPS's sessions (2026-11-04)
run() { local name=$1; shift; if out=$( (ulimit -v 4000000; timeout 600 "$@") 2>&1); then echo "ok    $name: $(echo "$out" | tail -1)"; else echo "FAIL  $name"; echo "$out" | tail -20 | sed 's/^/      /'; fail=1; fi; }
LAYOUTS=$(ls bench/results/*layouts*.jsonl 2>/dev/null | tr "\n" " ")
ASSETS=$(ls -d /root/.cache/pinpin-assets/out/DamagedHelmet-* 2>/dev/null | grep -v old | tr "\n" " ")  # converted models, when present
run "budget arbiter (room_lab/budget.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/budget_test.cpp -o $T/budget && $T/budget"
run "VT headset logic + recorded layouts" bash -c "g++ -std=c++17 -O1 -Wall -Wno-misleading-indentation -I room_lab bench/vt_headset_test.cpp -o $T/vt && $T/vt $LAYOUTS"
for f in $LAYOUTS; do run "vt_check recorded $(basename $f)" python3 bench/vt_check.py recorded "$f"; done
run "vt_check synthetic packer model" python3 bench/vt_check.py synthetic --runs 500 --seed 1
run "VT replay: recorded sessions through vt::Resident" bash -c "g++ -std=c++17 -Wall -I room_lab bench/vt_replay.cpp -o $T/replay && $T/replay $LAYOUTS"
run "gate.py self-test" python3 bench/gate_test.py
run "watchdog (fake logs)" python3 bench/watchdog_test.py
run "headset owner guard + install lock" python3 bench/guard_test.py
run "3D models: json, glb, progressive ktx2, asset budget" bash -c "g++ -std=c++17 -Wall -I room_lab bench/model_test.cpp -o $T/model && $T/model $ASSETS"
run "LOD by viewer pose (vt.h projectedPx, pose paths)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/lod_test.cpp -o $T/lod && $T/lod"
run "hero selection (vt.h Heroes)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/hero_test.cpp -o $T/hero && $T/hero"
run "keyframe requests (vt.h KeyReq)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/keyreq_test.cpp -o $T/keyreq && $T/keyreq"
run "settings explanations + AMBI under VT (bench/ui_v044_test.cpp)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/ui_v044_test.cpp -o $T/ui44 && $T/ui44"
run "shoulder menu (room_lab/shoulder.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/shoulder_test.cpp -o $T/shoulder && $T/shoulder"
run "house client vs house/server.py (room_lab/house.h)" bash bench/house_client_test.sh
run "room frame from our anchors (room_lab/anchors.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/anchors_test.cpp -o $T/anch && $T/anch"
run "wave to scroll (room_lab/scroll.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/scroll_test.cpp -o $T/scr && $T/scr"
run "assistant backend (offline model + screen mocks)" python3 bench/assistant_mock_test.py
run "assistant client: SSE, chat, wrapping (room_lab/assist.h)" bash bench/assist_test.sh
run "placements never disappear (room_lab/placement_guard.h, simulated scene timing)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/placement_guard_test.cpp -o $T/pg && $T/pg"
run "main-screen pointer: position before every button (room_lab/pointer_seq.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/pointer_seq_test.cpp -o $T/ps && $T/ps"
run "MULTIROOM: rooms, links, current room (room_lab/rooms.h, simulated anchors)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/rooms_test.cpp -o $T/rooms && $T/rooms"
run "MULTIROOM house round trip, links valid in house/schema.py (room_lab/rooms_house.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/rooms_house_test.cpp -o $T/rh && $T/rh $T/rooms.json && python3 bench/rooms_schema_check.py $T/rooms.json"
run "SCAN never shrinks or moves a house wall (room_lab/wallmerge.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/wallmerge_test.cpp -o $T/wm && $T/wm"
run "laptop discovery (room_lab/hosts.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/hosts_test.cpp -o $T/hosts && $T/hosts"
run "depth wall fitting (room_lab/wallfit.h, synthetic depth)" bash -c "g++ -std=c++17 -O2 -Wall -I room_lab bench/wallfit_test.cpp -o $T/wallfit && $T/wallfit"
run "depth wall fitting on a real Quest depth frame (both eyes agree on the wall)" bash -c "g++ -std=c++17 -O2 -Wall -I room_lab bench/wallfit_replay.cpp -o $T/wfreplay && $T/wfreplay house/fixtures/depth-47041720"
run "progressive room mesh (room_lab/roommesh.h): synthetic room + real depth dump" bash -c "g++ -std=c++17 -O2 -Wall -I room_lab bench/roommesh_test.cpp -o $T/roommesh && (ulimit -v 1500000; timeout 120 $T/roommesh house/fixtures/depth-47041720)"
run "scan meshes before/after: raw depth surface + plane-regularised room (rawsurface.h, roomplanes.h)" bash -c "g++ -std=c++17 -O2 -Wall -I room_lab -I bench bench/scanmesh_test.cpp -o $T/scanmesh && (ulimit -v 1500000; timeout 120 $T/scanmesh house/fixtures/depth-47041720)"
run "wall tools (room_lab/wallshape.h) + shapes valid in house/schema.py" bash -c "g++ -std=c++17 -O1 -Wall -I room_lab bench/wallshape_test.cpp -o $T/wallshape && (ulimit -v 1500000; timeout 60 $T/wallshape $T/shapes.json) && python3 house/check_fragment.py $T/shapes.json"
run "flag registry matches the code (room_lab/flags.json, bench/flags_check.py)" python3 bench/flags_check.py
run "flag registry loader (room_lab/flags.h)" bash -c "g++ -std=c++17 -Wall -I room_lab bench/flags_test.cpp -o $T/flags && $T/flags room_lab/flags.json"
run "house file: schema, migration, sync server (house/test_house.py)" python3 house/test_house.py
run "bench lock modes: shared / exclusive, writer priority, expiry (bench/benchlock_test.py)" python3 bench/benchlock_test.py
run "verdict.py self-test (pass/drop rules)" python3 bench/verdict.py --self-test
run "video LOD ladder along pose paths (bench/ladder_sim.cpp)" bash -c "g++ -std=c++17 -O1 -I room_lab bench/ladder_sim.cpp -o $T/ladder && $T/ladder"
run "bench scripts compile" python3 -m py_compile bench/stress.py bench/gate.py bench/headset_hold.py bench/vt_check.py bench/verdict.py bench/stress/headset_sim.py bench/watchdog.py bench/campaign.py bench/benchlock.py bench/placement_regress.py bench/headset_awake.py house/server.py house/migrate.py assistant/pinpin_assistant.py assistant/screen_mcp.py
mv "$T" "$T.done" 2>/dev/null   # scratch binaries stay in /tmp
[ $fail = 0 ] && echo "all offline tests passed" || echo "OFFLINE TESTS FAILED"
# The verdict for install scripts (stress.require_green_gate): result, the commit tested, when.
V="$([ $fail = 0 ] && echo PASS || echo FAIL) $(git rev-parse HEAD 2>/dev/null) $(date -u +%s)"
echo "$V" > /tmp/pinpin-offline-gate.txt 2>/dev/null; chmod a+rw /tmp/pinpin-offline-gate.txt 2>/dev/null   # the latest run, any agent
mkdir -p /tmp/pinpin-offline-gate 2>/dev/null; chmod a+rwx /tmp/pinpin-offline-gate 2>/dev/null
echo "$V" > "/tmp/pinpin-offline-gate/$(git rev-parse HEAD 2>/dev/null)" 2>/dev/null   # per commit: agents never overwrite each other's verdict
exit $fail
