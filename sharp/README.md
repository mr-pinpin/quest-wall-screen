# pinpin-sharp: lossless refinement sender

See ../docs/sharp-text.md for the design. Install on a Mac (Swift CLT required):

```
D=~/.local/share/pinpin-sharp; mkdir -p $D
swiftc -O -swift-version 5 SharpSender.swift -o $D/pinpin-sharp
cp start.command watchdog.sh $D/ && chmod +x $D/start.command $D/watchdog.sh
echo "--width 2560 --height 1600 --display 0" > $D/args   # the Moonlight stream size of this Mac
cp com.pinpin.sharp-watchdog.plist ~/Library/LaunchAgents/  # edit the user path inside
launchctl load -w ~/Library/LaunchAgents/com.pinpin.sharp-watchdog.plist
```

The sender runs through Terminal (start.command) so it inherits Terminal's Screen Recording
grant: no new permission prompt, and rebuilds need no re-approval. The watchdog relaunches it
every 30 s if it is not running; the sender itself restarts capture after display sleep/lock and
exits if a capture start hangs, so the watchdog starts a fresh one.

Check it: `python3 sharp_probe.py HOST --seconds 10 --png /tmp/refined.png`.
The headset reads `files/sharp.json`: `{"host": "<Mac IP>", "port": 57700, "source": "LAPTOP"}`.
