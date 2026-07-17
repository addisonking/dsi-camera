# Camera

A homebrew camera app for the Nintendo DSi. Fork of [Pk11's dsi-camera](https://github.com/Epicpkmn11/dsi-camera).

## Features

- Photos: raw 640x480 YUV from either camera, burst capture while held
- Video: 256x192 @ 10fps with microphone audio, recorded to a simple chunked container
- Album: date-grouped thumbnail gallery with full-screen viewing, video playback with sound, and delete
- Export photos to the stock DSi Camera album (signed, shows up like a real photo)
- Power LED turns red while capturing
- `tools/convert.sh` converts photos to PNG and videos to MP4 on a PC (ffmpeg + python3)

## Controls

| Key | Action |
| --- | --- |
| L/R | Take photos / start & stop recording |
| Y | Toggle photo / video mode |
| A | Swap camera |
| SELECT | Album |
| START | Exit |

## Building

Requires [devkitPro](https://devkitpro.org) with devkitARM, libnds, and calico.

```
make
```

Copy `dsi-camera.nds` to your SD card and launch it (tested with Unlaunch).
Photos and videos are saved to `sd:/photos/`; drop `tools/convert.sh` in that
folder and run it on a PC to get PNGs and MP4s.

## Credits

- [Pk11](https://github.com/Epicpkmn11) for the original app
- [Arisotura](http://kuribo64.net) for the camera init research
- [devkitPro](https://github.com/devkitPro) for devkitARM, libnds, and calico
- [nocash](https://problemkaputt.de) for GBATEK
