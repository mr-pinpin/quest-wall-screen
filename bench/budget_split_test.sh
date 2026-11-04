#!/bin/bash
# The sender's budget split (atlas/AtlasSender.swift, between "BEGIN budgetSplit" and "END budgetSplit") compiled alone
# with a test main on the Mini (swiftc; nothing touches the Air): heroes keep their share, atlas + heroes stay within
# 42 Mbit/s while hero streams run, and nothing changes without heroes.   bash bench/budget_split_test.sh
cd "$(dirname "$0")/.."
{ echo 'enum Atlas {'
  sed -n '/BEGIN budgetSplit/,/END budgetSplit/p' atlas/AtlasSender.swift
  echo '}'
  cat <<'SW'
var fails = 0
func check(_ c: Bool, _ m: String) { if !c { fails += 1; print("FAIL", m) } }
func cinema(_ heroPri: Double = 1.0) -> [Int: (vis: Bool, pri: Double)] {
    var w: [Int: (vis: Bool, pri: Double)] = [2: (true, heroPri), 3: (true, 0.1), 11: (true, 0.1)]
    for i in 4...10 { w[i] = (false, 0) }
    return w
}
func total(_ r: (atlas: Int, heroes: [Int: Int])) -> Int { r.atlas + r.heroes.values.reduce(0, +) }
// the stand-in's cinema demand (budget 60 Mbit/s): hero 30, atlas the neighbours' share, total 40 (block A measured 40.2)
var r = Atlas.budgetSplit(budgetKbps: 60000, maxKbps: 40000, capKbps: 42000, want: cinema(), heroes: [2])
check(r.heroes[2] == 30000 && r.atlas == 10000, "stand-in cinema: atlas \(r.atlas) hero \(r.heroes)")
// a generous headset budget: the cap holds, the hero keeps its share, the atlas takes the cut
r = Atlas.budgetSplit(budgetKbps: 120000, maxKbps: 40000, capKbps: 42000, want: cinema(), heroes: [2])
check(total(r) <= 42000 && r.heroes[2] == 30000 && r.atlas == 12000, "budget 120: atlas \(r.atlas) hero \(r.heroes)")
// no budget in demand: hero 12, atlas the rest of 42
r = Atlas.budgetSplit(budgetKbps: 0, maxKbps: 40000, capKbps: 42000, want: cinema(), heroes: [2])
check(total(r) == 42000 && r.heroes[2] == 12000 && r.atlas == 30000, "no budget: atlas \(r.atlas) hero \(r.heroes)")
// two heroes that alone would exceed the cap: scaled, the atlas keeps 4 Mbit/s
var w2 = cinema(); w2[3] = (true, 1.0)
r = Atlas.budgetSplit(budgetKbps: 200000, maxKbps: 40000, capKbps: 42000, want: w2, heroes: [2, 3])
check(total(r) <= 42000 && r.atlas >= 4000 && r.heroes.count == 2, "two heroes: atlas \(r.atlas) heroes \(r.heroes)")
// without heroes nothing changes: the atlas gets the extras' share, up to the encoder's max, no 42 cap
r = Atlas.budgetSplit(budgetKbps: 60000, maxKbps: 40000, capKbps: 42000, want: cinema(), heroes: [])
check(r.atlas == 40000 && r.heroes.isEmpty, "no heroes: atlas \(r.atlas)")
// random demands: with heroes the total never exceeds the cap, the atlas never under 1.5 Mbit/s
var rng = SystemRandomNumberGenerator(); var over = 0
for _ in 0..<2000 {
    var w: [Int: (vis: Bool, pri: Double)] = [:]
    for id in 1...12 { w[id] = (Bool.random(using: &rng), Double.random(in: 0...1, using: &rng)) }
    let hs = Set((2...12).filter { _ in Int.random(in: 0..<5, using: &rng) == 0 })
    let res = Atlas.budgetSplit(budgetKbps: [0, 30000, 60000, 150000].randomElement(using: &rng)!, maxKbps: 40000, capKbps: 42000, want: w, heroes: hs)
    if !hs.isEmpty && total(res) > 42000 || res.atlas < 1500 { over += 1 }
}
check(over == 0, "\(over) of 2000 random demands broke the cap")
print("budget split: \(fails) failures"); if fails > 0 { fatalError("budget split failed") }
SW
} > /tmp/budget_split_test.swift
scp -q -o BatchMode=yes /tmp/budget_split_test.swift pinpin-mini:/tmp/budget_split_test.swift && \
ssh -o BatchMode=yes pinpin-mini 'cd /tmp && swiftc -O budget_split_test.swift -o budget_split_test 2>&1 | grep -v warning | head -20 && ./budget_split_test'
