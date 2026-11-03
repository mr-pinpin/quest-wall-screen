# Cached video LODs prototype (docs/sources.md roadmap 4): one clip pre-encoded at 3 rungs with aligned 1 s closed
# GOPs, cut into Annex B segments; a switcher follows a viewer path and concatenates rung segments; ffmpeg must
# decode the result cleanly across every resolution change. Runs on the Mini (ffmpeg + libx264).
set -e
D=~/pinpin-ladder; rm -rf $D; mkdir -p $D; cd $D
ffmpeg -v error -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 60 -pix_fmt yuv420p -c:v rawvideo -f nut src.nut
for r in "0 1920 1080 8000k" "1 960 540 2500k" "2 480 270 800k"; do set -- $r
  ffmpeg -v error -i src.nut -vf scale=$2:$3 -c:v libx264 -preset veryfast -tune zerolatency -b:v $4 -maxrate $4 -bufsize $4 \
    -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 -x264-params "repeat-headers=1:open-gop=0:scenecut=0" -force_key_frames "expr:gte(t,n_forced*1)" \
    -bsf:v h264_mp4toannexb -f segment -segment_time 1 -segment_format h264 -break_non_keyframes 0 "r$1_%03d.h264"
  echo "rung $1 ${2}x$3: $(ls r$1_*.h264 | wc -l | tr -d ' ') segments, $(du -ck r$1_*.h264 | tail -1 | cut -f1) KB"
done
python3 - <<'PY'
import math, os, glob
rungs = [(0, 1920), (1, 960), (2, 480)]   # rung, width
segs = min(len(glob.glob(f"r{r}_*.h264")) for r, _ in rungs)
def px_at(t):  # a 1.6 m wide screen: approach 6 m -> 1 m (0-20 s), orbit at 1 m (20-40 s), walk away to 6 m (40-60 s)
    d = 6 - 5 * t / 20 if t < 20 else 1.0 if t < 40 else 1 + 5 * (t - 40) / 20
    return int(2 * math.atan(0.8 / d) * 1260)
cur, down_since, sched, switches = 2, None, [], 0
for s in range(segs):
    want = max([r for r, w in rungs if w >= px_at(s + 0.5)] or [0])   # the coarsest rung still >= the projected px
    if want < cur: cur, down_since = want, None; switches += 1         # more detail: at the next segment
    elif want > cur:                                                    # less detail: only after 3 s wanting it
        down_since = s if down_since is None else down_since
        if s - down_since >= 3: cur, down_since = want, None; switches += 1
    else: down_since = None
    sched.append(cur)
with open("out.h264", "wb") as o:
    for s, r in enumerate(sched): o.write(open(f"r{r}_{s:03d}.h264", "rb").read())
fixed = sum(os.path.getsize(f"r0_{s:03d}.h264") for s in range(segs))
print(f"schedule: {''.join(str(r) for r in sched)}")
print(f"switches {switches} in {segs} s ({switches * 60 / segs:.1f}/min); bytes {os.path.getsize('out.h264')//1024} KB vs {fixed//1024} KB always-full ({100*os.path.getsize('out.h264')//fixed}%)")
PY
errs=$(ffmpeg -v error -f h264 -i out.h264 -f null - 2>&1 | wc -l | tr -d ' ')
frames=$(ffprobe -v error -f h264 -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 out.h264)
sizes=$(ffprobe -v error -f h264 -show_entries frame=width -of csv=p=0 out.h264 | uniq -c | awk '{printf "%sx%s ", $2, $1}')
echo "decode: $errs error lines, $frames frames (expect 3600); width runs: $sizes"
