#!/bin/sh
# LaunchAgent com.pinpin.atlas-watchdog: keep the extra-screen atlas sender running. It is started through
# Terminal (start-atlas.command) so it inherits Terminal's Screen Recording grant.
D="$HOME/.local/share/pinpin-atlas"
pgrep -qx pinpin-atlas || open -g -a Terminal "$D/start-atlas.command"
