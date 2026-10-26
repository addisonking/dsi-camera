# Camera

A homebrew camera app for the Nintendo DSi. Fork of [Pk11's dsi-camera](https://github.com/Epicpkmn11/dsi-camera).

## Features

- Photos: raw 640x480 YUV from either camera, burst capture while held
- Advanced photo controls: real sensor integration (shutter) and analogue gain
- Night burst: four full-resolution frames aligned and merged on the DSi into one photo
- Rear camera light toggle; always off during Night Burst captures
- Video: 256x192 with microphone audio at up to 10fps, recorded to a simple chunked container
- Album: date-grouped thumbnail gallery with full-screen viewing, video playback with sound, and delete
- Optional timestamp watermark burned into photos and video frames as they're captured
- Export photos to the stock DSi Camera album (signed, shows up like a real photo)
- Power LED turns red while capturing
- `tools/convert.sh` converts photos to PNG and videos to MP4 on a PC (ffmpeg + python3)

## Controls

| Key | Action |
| --- | --- |
| L/R | Take photos / start & stop recording |
| Y | Toggle photo / video mode |
| A | Swap camera |
| X | Advanced controls |
| SELECT | Album |
| START | Exit |

## Building

Requires [devkitPro](https://devkitpro.org) with devkitARM, libnds, and calico.

```
make
```

The host-side photo index tests can be run without devkitPro using
`./tools/pit/test.sh` (requires a C compiler).

Copy `dsi-camera.nds` to your SD card and launch it (tested with Unlaunch).
Photos and videos are saved to `sd:/photos/`; drop `tools/convert.sh` in that
folder and run it on a PC to get PNGs and MP4s. Photo controls and the
timestamp watermark persist in `sd:/photos/settings.cfg`.

The watermark is off by default. When on, the capture date and time are drawn
into the pixels themselves — bottom right, white with a black outline — so they
survive export to the DSi album and conversion on a PC. It's applied at capture
time only; existing photos are never modified, and the live viewfinder stays
clean.

## Credits

- [Pk11](https://github.com/Epicpkmn11) for the original app
- [Arisotura](http://kuribo64.net) for the camera init research
- [devkitPro](https://github.com/devkitPro) for devkitARM, libnds, and calico
- [nocash](https://problemkaputt.de) for GBATEK

## Advanced controls

Press **X**. Use up/down to select a row and left/right to adjust,
or tap the arrows beside a value. Press **A** on Shutter to return to Auto,
on Gain, Night Burst, or Timestamp to cycle/toggle, or on Reset to restore all
settings. Press **B** or **X** to return to the viewfinder. The top screen stays
live while adjusting. Gain is gray and cannot be selected until Shutter is manual.
In photo mode, L/R takes photos directly from Advanced without closing it;
the save confirmation appears below the controls. Hold L/R for regular burst
capture, or tap once for a Night Burst photo.
In video mode, the photo controls are disabled; Timestamp and Reset remain available.
Rear Light is available whenever the outer camera is selected.

- **Shutter:** Auto, or 1–640 sensor lines. This writes the MT9V113's physical
  coarse integration register (`0x3012`), disables the live sequencer drivers
  as well as their state presets, and reapplies the setting
  after preview/capture transitions. Shorter values collect less light; longer
  values collect more. These are sensor integration units, not calibrated
  fractions of a second. The fine integration term is fixed at `0x006B`.
- **Gain:** nominal 1x, 2x, 4x, or 8x analogue sensor gain with a manual
  shutter. The dedicated color-channel gain registers (`0x302A`–`0x3030`)
  preserve the color balance sampled when leaving Auto; white balance stays
  locked during manual exposure. Sensor gain limits and quantization can limit
  the selected gain. Auto restores the original core registers, automatic
  exposure, gain, and white balance. This is not an ISO scale.
- **Night Burst:** one shutter press captures four frames and saves one merged
  photo. Hold still; press B to cancel. Alignment compensates for small camera
  shifts, and changed pixels are rejected to limit ghosting. If alignment fails,
  the first frame supplies the affected detail. It reduces noise without raising
  brightness or recovering clipped highlights. Use a longer manual integration
  when more light is needed. Only the completed photo and its thumbnail are saved;
  source frames stay in RAM. Timestamping happens after the merge.
- **Timestamp:** burns the capture date and time into saved photos and video.
- **Rear Light:** controls the pink indicator beside the outer camera and saves
  your preference. Night Burst always switches it off before capturing any
  frames, then restores your preference, including after cancellation or failure.
  If switching it off cannot be verified, the burst does not capture a photo.
  The inner camera keeps this light off.

For the moon, start with a short manual shutter and 1x gain, then lengthen the
integration until the bright surface has detail. Night Burst can reduce noise
at that same exposure. The DSi's fixed wide-angle lens still limits lunar detail.
There is no digital exposure/brightness adjustment in this pipeline.

Shutter, Gain, and Night Burst apply only to photos. Video restores each sensor's automatic
exposure settings; returning to photo mode reapplies the saved photo controls.
New settings default to Auto / 1x / Night Burst Off. Existing settings files
remain compatible.

Sensor writes are grouped and integration/gain are read back at sensor frame
boundaries before use. Context changes wait for final preview/capture entry
before reapplying the controls. Failures keep the previous selection, show a
stage/register code, and append expected/observed values to
`/photos/exposure-errors.log` before rollback. Exact
shutter times and actual sensor behavior still need validation on a physical DSi;
no diagnostic or probe app is included. Register definitions follow
[GBATEK's Aptina register research](https://mgba-emu.github.io/gbatek/#dsi-aptina-camera-registers-core-3000h-31ffh-38xxh).

Host checks for frame alignment, noise reduction, motion rejection, and sensor
register/restore behavior, including live AE overwrite, delayed context entry,
and deferred/quantized gains (with mocked I2C):

```sh
make -C tools/capture test
```
