#!/bin/sh
# LaunchAgent com.pinpin.sharp-watchdog: keep the sharp-text sender running. It is started through
# Terminal (start.command) so it inherits Terminal's Screen Recording grant.
D="$HOME/.local/share/pinpin-sharp"
pgrep -qf "$D/pinpin-sharp " || open -g -a Terminal "$D/start.command"
