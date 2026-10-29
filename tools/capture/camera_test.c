#include "camera.h"

#include <assert.h>
#include <stdio.h>

#undef REG_CAM_MCNT
#undef REG_CAM_CNT
#undef REG_CAM_DAT
#undef REG_NDMA1SAD
#undef REG_NDMA1DAD
#undef REG_NDMA1TCNT
#undef REG_NDMA1WCNT
#undef REG_NDMA1BCNT
#undef REG_NDMA1CNT
static u16 REG_CAM_MCNT, REG_CAM_CNT, REG_CAM_DAT;
static u32 REG_NDMA1SAD, REG_NDMA1DAD, REG_NDMA1TCNT, REG_NDMA1WCNT, REG_NDMA1BCNT, REG_NDMA1CNT;
u32 REG_SCFG_CLK, REG_IF;

#include "../../arm9/source/camera.c"

static unsigned sensorLines, sensorGain, exposureCalls;
static unsigned failures;
static bool failMode;
static bool failLight, light;
static unsigned lightWrites;

u32 pxiSendAndReceive(unsigned channel, u32 command) {
	assert(channel == PXI_CAMERA);
	if(command == CAM_REAR_LIGHT_ON || command == CAM_REAR_LIGHT_OFF) {
		lightWrites++;
		if(failLight)
			return 0;
		light = command == CAM_REAR_LIGHT_ON;
		return 1;
	}
	if(command & CAM_EXPOSURE_REQUEST) {
		exposureCalls++;
		if(failures) {
			failures--;
			return 0;
		}
		sensorLines = command & CAM_EXPOSURE_LINES_MASK;
		sensorGain  = (command >> CAM_EXPOSURE_GAIN_SHIFT) & 3;
		return 1;
	}
	if(command == CAM_SET_MODE_PREVIEW || command == CAM_SET_MODE_CAPTURE)
		return failMode ? 0 : command == CAM_SET_MODE_PREVIEW ? CAPTURE_MODE_PREVIEW : CAPTURE_MODE_CAPTURE;
	if(command >= CAM_GET_EXPOSURE_ERROR_STAGE && command <= CAM_GET_EXPOSURE_ERROR_OBSERVED) {
		const u16 error[] = {2, 0x3022, 0, 0x100};
		return error[command - CAM_GET_EXPOSURE_ERROR_STAGE];
	}
	return command;
}

void swiDelay(unsigned delay) { (void)delay; }
void swiWaitForVBlank(void) { REG_NDMA1CNT = 0; }
void irqSet(u32 mask, void (*handler)(void)) {
	(void)mask;
	(void)handler;
}
void irqEnable(u32 mask) {
	(void)mask;
	assert(!(REG_CAM_CNT & BIT(15)));
}
void irqDisable(u32 mask) { (void)mask; }
void irqClear(u32 mask) { (void)mask; }
void armDCacheFlush(const void *address, unsigned size) {
	(void)address;
	(void)size;
	assert(!(REG_CAM_CNT & BIT(15)));
}
void armDCacheInvalidate(const void *address, unsigned size) {
	(void)address;
	(void)size;
}

static void pollPreview(void) {
	for(unsigned i = 0; i < 241; i++)
		assert(cameraNextFrame() == NULL);
}

