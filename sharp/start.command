#!/bin/sh
# Started through Terminal so the sender inherits Terminal's Screen Recording grant
# (no extra permission prompt). The sender is fully detached, and this window closes itself.
D="$HOME/.local/share/pinpin-sharp"
ARGS=$(cat "$D/args" 2>/dev/null)
pkill -f "$D/pinpin-sharp " 2>/dev/null
nohup "$D/pinpin-sharp" $ARGS > "$D/sharp.log" 2>&1 < /dev/null &
disown
( sleep 1; osascript -e 'tell application "Terminal" to close (every window whose name contains "start.command")' ) > /dev/null 2>&1 &
exit 0
