#include "camera.h"

#include "calico/arm/cache.h"
#include "calico/nds/irq.h"
#include "calico/nds/pxi.h"

#include <nds.h>
#include <stddef.h>

// https://problemkaputt.de/gbatek-dsi-cameras.htm

Camera activeCamera    = CAM_NONE;
CaptureMode activeMode = CAPTURE_MODE_PREVIEW;

// NDMA1 is the camera channel. "Start by hardware, repeat until TCNT" with
// TCNT = one whole frame is the documented camera usage: the channel raises an
// IRQ after the last block, and the IRQ re-arms it into the other buffer.
//
// The important part is what we do NOT do: CAM_CNT.15 (transfer enable) is set
// once and left alone while streaming. Clearing it per frame - which is what
// the older code did, to make the config bits writeable - plus the implicit
// CAM_CNT.5 flush ("flush CAM_DAT till next Camera Vblank") throws away most of
// a sensor frame on every capture, which is why preview and recording ran at
// about half the sensor's frame rate.
#define CAM_NDMA_CFG 0xCB044000u // start | irq enable | camera timing | 16-word blocks | fixed src
#define CAM_NDMA_ONE 0x8B044000u // same, without the completion IRQ

// Frame buffers in main RAM. The ISR writes the next frame into one while the
// ARM9 reads another; the third absorbs a consumer that is a frame late.
// Cache line aligned so invalidating a finished frame never touches
// neighbouring data.
#define CAM_FRAMES 3
alignas(32) static u16 s_frames[CAM_FRAMES][256 * 192];

static bool s_streaming;
static bool s_streamRequested;
static bool s_rearLight = true;
static unsigned s_exposureLines;
static unsigned s_exposureGain;
static u16 s_exposureError[EXPOSURE_ERROR_FIELDS];
static bool s_exposureDirty = true;
static bool s_modeValid;          // sensor is known to be in activeMode
static volatile u32 s_frameCount; // frames completed since stream start
static volatile u8 s_fill;        // buffer the camera is writing right now
static volatile u32 s_overruns;   // CAM_CNT.4 episodes seen in the ISR
static bool s_overrunSeen;        // CAM_CNT.4 stays latched until a flush
static u32 s_seen;                // frames handed to the caller so far
static u32 s_callsWithoutFrame;   // stall watchdog for cameraNextFrame

// Re-arm straight from the completion IRQ so the gap between frames is a few
// microseconds, not the time it takes the main loop to notice and copy.
static void camDmaIsr(void) {
	int next = s_fill + 1;
	if(next == CAM_FRAMES)
		next = 0;
	s_fill = (u8)next;
	s_frameCount++;

	// Bit 4 latches until it is cleared with bit 5, so count episodes, not
	// every frame that happens to still see it.
	if(REG_CAM_CNT & BIT(4)) {
		if(!s_overrunSeen)
			s_overruns++;
		s_overrunSeen = true;
	} else {
		s_overrunSeen = false;
	}

	REG_NDMA1DAD  = (u32)s_frames[next];
	REG_NDMA1TCNT = CAM_PREVIEW_WORDS;
	REG_NDMA1WCNT = 512; // 4 scanlines per block
	REG_NDMA1CNT  = CAM_NDMA_CFG;
}

bool cameraInit() {
	REG_SCFG_CLK |= BIT(2); // CamInterfaceClock = ON
	REG_CAM_MCNT = 0;       // Camera Module Control
	swiDelay(0x1E);
	REG_SCFG_CLK |= BIT(8); // CamExternal Clock = ON
	swiDelay(0x1E);
	REG_CAM_MCNT = BIT(1) | BIT(5); // Camera Module Control
	swiDelay(0x2008);
	REG_SCFG_CLK &= ~BIT(8); // CamExternal Clock = OFF
	REG_CAM_CNT &= ~BIT(15); // allow changing params
	REG_CAM_CNT |= BIT(5);   // flush data fifo
	REG_CAM_CNT = (REG_CAM_CNT & ~(0x0300)) | 0x0200;
	REG_CAM_CNT |= BIT(10);
	REG_CAM_CNT |= BIT(11); // irq enable
	REG_SCFG_CLK |= BIT(8); // CamExternal Clock = ON
	swiDelay(0x14);

	// issue "aptina_code_list_init" via I2C bus on ARM7 side
	u32 val = pxiSendAndReceive(PXI_CAMERA, CAM_INIT);

	REG_SCFG_CLK &= ~BIT(8); // CamExternal Clock = OFF
	REG_SCFG_CLK |= BIT(8);  // CamExternal Clock = ON
	swiDelay(0x14);

	s_modeValid = false;
	return val == 0x2280;
}