int main(void) {
	assert(cameraActivate(CAM_OUTER));
	assert(cameraSetExposure(64, 2));
	assert(cameraStreamStart() && cameraStreamRunning());
	camDmaIsr();
	unsigned unchangedCalls = exposureCalls;
	assert(cameraSetExposure(64, 2));
	assert(exposureCalls == unchangedCalls && s_frameCount == 1 && cameraStreamRunning());

	failures = 1;
	assert(!cameraSetExposure(640, 3));
	assert(cameraStreamRunning() && sensorLines == 64 && sensorGain == 2);
	assert(cameraExposureError(EXPOSURE_ERROR_REGISTER) == 0x3022);

	// Failed apply, rollback, and preview reapply leave a recoverable request.
	failures = 3;
	assert(!cameraSetExposure(640, 3));
	assert(!cameraStreamRunning());
	assert(cameraSetExposure(128, 1));
	assert(cameraStreamRunning() && sensorLines == 128 && sensorGain == 1);

	failures = 3;
	assert(!cameraSetExposure(640, 3));
	pollPreview();
	assert(cameraStreamRunning() && sensorLines == 128 && sensorGain == 1);

	cameraStreamStop();
	unsigned calls = exposureCalls;
	pollPreview();
	assert(!cameraStreamRunning() && exposureCalls == calls);

	// A failed initial mode entry can recover without another shutter press.
	assert(cameraActivate(CAM_INNER));
	failMode = true;
	assert(!cameraStreamStart());
	assert(!cameraSetExposure(64, 0));
	assert(!cameraStreamRunning());
	failMode = false;
	pollPreview();
	assert(cameraStreamRunning() && sensorLines == 128 && sensorGain == 1);

	u16 capture[256 * 192];
	assert(cameraCapture(capture, CAPTURE_MODE_PREVIEW));
	pollPreview();
	assert(!cameraStreamRunning());
	assert(cameraStreamStart());
	assert(cameraDeactivate(CAM_INNER));
	pollPreview();
	assert(!cameraStreamRunning());
	assert(cameraExposureError(EXPOSURE_ERROR_STAGE));
	assert(!cameraSetExposure(64, 0));
	assert(!cameraExposureError(EXPOSURE_ERROR_STAGE));
	assert(!cameraExposureError(EXPOSURE_ERROR_REGISTER));

	// A recovered preview retries a light write that failed alongside it.
	assert(cameraActivate(CAM_OUTER));
	failLight = failMode = true;
	assert(!cameraSetRearLight(true));
	assert(!cameraStreamStart());
	failLight = failMode = false;
	pollPreview();
	assert(cameraStreamRunning() && light);
	assert(cameraSetRearLight(false));
	unsigned writes = lightWrites;
	cameraStreamStop();
	pollPreview();
	assert(!light && lightWrites == writes);
	assert(cameraStreamStart());
	pollPreview();
	assert(!light && lightWrites == writes + 1);

	// A FIFO error must not publish a frame that spans two sensor frames.
	u32 overruns = cameraStreamOverruns();
	REG_CAM_CNT &= ~BIT(4);
	u16 *frame = s_frames[s_fill];
	for(unsigned y = 0; y < 192; y++)
		for(unsigned x = 0; x < 256; x++)
			frame[y * 256 + x] = y;
	camDmaIsr();
	assert(cameraNextFrame() == frame);
	assert(frame[0] == 0 && frame[191 * 256] == 191);

	u32 completed = s_frameCount;
	REG_CAM_CNT |= BIT(4);
	camDmaIsr();
	assert(s_frameCount == completed);
	assert(cameraStreamOverruns() == overruns + 1);
	assert(!cameraStreamRunning() && cameraStreamRequested());
	assert(!(REG_CAM_CNT & BIT(15)) && !REG_NDMA1CNT);
	assert(cameraNextFrame() == NULL);
	assert(cameraStreamRunning() && s_frameCount == 0);
	assert((REG_CAM_CNT & (BIT(5) | BIT(15))) == (BIT(5) | BIT(15)));
	assert(REG_NDMA1CNT == CAM_NDMA_CFG && REG_NDMA1TCNT == CAM_PREVIEW_WORDS);
	assert(cameraNextFrame() == NULL);

	REG_CAM_CNT &= ~BIT(4); // hardware clears the error when the FIFO is flushed
	frame = s_frames[s_fill];
	camDmaIsr();
	assert(cameraNextFrame() == frame);
	REG_CAM_CNT |= BIT(4);
	camDmaIsr();
	assert(cameraStreamOverruns() == overruns + 2);
	cameraStreamStop();
	pollPreview();
	assert(!cameraStreamRunning() && !cameraStreamRequested());
	puts("camera: exposure faults, preview retry, DMA startup, FIFO resync, capture and stop passed");
}
