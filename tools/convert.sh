#!/bin/sh
# Sits in the photos directory; converts every new IMG_*.YUV (640x480 packed
# YUV422) to png/ and every new VID_*.VID (dsi-camera video container) to
# vid/ as mp4. Already-converted files are skipped on the next run.
set -e
cd "$(dirname "$0")"

mkdir -p png
new=0
for f in IMG_*.YUV; do
	[ -f "$f" ] || continue
	png="png/${f%.*}.png"
	[ -f "$png" ] && continue
	ffmpeg -loglevel error -n -f rawvideo -pix_fmt yuyv422 -s 640x480 -i "$f" "$png"
	echo "$f -> $png"
	new=$((new + 1))
done
echo "$new new photo(s) converted."

mkdir -p vid
new=0
for f in VID_*.VID; do
	[ -f "$f" ] || continue
	mp4="vid/${f%.*}.mp4"
	[ -f "$mp4" ] && continue
	python3 - "$f" "$mp4" <<'EOF'
import struct, subprocess, sys, os, tempfile

src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
magic, w, h, fps, den, rate, fmt, frames_total, samp_total = struct.unpack('<8s8I', d[:40])
assert magic == b'DSIVID01', 'not a dsi-camera video'

# Walk chunks; duplicate the previous frame over index gaps (dropped frames).
off, frames, pcm, lv = 40, [], b'', -1
while off + 12 <= len(d):
    t, idx, sz = struct.unpack('<3I', d[off:off+12])
    off += 12
    if t == 1:
        if frames and idx > lv + 1:
            frames.extend([frames[-1]] * (idx - lv - 1))
        lv = idx
        frames.append(d[off:off+sz])
    elif t == 2:
        pcm += d[off:off+sz]
    off += sz

tmp = tempfile.mkdtemp()
v = os.path.join(tmp, 'v.rgb')
a = os.path.join(tmp, 'a.pcm')
open(v, 'wb').write(b''.join(frames))
open(a, 'wb').write(pcm)
subprocess.run([
    'ffmpeg', '-loglevel', 'error', '-y',
    '-f', 'rawvideo', '-pix_fmt', 'bgr555', '-s', f'{w}x{h}', '-r', str(fps), '-i', v,
    '-f', 's16le', '-ar', str(rate), '-ac', '1', '-i', a,
    '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-c:a', 'aac', dst,
], check=True)
os.remove(v); os.remove(a); os.rmdir(tmp)
EOF
	echo "$f -> $mp4"
	new=$((new + 1))
done
echo "$new new video(s) converted."