bool cameraActivate(Camera cam) {
	if(activeCamera != CAM_NONE)
		cameraDeactivate(activeCamera);

	cameraStreamStop();

	u32 command = (cam == CAM_INNER) ? CAM0_ACTIVATE : CAM1_ACTIVATE;
	if(pxiSendAndReceive(PXI_CAMERA, command) == command) {
		activeCamera    = cam;
		s_exposureDirty = true;
		s_modeValid     = false; // waking a camera is the safe place to resend the mode
		return true;
	} else {
		return false;
	}
}

bool cameraSetRearLight(bool enabled) {
	s_rearLight = enabled;
	return pxiSendAndReceive(PXI_CAMERA, enabled ? CAM_REAR_LIGHT_ON : CAM_REAR_LIGHT_OFF) == 1;
}

bool cameraDeactivate(Camera cam) {
	cameraStreamStop();

	u32 command = (cam == CAM_INNER) ? CAM0_DEACTIVATE : CAM1_DEACTIVATE;
	if(pxiSendAndReceive(PXI_CAMERA, command) == command) {
		activeCamera = CAM_NONE;
		s_modeValid  = false;
		return true;
	} else {
		return false;
	}
}

// Waits for NDMA1 to go idle, bailing out after roughly `vblanks` frames so a
// wedged camera can never hang the app. Always leaves the channel disabled.
static bool camWaitDone(u32 vblanks) {
	for(u32 i = 0; i < vblanks; i++) {
		if(!(REG_NDMA1CNT & BIT(31))) {
			REG_NDMA1CNT = 0;
			return true;
		}
		swiWaitForVBlank();
	}
	REG_NDMA1CNT = 0;
	return false;
}

static bool camApplyExposure(unsigned lines, unsigned gain);

static void camReadExposureError(void) {
	for(unsigned i = 0; i < EXPOSURE_ERROR_FIELDS; i++)
		s_exposureError[i] = pxiSendAndReceive(PXI_CAMERA, CAM_GET_EXPOSURE_ERROR_STAGE + i);
}

// Sets up the sensor mode and the CAM_CNT format bits while stopped.
static bool camConfigure(CaptureMode mode) {
	bool preview = mode == CAPTURE_MODE_PREVIEW;

	if(mode != activeMode || !s_modeValid) {
		if(pxiSendAndReceive(PXI_CAMERA, preview ? CAM_SET_MODE_PREVIEW : CAM_SET_MODE_CAPTURE) != mode) {
			camReadExposureError();
			return false;
		}
		activeMode      = mode;
		s_modeValid     = true;
		s_exposureDirty = true;
	}

	if(s_exposureDirty) {
		if(!camApplyExposure(s_exposureLines, s_exposureGain))
			return false;
		s_exposureDirty = false;
	}
	REG_CAM_CNT &= ~BIT(15); // allow changing params
	if(preview)              // YUV-to-RGB555 and "scanline count - 1" = 3
		REG_CAM_CNT |= 0x2003;
	else // raw YUV and "scanline count - 1" = 0
		REG_CAM_CNT &= ~0x2003;
	REG_CAM_CNT |= BIT(5); // clear overrun flag, flush till next camera vblank
	REG_CAM_CNT |= BIT(15);
	return true;
}

static bool camApplyExposure(unsigned lines, unsigned gain) {
	bool ok = pxiSendAndReceive(PXI_CAMERA, CAM_EXPOSURE_REQUEST | lines | (gain << CAM_EXPOSURE_GAIN_SHIFT)) == 1;
	if(ok) {
		s_exposureLines = lines;
		s_exposureGain  = gain;
		s_exposureDirty = false;
	}
	bool restored = ok;
	if(!ok) {
		camReadExposureError();
		restored =
			pxiSendAndReceive(
				PXI_CAMERA, CAM_EXPOSURE_REQUEST | s_exposureLines | (s_exposureGain << CAM_EXPOSURE_GAIN_SHIFT)) == 1;
		s_exposureDirty = !restored;
	}
	return ok;
}

bool cameraSetExposure(unsigned lines, unsigned gain) {
	for(unsigned i = 0; i < EXPOSURE_ERROR_FIELDS; i++)
		s_exposureError[i] = 0;
	if(lines > CAM_EXPOSURE_MAX_LINES || gain > 3 || activeCamera == CAM_NONE)
		return false;
	bool resume       = s_streamRequested;
	unsigned oldLines = s_exposureLines, oldGain = s_exposureGain;
	cameraStreamStop();
	bool ok = camApplyExposure(lines, gain);
	if(resume && !cameraStreamStart()) {
		if(ok) {
			bool restored   = camApplyExposure(oldLines, oldGain);
			s_exposureLines = oldLines;
			s_exposureGain  = oldGain;
			s_exposureDirty = !restored;
			cameraStreamStart();
		}
		return false;
	}
	return ok;
}

