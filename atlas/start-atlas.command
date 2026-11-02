#!/bin/sh
# Started through Terminal so the atlas sender inherits Terminal's Screen Recording grant; fully detached.
D="$HOME/.local/share/pinpin-atlas"
ARGS=$(cat "$D/args" 2>/dev/null)
pkill -x pinpin-atlas 2>/dev/null
nohup "$D/pinpin-atlas" $ARGS > "$D/atlas.log" 2>&1 < /dev/null &
disown
( sleep 1; osascript -e 'tell application "Terminal" to close (every window whose name contains "start-atlas.command")' ) > /dev/null 2>&1 &
exit 0
