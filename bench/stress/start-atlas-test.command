#!/bin/sh
# Test copy of the atlas sender for bench/stress.py --sim: its own binary name (so the live sender's pkill and
# watchdog never touch it) and ports 57730/57731, so the headset's live stream on 57720/57721 is untouched.
# Started through Terminal for its Screen Recording grant; fully detached; stopped with pkill -x pinpin-atlas-test.
D="$HOME/.local/share/pinpin-atlas"
cp -f "$D/pinpin-atlas" "$D/pinpin-atlas-test"
pkill -x pinpin-atlas-test 2>/dev/null
nohup "$D/pinpin-atlas-test" --port 57730 --demand-port 57731 > "$D/atlas-sim.log" 2>&1 < /dev/null &
disown
( sleep 1; osascript -e 'tell application "Terminal" to close (every window whose name contains "start-atlas-test.command")' ) > /dev/null 2>&1 &
exit 0
