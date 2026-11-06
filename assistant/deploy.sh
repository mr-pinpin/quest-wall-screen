#!/bin/sh
# Deploy the PinPin assistant backend to THIS Mac (run over ssh on the Mini or the Air).
# It installs under ~/.local/share/pinpin-assistant, builds the two tiny computer-use helpers, and registers a
# PinPin Host plugin so the service runs as a host child — inheriting Screen Recording + Accessibility + Input
# with NO new permission prompts. (It never rebuilds PinPin Displays, which would re-prompt for Accessibility.)
set -e
D="$HOME/.local/share/pinpin-assistant"
SRC="${1:-$D/src}"            # source dir holding the files from the repo's assistant/
mkdir -p "$D"
# parent_watch.py is a synced copy of receptionist/parent_watch.py (the orphan guard); keep them identical.
for f in pinpin_assistant.py screen_mcp.py parent_watch.py; do cp "$SRC/$f" "$D/$f"; done

# Build the helpers (Swift). Ad-hoc signed; they run as host children so their own signature does not gate TCC.
swiftc -O "$SRC/pinpin-screencap.swift" -o "$D/pinpin-screencap"
swiftc -O "$SRC/pinpin-type.swift"      -o "$D/pinpin-type"

# MCP config the claude CLI loads for computer use. PINPIN_SCREEN is set per-turn by the service and inherited.
cat > "$D/screen-mcp.json" <<JSON
{ "mcpServers": { "screen": { "command": "python3", "args": ["$D/screen_mcp.py"] } } }
JSON

# PinPin Host plugin spec: the host starts it, restarts on exit, re-reads the folder every 3 s.
PL="$HOME/Library/Application Support/PinPin Host/plugins"
mkdir -p "$PL"
CLAUDE="$(command -v claude || echo "$HOME/.local/bin/claude")"
cat > "$PL/assistant.json" <<JSON
{ "exec": "/usr/bin/python3", "args": ["$D/pinpin_assistant.py"],
  "env": { "PINPIN_ASSIST_PORT": "57840", "PATH": "$(dirname "$CLAUDE"):/usr/bin:/bin:/usr/local/bin",
           "PINPIN_ASSIST_MAX_WALL_S": "180", "PINPIN_ASSIST_MAX_STEPS": "20" },
  "cwd": "$D", "log": "$D/assistant.log", "truncateLog": true, "enabled": true, "restartDelay": 1.5 }
JSON
echo "deployed to $D; PinPin Host will start it within ~3 s. Health: curl -s http://127.0.0.1:57840/health"
