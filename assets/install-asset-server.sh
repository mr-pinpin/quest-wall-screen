#!/bin/sh
# Install the headset asset server on the Mini as a LaunchAgent (starts at login, restarts if it exits).
#   ssh pinpin-mini 'sh -s' < assets/install-asset-server.sh   (with the plist piped alongside, see below)
# Expects io.mr-pinpin.assets.plist in the current directory. Replaces a nohup'd http.server on :8099 if any.
set -e
P=~/Library/LaunchAgents/io.mr-pinpin.assets.plist
mkdir -p ~/pinpin-assets
cp io.mr-pinpin.assets.plist "$P"
pkill -f "http.server 8099" 2>/dev/null || true
launchctl bootout gui/$(id -u)/io.mr-pinpin.assets 2>/dev/null || true
sleep 1
launchctl bootstrap gui/$(id -u) "$P"
sleep 2
curl -sI -m5 http://127.0.0.1:8099/ | head -1
