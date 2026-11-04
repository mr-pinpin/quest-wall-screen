#!/bin/bash
# Launch the VPS receptionist service. Runs as pinpin-claude.
# Production: no block-list (the captain session must be reachable by the user).
# Testing: set PINPIN_RECEPT_BLOCK="work-df" to make the captain structurally unreachable.
set -u
cd "$(dirname "$0")"
exec python3 ./pinpin_receptionist.py
