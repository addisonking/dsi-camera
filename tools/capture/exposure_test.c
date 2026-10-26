#include "aptina.h"
#include "aptina_i2c.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern u8 currentDevice;
static u16 core[2][65536], mcu[2][65536], address[2];
static u16 pending[2][6];
static unsigned coreDelay[2], contextDelay[2], waits;
static u16 nextState[2];
static int failReg = -1, ignoreReg = -1, failMcu = -1;
static bool overrideExposure, stuckContext, liveAe, deferredCore, inFlightAwb;
static const u16 coreRegs[] = {0x3012, 0x3014, 0x302A, 0x302C, 0x302E, 0x3030};
static const u16 baseline[] = {120, 246, 32, 36, 40, 32};
static const u16 autoRegs[] = {0xA117,
							   0xA11D,
							   0xA123,
							   0xA129,
							   0xA118,
							   0xA11E,
							   0xA124,
							   0xA12A,
							   0xA119,
							   0xA11F,
							   0xA125,
							   0xA12B,
							   0xA11A,
							   0xA120,
							   0xA126,
							   0xA12C,
							   0xA115,
							   0xA102};

static int cameraIndex(void) { return currentDevice == I2C_CAM0 ? 0 : 1; }

u8 aptWriteRegister(u8 device, u16 reg, u16 value) {
	int cam = device == I2C_CAM0 ? 0 : 1;
	if(reg == failReg)
		return 0;
	if(reg == ignoreReg)
		return 1;
	if(reg == 0x098C) {
		address[cam] = value;
		return 1;
	}
	if(reg == 0x0990) {
		if(address[cam] == failMcu)
			return 0;
		mcu[cam][address[cam]] = value;
		if(address[cam] == 0xA103 && !stuckContext) {
			nextState[cam]    = value == CAPTURE_MODE_PREVIEW ? 3 : 7;
			contextDelay[cam] = 4;
			mcu[cam][0xA103]  = 0;
			mcu[cam][0xA104]  = 5;
		}
		return 1;
	}
	// Do not destroy the sensor's color/digital calibration via global gain.
	assert(reg == 0x3022 || reg == 0x3012 || reg == 0x3014 || (reg >= 0x302A && reg <= 0x3030));
	if(reg == 0x3022) {
		if(!(value & 0x0100) && (core[cam][reg] & 0x0100)) {
			if(deferredCore)
				coreDelay[cam] = 20; // intentionally longer than the old fixed wait
			else
				for(unsigned i = 0; i < 6; i++)
					core[cam][coreRegs[i]] = pending[cam][i];
		} else if(value & 0x0100) {
			for(unsigned i = 0; i < 6; i++)
				pending[cam][i] = core[cam][coreRegs[i]];
		}
		core[cam][reg] = value;
		return 1;
	}
	for(unsigned i = 0; i < 6; i++) {
		if(reg != coreRegs[i])
			continue;
		if(i >= 2)
			value &= value >= 64 ? ~3u : ~1u;
		if(core[cam][0x3022] & 0x0100)
			pending[cam][i] = value;
		else
			core[cam][reg] = value;
		return 1;
	}
	abort();
}

u16 aptReadRegister(u8 device, u16 reg) {
	int cam = device == I2C_CAM0 ? 0 : 1;
	return reg == 0x0990 ? mcu[cam][address[cam]] : core[cam][reg];
}

void swiWaitForVBlank(void) {
	waits++;
	int cam = cameraIndex();
	if(liveAe && (mcu[cam][0xA102] & 1))
		core[cam][0x3012] = 999;
	if(inFlightAwb) {
		// A color update already queued when live drivers were disabled.
		core[cam][0x302C] = 44;
		inFlightAwb       = false;
	}
	if(coreDelay[cam] && !--coreDelay[cam])
		for(unsigned i = 0; i < 6; i++)
			core[cam][coreRegs[i]] = pending[cam][i];
	if(overrideExposure)
		core[cam][0x3012] = 999;
	if(contextDelay[cam] && !--contextDelay[cam]) {
		mcu[cam][0xA104] = nextState[cam];
		for(unsigned i = 0; i < 6; i++)
			core[cam][coreRegs[i]] = baseline[i];
	}
}
u8 i2cWriteRegister(u8 device, u8 reg, u8 value) { return 1; }
u8 i2cReadRegister(u8 device, u8 reg) { return 0; }

static void setup(int cam) {
	currentDevice = cam ? I2C_CAM1 : I2C_CAM0;
	assert(setExposure(0, 0));
	for(unsigned i = 0; i < sizeof(autoRegs) / sizeof(*autoRegs); i++)
		mcu[cam][autoRegs[i]] = autoRegs[i] == 0xA115 ? 0x72 : autoRegs[i] == 0xA102 ? 0x8F : cam + 1;
	for(unsigned i = 0; i < 6; i++)
		core[cam][coreRegs[i]] = baseline[i];
	core[cam][0x3022] = 1;
	mcu[cam][0xA104]  = 3;
}

