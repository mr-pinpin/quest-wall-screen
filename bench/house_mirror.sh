#!/bin/bash
# Mirror the canonical house (the Mini's house server, house/README.md) into this repo as house/house.json and commit
# when it changed. The Mini keeps the full history in its own git repo (~/pinpin-house); this copy rides along with
# the code. Run on the VPS, e.g. from cron:   bash bench/house_mirror.sh
cd "$(dirname "$0")/.." || exit 1
new=$(ssh -o BatchMode=yes -o ConnectTimeout=10 pinpin-mini 'curl -sf -m10 127.0.0.1:57800/house') || { echo "house server unreachable"; exit 1; }
echo "$new" | python3 -c 'import json,sys; sys.path.insert(0,"house"); import schema; d=json.load(sys.stdin); p=schema.validate(d); print("\n".join(p)) if p else None; sys.exit(1 if p else 0)' || { echo "mirror: invalid house, not copied"; exit 1; }
[ -f house/house.json ] && [ "$(cat house/house.json)" = "$new" ] && { echo "house unchanged"; exit 0; }
printf '%s\n' "$new" > house/house.json
rev=$(echo "$new" | python3 -c 'import json,sys; print(json.load(sys.stdin)["rev"])')
git add house/house.json && git -c user.name="Mr. PinPin" -c user.email=miguelemosreverte@gmail.com commit -q -m "house: mirror rev $rev from the Mini" &&
  git pull -q --rebase --autostash origin main && git push -q origin HEAD:main && echo "mirrored rev $rev"
