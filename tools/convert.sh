#!/bin/sh
# Sits in the photos directory; converts every IMG_*.YUV (640x480 packed
# YUV422) that hasn't been converted yet. PNGs land in png/ next to it, so
# already-converted photos are skipped on the next run.
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
