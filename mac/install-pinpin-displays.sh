#!/bin/sh
# Build the PinPin Displays binary. PinPin Host is the SOLE owner of pinpin-displays (the plugin
# model the user asked for): it runs it as a child plugin, so the grant (Accessibility) is charged
# to PinPin Host's PERMANENT identity and survives rebuilds — no re-prompt. See AGENTS.md.
#
# This script therefore ONLY builds the binary and retires any stale standalone launchd agent. It
# NEVER creates/bootstraps io.mr-pinpin.displays and NEVER `open`s the app — doing so adds a second
# (and third) owner fighting over :57710, which crash-loops and churns the displays (incident
# 2026-11-04). To pick up a new binary, replace it on disk and let PinPin Host restart the plugin.
set -e
A="$HOME/Applications/PinPin Displays.app"
mkdir -p "$A/Contents/MacOS"
cat > "$A/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>io.mr-pinpin.displays</string>
<key>CFBundleName</key><string>PinPin Displays</string>
<key>CFBundleExecutable</key><string>pinpin-displays</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>LSUIElement</key><true/>
</dict></plist>
PL
clang -fobjc-arc -O2 -Wno-deprecated-declarations -framework Foundation -framework AppKit -framework CoreGraphics -framework ApplicationServices -framework ImageIO \
  "$HOME/.local/share/pinpin-displays/src/pinpin-displays.m" -o "$A/Contents/MacOS/pinpin-displays"
codesign -f -s - "$A"

# Retire any stale standalone LaunchAgent (superseded by the Host plugin, 06147ca). Keeping it would
# make a second owner of :57710.
launchctl bootout gui/$(id -u)/io.mr-pinpin.displays 2>/dev/null || true
launchctl disable gui/$(id -u)/io.mr-pinpin.displays 2>/dev/null || true
P=~/Library/LaunchAgents/io.mr-pinpin.displays.plist
[ -f "$P" ] && mv "$P" "$P.retired-$(date +%s)" || true

# Pick up the new binary through the SINGLE owner, PinPin Host (clean restart, host backoff applies).
# The Host restarts a plugin from the binary on disk after any exit; its control server does it cleanly.
if curl -s -m5 -X POST "http://127.0.0.1:57790/restart?name=displays" >/dev/null 2>&1; then
  echo "asked PinPin Host to restart the displays plugin"
else
  echo "NOTE: could not reach the Host control (:57790). Confirm the displays plugin name in"
  echo "      ~/Library/Application Support/PinPin Host/plugins/ and POST /restart?name=<it>,"
  echo "      or pkill the pinpin-displays child so the Host respawns it from the new binary."
fi
sleep 3
curl -s -m5 http://127.0.0.1:57710/displays; echo
