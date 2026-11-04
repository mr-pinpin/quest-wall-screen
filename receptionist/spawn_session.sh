#!/bin/bash
# spawn_session.sh "<topic>" "<brief>"
# Start a new Claude Code session the same way the existing ones run (root tmux, runuser
# pinpin-claude, --dangerously-skip-permissions, --session-id). Prints "<name> <session-id>"
# once the session registers, so work-df can hand off a topic and later relay its replies.
set -u
TOPIC="${1:-New topic}"
BRIEF="${2:-$TOPIC}"
cd "$(dirname "$0")"

SID=$(BRIEF="$BRIEF" python3 -c 'import os,recept_core as rc;print(rc.spawn_engineering(os.environ["BRIEF"]))')
[ -n "$SID" ] || { echo "spawn failed" >&2; exit 1; }

for _ in $(seq 1 25); do
  sleep 1
  NAME=$(SID="$SID" python3 -c 'import os,recept_core as rc;s=rc.resolve(os.environ["SID"]);print(s["name"] if s else "")')
  [ -n "$NAME" ] && { echo "$NAME $SID"; exit 0; }
done
echo "? $SID"   # registered slowly; id is valid, name not yet derived
