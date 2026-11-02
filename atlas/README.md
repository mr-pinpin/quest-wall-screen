# pinpin-atlas: extra screens as one stream

Each summoned screen in the headset is a macOS virtual display on the laptop (mac/pinpin-displays,
HTTP :57710). `pinpin-atlas` polls `GET :57710/displays`, captures every listed display with
ScreenCaptureKit, packs them into one atlas frame (cells fit 1920x1200, rows wrap at 3840) and serves it
as hardware H.264 on TCP :57720 in MacScreenSender's packet format. Before every keyframe it sends the
layout as a text packet: `{"atlas":[W,H],"cells":[{"screen":id,"x","y","w","h"}]}`. A still atlas is
re-sent every 200 ms (decoders hold the first frame until the next).

Headset: `files/atlas.json` = `{"host": "<laptop IP>", "port": 57720}`. A second VideoStream decodes
the atlas (direct TCP mode); each live screen copies its cell into its own PanelLayer (compositor quad
with the panel's QUAD settings) and gets the AMBI bezel coloured from its cell.

Install on the laptop (Swift CLT):
```
D=~/.local/share/pinpin-atlas; mkdir -p $D
swiftc -O -swift-version 5 AtlasSender.swift -o $D/pinpin-atlas
cp start-atlas.command watchdog-atlas.sh $D/ && chmod +x $D/*.command $D/*.sh
cp com.pinpin.atlas-watchdog.plist ~/Library/LaunchAgents/ && launchctl load -w ~/Library/LaunchAgents/com.pinpin.atlas-watchdog.plist
```
Testing without virtual displays: `echo "--test 2:main,3:main" > $D/args` streams the main display as screens 2 and 3.
