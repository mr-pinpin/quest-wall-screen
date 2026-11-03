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
OLD="$APP.old.$(date +%s)"   # moved aside, not deleted (no rm prompts on this Mac)
[ -d "$APP" ] && mv "$APP" "$OLD"
mv "$B" "$APP"
# Plugins keep running across this restart: the new host adopts them (see "Host restarts" in PinPinHost.swift).
PL=~/Library/LaunchAgents/io.mr-pinpin.host.plist
J=gui/$(id -u)/io.mr-pinpin.host
if ! launchctl print $J >/dev/null 2>&1; then
  cp io.mr-pinpin.host.plist "$PL"; launchctl bootstrap gui/$(id -u) "$PL"
elif cmp -s io.mr-pinpin.host.plist "$PL"; then
  launchctl kickstart -k $J
else  # launchd reads the plist only at bootstrap
  launchctl bootout $J; sleep 1; cp io.mr-pinpin.host.plist "$PL"; launchctl bootstrap gui/$(id -u) "$PL"
fi
sleep 3; curl -s -m3 127.0.0.1:57790/status; echo
