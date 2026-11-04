#!/bin/bash
# Install or update the house sync server on the Mini (run from the VPS):   bash house/install-house-server.sh
# Copies house/*.py to ~/pinpin-house-server, (re)loads the LaunchAgent, and checks /health. The data
# (~/pinpin-house, a git repo) is never touched by an install.
set -e
cd "$(dirname "$0")"
M="ssh -o BatchMode=yes -o ConnectTimeout=10 pinpin-mini"
$M 'mkdir -p ~/pinpin-house-server'
scp -q -o BatchMode=yes schema.py server.py migrate.py floorplan.py pinpin-mini:pinpin-house-server/
scp -q -o BatchMode=yes io.mr-pinpin.house.plist pinpin-mini:Library/LaunchAgents/
$M 'launchctl bootout gui/$(id -u)/io.mr-pinpin.house 2>/dev/null; sleep 1; launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/io.mr-pinpin.house.plist'
for i in 1 2 3 4 5 6 7 8 9 10; do
  h=$($M 'curl -sf -m3 127.0.0.1:57800/health' 2>/dev/null) && { echo "house server up: $h"; exit 0; }
  sleep 1
done
echo "house server did not come up:"; $M 'tail -20 /tmp/pinpin-house.log'; exit 1
