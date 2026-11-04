#!/bin/bash
# Captain's room tools. e.g.: room.sh house_info | room.sh move 3 left | room.sh select 3 | room.sh add_card "Title" "details"
exec python3 "$(dirname "$0")/room_tools.py" "$@"