unsigned cameraExposureError(ExposureErrorField field) {
	return field < EXPOSURE_ERROR_FIELDS ? s_exposureError[field] : 0;
}

bool cameraCapture(u16 *dst, CaptureMode mode) {
	cameraStreamStop();
	if(!camConfigure(mode))
		return false;

	u32 bytes = mode == CAPTURE_MODE_PREVIEW ? CAM_PREVIEW_SIZE : (640 * 480 * 2);

	// Clean the destination before the DMA: a reuse of this buffer can leave
	// dirty lines that would otherwise be evicted over the incoming frame.
	armDCacheFlush(dst, bytes);

	REG_NDMA1SAD  = (u32)&REG_CAM_DAT;
	REG_NDMA1DAD  = (u32)dst;
	REG_NDMA1TCNT = (mode == CAPTURE_MODE_PREVIEW ? CAM_PREVIEW_WORDS : (640 * 480) / 2);
	REG_NDMA1WCNT = (mode == CAPTURE_MODE_PREVIEW ? 512 : 320);
	REG_NDMA1BCNT = 2;
	REG_NDMA1CNT  = CAM_NDMA_ONE;

	bool ok = camWaitDone(120); // ~2s at 60Hz
	if(ok)                      // the DMA wrote around the cache, drop whatever the CPU still has
		armDCacheInvalidate(dst, bytes);
	REG_CAM_CNT &= ~BIT(15);
	return ok;
}

bool cameraStreamStart(void) {
	if(activeCamera == CAM_NONE) // nothing to stream from
		return false;

	cameraStreamStop();
	s_streamRequested   = true;
	s_callsWithoutFrame = 0;

	if(!camConfigure(CAPTURE_MODE_PREVIEW))
		return false;

	// Drop any dirty lines the CPU may still hold for the capture buffers: the
	// DMA bypasses the cache and a stray writeback would corrupt a frame.
	armDCacheFlush(s_frames, sizeof(s_frames));

	s_fill              = 0;
	s_frameCount        = 0;
	s_seen              = 0;
	s_overrunSeen       = false;
	s_callsWithoutFrame = 0;
	s_streaming         = true;

	REG_IF = IRQ_NDMA1; // drop a completion latched while the IRQ was masked
	irqSet(IRQ_NDMA1, camDmaIsr);
	irqEnable(IRQ_NDMA1);

	REG_NDMA1SAD  = (u32)&REG_CAM_DAT;
	REG_NDMA1DAD  = (u32)s_frames[0];
	REG_NDMA1TCNT = CAM_PREVIEW_WORDS;
	REG_NDMA1WCNT = 512;
	REG_NDMA1BCNT = 2;
	REG_NDMA1CNT  = CAM_NDMA_CFG;
	return true;
}

void cameraStreamStop(void) {
	// Mask first so the ISR can't re-arm the channel behind our back, then
	// shut it down and drop any completion that latched in between.
	irqDisable(IRQ_NDMA1);
	REG_NDMA1CNT = 0;
	REG_CAM_CNT &= ~BIT(15);
	REG_IF = IRQ_NDMA1;
	irqClear(IRQ_NDMA1);
	s_streaming = false;
	s_streamRequested = false;
}

u16 *cameraNextFrame(void) {
	u32 count = s_frameCount;
	if(count == s_seen || count == 0) {
		// Nothing for a long time means the camera stream died (or the sensor
		// went into standby); restart it rather than sitting on a frozen
		// picture forever. Callers poll once per vblank.
		if(s_streamRequested && ++s_callsWithoutFrame > 240) {
			s_callsWithoutFrame = 0;
			if(cameraStreamStart())
				cameraSetRearLight(s_rearLight);
		}
		return NULL;
	}

	s_callsWithoutFrame = 0;

	// With three buffers the frame we hand out is two completions away from
	// being written again; anything older is already gone.
	s_seen = count;
	u16 *f = s_frames[(count - 1) % CAM_FRAMES];
	armDCacheInvalidate(f, CAM_PREVIEW_SIZE);
	return f;
}

u32 cameraStreamOverruns(void) { return s_overruns; }

bool cameraStreamRunning(void) { return s_streaming; }

bool cameraStreamRequested(void) { return s_streamRequested; }
