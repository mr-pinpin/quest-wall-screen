#!/bin/bash
# Build, sign and (re)start PinPin Host on this Mac. Run from the directory holding PinPinHost.swift.
# Signed with the permanent "Mr PinPin Code Signing" identity (created once by make-identity.sh), so the
# designated requirement never changes and the permissions granted to the app survive every rebuild.
# Never change the bundle identifier or the signing identity: either would cost the user a new round of clicks.
set -e
cd "$(dirname "$0")"
APP=~/Applications/"PinPin Host.app"
KC=~/Library/Keychains/pinpin-signing.keychain-db
ID=771F0B13AE1367BF714B884FCC7582606FE3175D   # SHA-1 of "Mr PinPin Code Signing"
VERSION=${1:-$(date +%Y.%m.%d.%H%M)}
B=$(mktemp -d)/"PinPin Host.app"
mkdir -p "$B/Contents/MacOS"
swiftc -O -swift-version 5 PinPinHost.swift -o "$B/Contents/MacOS/PinPinHost"
sed "s/__VERSION__/$VERSION/g" Info.plist > "$B/Contents/Info.plist"
security unlock-keychain -p "$(cat ~/.local/share/pinpin-host/signing/keychain-pass)" "$KC"
codesign --force --sign "$ID" --keychain "$KC" --identifier io.mr-pinpin.host "$B"
codesign -d -r- "$B" 2>&1 | grep designated
# Swap the bundle in place, then let launchd restart the host (it replaces any plugin copies left behind).
mkdir -p ~/Applications
rm -rf "$APP.old"; [ -d "$APP" ] && mv "$APP" "$APP.old"
mv "$B" "$APP"; rm -rf "$APP.old"
PL=~/Library/LaunchAgents/io.mr-pinpin.host.plist
cp io.mr-pinpin.host.plist "$PL"
if launchctl print gui/$(id -u)/io.mr-pinpin.host >/dev/null 2>&1; then
  launchctl kickstart -k gui/$(id -u)/io.mr-pinpin.host
else
  launchctl bootstrap gui/$(id -u) "$PL"
fi
sleep 2; curl -s -m3 127.0.0.1:57790/status; echo