static void restored(int cam) {
	assert(core[cam][0x3022] == 1);
	for(unsigned i = 0; i < 6; i++)
		assert(core[cam][coreRegs[i]] == baseline[i]);
	for(unsigned i = 0; i < sizeof(autoRegs) / sizeof(*autoRegs); i++)
		assert(mcu[cam][autoRegs[i]] == (autoRegs[i] == 0xA115 ? 0x72 : autoRegs[i] == 0xA102 ? 0x8F : cam + 1));
}

int main(void) {
	liveAe       = true;
	deferredCore = true;
	setup(0);
	core[0][0x302E] = 52;
	// Physical DSi failure: blue gain request 13 reads back as 12.
	assert(setExposure(1, 0));
	assert(core[0][0x302E] == 12);
	assert(core[0][0x3012] == 1);
	assert(setExposure(0, 0));
	assert(core[0][0x302E] == 52);
	for(int cam = 0; cam < 2; cam++) {
		setup(cam);
		// This recreates the reported auto->manual failure with live AE enabled.
		assert(setExposure(1, 0));
		assert(mcu[cam][0xA102] == 0x80);
		assert(mcu[cam][0xA115] == 0x02);
		assert(core[cam][0x3012] == 1 && core[cam][0x3014] == 0x6B);
		assert(core[cam][0x302A] == 8 && core[cam][0x302C] == 8 && core[cam][0x302E] == 10);
		for(unsigned gain = 0; gain < 4; gain++) {
			assert(setExposure(640, gain));
			assert(core[cam][0x3012] == 640);
			assert(core[cam][0x302A] == (8u << gain));
			assert(core[cam][0x302C] == (gain ? 9u << gain : 8));
			assert(core[cam][0x302E] == (10u << gain));
		}
		assert(setMode(CAPTURE_MODE_CAPTURE));
		assert(!contextDelay[cam] && mcu[cam][0xA104] == 7);
		assert(setExposure(64, 0));
		for(int i = 0; i < 8; i++)
			swiWaitForVBlank();
		assert(core[cam][0x3012] == 64);
		assert(setMode(CAPTURE_MODE_PREVIEW));
		assert(setExposure(64, 0));
		assert(core[cam][0x3012] == 64);
		assert(setExposure(0, 0));
		restored(cam);
	}
	setup(0);
	inFlightAwb = true;
	assert(setExposure(8, 0));
	assert(core[0][0x302C] == 10); // Color baseline sampled after the queued update.
	assert(setExposure(0, 0));
	assert(core[0][0x302C] == 44);

	setup(0);
	core[0][0x302A] = core[0][0x3030] = 12;
	core[0][0x302C]                   = 14;
	core[0][0x302E]                   = 16;
	assert(setExposure(8, 2));
	assert(core[0][0x302A] == 32 && core[0][0x302C] == 36 && core[0][0x302E] == 42);
	assert(setExposure(0, 0));
	assert(core[0][0x302A] == 12 && core[0][0x302C] == 14 && core[0][0x302E] == 16);

	setup(0);
	assert(!setExposure(641, 0) && !setExposure(1, 4));
	failMcu = 0xA102;
	assert(!setExposure(8, 0));
	assert(exposureError(EXPOSURE_ERROR_STAGE) == 2);
	assert(exposureError(EXPOSURE_ERROR_REGISTER) == 0xA102);
	failMcu = -1;
	assert(setExposure(0, 0));
	restored(0);

	setup(0);
	failReg = 0x3012;
	assert(!setExposure(8, 0));
	assert(exposureError(EXPOSURE_ERROR_REGISTER) == 0x3012);
	assert(!(core[0][0x3022] & 0x0100));
	failReg = -1;
	assert(setExposure(0, 0));
	restored(0);

	setup(0);
	ignoreReg = 0x3012;
	assert(!setExposure(8, 0));
	assert(exposureError(EXPOSURE_ERROR_STAGE) == 5);
	assert(exposureError(EXPOSURE_ERROR_REGISTER) == 0x3012);
	assert(exposureError(EXPOSURE_ERROR_EXPECTED) == 8);
	assert(exposureError(EXPOSURE_ERROR_OBSERVED) == 120);
	ignoreReg = -1;
	assert(setExposure(0, 0));
	restored(0);

	setup(0);
	assert(setExposure(8, 0));
	overrideExposure = true;
	assert(!setExposure(4, 0));
	overrideExposure = false;
	assert(setExposure(0, 0));
	restored(0);

	stuckContext = true;
	waits        = 0;
	assert(!setMode(CAPTURE_MODE_CAPTURE));
	assert(waits == 180 && exposureError(EXPOSURE_ERROR_REGISTER) == 0xA104);
	puts("exposure: live AE regression, delayed/quantized gains, color preservation, context entry, both cameras, "
		 "restore, failed/ignored writes, override, timeout passed");
}
