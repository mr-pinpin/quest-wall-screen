#!/bin/sh
# Build and install pinpin-displays on this Mac as a login agent (runs in the GUI session).
# Usage: copy pinpin-displays.m to ~/.local/share/pinpin-displays/src/, then run this script.
set -e
mkdir -p ~/.local/share/pinpin-displays
cd ~/.local/share/pinpin-displays
clang -fobjc-arc -O2 -Wno-deprecated-declarations -framework Foundation -framework CoreGraphics -framework ApplicationServices src/pinpin-displays.m -o pinpin-displays
codesign -f -s - -i io.mr-pinpin.displays pinpin-displays
P=~/Library/LaunchAgents/io.mr-pinpin.displays.plist
cat > $P <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>io.mr-pinpin.displays</string>
<key>ProgramArguments</key><array><string>$HOME/.local/share/pinpin-displays/pinpin-displays</string></array>
<key>RunAtLoad</key><true/><key>KeepAlive</key><true/>
<key>LimitLoadToSessionType</key><string>Aqua</string>
<key>StandardErrorPath</key><string>$HOME/.local/share/pinpin-displays/displays.log</string>
</dict></plist>
PL
launchctl bootout gui/$(id -u)/io.mr-pinpin.displays 2>/dev/null
launchctl bootstrap gui/$(id -u) $P && echo bootstrapped
sleep 2; launchctl list | grep mr-pinpin.displays; tail -2 ~/.local/share/pinpin-displays/displays.log
curl -s -m 10 http://127.0.0.1:57710/displays; echo
