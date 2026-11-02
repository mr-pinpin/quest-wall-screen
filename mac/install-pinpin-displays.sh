#!/bin/sh
# Build PinPin Displays.app (a windowless agent app) and run it as the login agent io.mr-pinpin.displays.
# Source: ~/.local/share/pinpin-displays/src/pinpin-displays.m. Rebuilding changes the signature, and then
# macOS asks for Accessibility again, so rebuild only when the code changes.
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
clang -fobjc-arc -O2 -Wno-deprecated-declarations -framework Foundation -framework AppKit -framework CoreGraphics -framework ApplicationServices \
  "$HOME/.local/share/pinpin-displays/src/pinpin-displays.m" -o "$A/Contents/MacOS/pinpin-displays"
codesign -f -s - "$A"
P=~/Library/LaunchAgents/io.mr-pinpin.displays.plist
cat > $P <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>io.mr-pinpin.displays</string>
<key>ProgramArguments</key><array><string>$A/Contents/MacOS/pinpin-displays</string></array>
<key>RunAtLoad</key><true/><key>KeepAlive</key><true/>
<key>LimitLoadToSessionType</key><string>Aqua</string>
<key>StandardErrorPath</key><string>$HOME/.local/share/pinpin-displays/displays.log</string>
</dict></plist>
PL
launchctl bootout gui/$(id -u)/io.mr-pinpin.displays 2>/dev/null || true
sleep 1
launchctl bootstrap gui/$(id -u) $P
sleep 4
tail -1 ~/.local/share/pinpin-displays/displays.log
curl -s -m 5 http://127.0.0.1:57710/displays; echo
