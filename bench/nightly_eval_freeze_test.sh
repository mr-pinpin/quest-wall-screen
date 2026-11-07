#!/bin/bash
# Offline test: bench/nightly-eval.sh honours the /root/work/.air-freeze demo freeze (work-df, 2026-11-06).
# Frozen  -> exit 0, a NOTE in activity.log, and NOT a single ssh to the laptop.
# Unfrozen -> it proceeds past the guard (reaches the ssh skip probes) and does NOT log the freeze NOTE.
# No hardware: `ssh` is stubbed on PATH so the unfrozen path stops at the Air-load skip instead of reaching out.
set -u
cd "$(dirname "$0")/.."
SCRIPT="$PWD/bench/nightly-eval.sh"
T=$(mktemp -d); fail=0
trap 'rm -rf "$T"' EXIT

# Stub ssh: record that it was called, return empty+0 (so an unfrozen run reaches the "SKIP: Air load=?" branch).
mkdir -p "$T/bin"
cat > "$T/bin/ssh" <<EOF
#!/bin/bash
echo CALLED >> "$T/ssh-called"
exit 0
EOF
chmod +x "$T/bin/ssh"

run_script() {  # \$1 = fake REPO root (gets a bench/ dir); freeze file is \$1/.air-freeze
  mkdir -p "$1/bench"
  PATH="$T/bin:$PATH" REPO="$1" AIR_FREEZE="$1/.air-freeze" timeout 30 bash "$SCRIPT" >/dev/null 2>&1
}
check() { if [ "$2" = "$3" ]; then echo "ok    $1"; else echo "FAIL  $1: expected [$3] got [$2]"; fail=1; fi; }

# --- frozen: skip, NOTE, no ssh ---
R1="$T/r1"; mkdir -p "$R1/bench"
printf 'work-df freeze 2026-11-06: user demoing the headset\n' > "$R1/.air-freeze"
rm -f "$T/ssh-called"
run_script "$R1"; rc=$?
check "frozen exits 0" "$rc" "0"
grep -q "air-freeze" "$R1/bench/activity.log" 2>/dev/null && note=yes || note=no
check "frozen logs the freeze NOTE" "$note" "yes"
grep -q "NOTE" "$R1/bench/activity.log" 2>/dev/null && word=yes || word=no
check "frozen NOTE line is tagged NOTE" "$word" "yes"
[ -e "$T/ssh-called" ] && sshd=yes || sshd=no
check "frozen performs no ssh" "$sshd" "no"

# --- not frozen: proceeds past the guard, no freeze NOTE ---
R2="$T/r2"; mkdir -p "$R2/bench"   # no .air-freeze
rm -f "$T/ssh-called"
run_script "$R2"; rc=$?
check "unfrozen exits 0" "$rc" "0"
if grep -q "air-freeze" "$R2/bench/activity.log" 2>/dev/null; then n2=present; else n2=absent; fi
check "unfrozen does NOT log the freeze NOTE" "$n2" "absent"
[ -e "$T/ssh-called" ] && s2=yes || s2=no
check "unfrozen proceeds to the ssh skip probes" "$s2" "yes"

[ $fail = 0 ] && echo "nightly-eval freeze test passed" || { echo "OFFLINE TEST FAILED"; exit 1; }
