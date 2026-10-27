#!/bin/sh
# Sits in the photos directory; converts every new IMG_*.YUV (640x480 packed
# YUV422) to png/ and every new VID_*.VID (dsi-camera video container) to
# vid/ as mp4. Output files inherit the source capture timestamp.
# Already-converted files are skipped on the next run.
set -e
cd "$(dirname "$0")"

preserve_timestamp() {
	src=$1
	dst=$2
	touch -r "$src" "$dst"
	if command -v SetFile >/dev/null 2>&1; then
		stamp=$(stat -f '%Sm' -t '%m/%d/%Y %H:%M:%S' "$src")
		SetFile -d "$stamp" -m "$stamp" "$dst"
	fi
}

mkdir -p png
new=0
for f in IMG_*.YUV; do
	[ -f "$f" ] || continue
	[ -s "$f" ] || {
		echo "$f: empty, skipping"
		continue
	}
	png="png/${f%.*}.png"
	[ -f "$png" ] && continue
	ffmpeg -loglevel error -n -f rawvideo -pix_fmt yuyv422 -s 640x480 -i "$f" "$png"
	preserve_timestamp "$f" "$png"
	echo "$f -> $png"
	new=$((new + 1))
done
echo "$new new photo(s) converted."

mkdir -p vid
new=0
for f in VID_*.VID; do
	[ -f "$f" ] || continue
	[ -s "$f" ] || {
		echo "$f: empty, skipping"
		continue
	}
	mp4="vid/${f%.*}.mp4"
	[ -f "$mp4" ] && continue
	if python3 - "$f" "$mp4" <<'EOF'
import datetime, math, struct, subprocess, sys, os, tempfile

src, dst = sys.argv[1], sys.argv[2]
st = os.stat(src)
d = open(src, 'rb').read()
MAGICS = (b'DSIVID01', b'DSIVID02')
if len(d) < 40 or d[:8] not in MAGICS:
    print(f'{src}: empty or not a dsi-camera video, skipping')
    sys.exit(0)
timed = d[:8] == b'DSIVID02'  # chunk index is a capture time in ms
w, h, fps, den, rate, fmt, frames_total, samp_total = struct.unpack('<8I', d[8:40])
duration_ms = den if timed and den > 1 else 0
fps = fps or 10

# Walk chunks, then lay frames out on a constant-rate timeline: DSIVID02 chunks
# carry the real capture time (so a slow or jittery capture still plays at the
# right speed), DSIVID01 chunks carry a frame index.
off, frames, stamps, pcm = 40, [], [], b''
while off + 12 <= len(d):
    t, idx, sz = struct.unpack('<3I', d[off:off+12])
    off += 12
    if t == 1:
        chunk = d[off:off + sz]
        if len(chunk) == w * h * 2:  # fixed-size frames; anything else is bad
            frames.append(chunk)
            stamps.append(idx)
    elif t == 2:
        pcm += d[off:off+sz]
    off += sz

if not frames:
    print(f'{src}: no video frames, skipping')
    sys.exit(1)

if timed and len(stamps) > 1 and stamps[-1] > stamps[0] + 50:
    # Real rate of this clip, from its own timestamps. Clamped so a two-frame
    # clip can't produce a nonsense rate.
    fps = max(1.0, min(60.0, (len(stamps) - 1) * 1000.0 / (stamps[-1] - stamps[0])))

slots = [round(s * fps / 1000.0) for s in stamps] if timed else stamps
out, lv = [], None
for slot, fr in zip(slots, frames):
    if lv is None:
        # audio starts before the first frame; hold that frame so the two
        # tracks stay lined up (clamped: a corrupt stamp shouldn't eat ram)
        out.extend([fr] * min(max(0, slot), 600))
    elif slot <= lv:
        continue  # two frames in the same slot: keep the first
    else:
        out.extend([out[-1]] * min(slot - lv - 1, 600))  # hold the last frame over a gap
    lv = slot
    out.append(fr)
# Hold the last frame through stop, including a stalled camera at the end.
end_slot = math.ceil(duration_ms * fps / 1000.0)
out.extend([out[-1]] * min(max(0, end_slot - len(out)), 600))
frames = out

with tempfile.TemporaryDirectory() as tmp:
    v = os.path.join(tmp, 'v.rgb')
    open(v, 'wb').write(b''.join(frames))
    cmd = [
        'ffmpeg', '-loglevel', 'error', '-y',
        '-f', 'rawvideo', '-pix_fmt', 'bgr555', '-s', f'{w}x{h}', '-r', f'{fps:.3f}', '-i', v,
    ]
    if pcm:
        a = os.path.join(tmp, 'a.pcm')
        open(a, 'wb').write(pcm)
        cmd += ['-f', 's16le', '-ar', str(rate), '-ac', '1', '-i', a]
    cmd += [
        '-metadata', 'creation_time=' + datetime.datetime.fromtimestamp(
            st.st_mtime, datetime.timezone.utc).isoformat(timespec='seconds').replace('+00:00', 'Z'),
        '-c:v', 'libx264', '-pix_fmt', 'yuv420p',
    ]
    if pcm:
        cmd += ['-c:a', 'aac']
    cmd.append(dst)
    subprocess.run(cmd, check=True)
EOF
	then
		[ ! -f "$mp4" ] || preserve_timestamp "$f" "$mp4"
		echo "$f -> $mp4"
		new=$((new + 1))
	fi
done
echo "$new new video(s) converted."
