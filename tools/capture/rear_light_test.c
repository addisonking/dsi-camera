#include "aptina.h"
#include "aptina_i2c.h"

#include <assert.h>
#include <stdio.h>

static u16 core[2][65536];
static u8 light;
static bool failWrite, ignoreWrite;

u8 aptWriteRegister(u8 device, u16 reg, u16 value) {
	int cam        = device == I2C_CAM0 ? 0 : 1;
	core[cam][reg] = value;
	if(reg == 0x0018) {
		if(value & 1) {
			core[cam][reg] |= 0x4000;
			core[cam][0x301A] &= ~4;
		} else {
			core[cam][reg] &= ~0x4000;
			core[cam][0x301A] |= 4;
		}
	}
	return 1;
}

u16 aptReadRegister(u8 device, u16 reg) { return core[device == I2C_CAM0 ? 0 : 1][reg]; }

u8 i2cWriteRegister(u8 device, u8 reg, u8 value) {
	assert(device == I2C_PM && reg == I2CREGPM_CAMLED);
	if(failWrite)
		return 0;
	if(!ignoreWrite)
		light = value;
	return 1;
}

u8 i2cReadRegister(u8 device, u8 reg) {
	assert(device == I2C_PM && reg == I2CREGPM_CAMLED);
	return light;
}

void swiWaitForVBlank(void) { assert(!"unexpected wait"); }

int main(void) {
	assert(setRearLight(false) && !light);
	activate(I2C_CAM1);
	assert(!light);
	assert(setRearLight(true) && light == 1);
	assert(setRearLight(false) && !light);
	deactivate(I2C_CAM1);
	activate(I2C_CAM1);
	assert(!light);
	assert(setRearLight(true) && light == 1);
	deactivate(I2C_CAM1);
	assert(!light);
	activate(I2C_CAM0);
	assert(!light);
	assert(setRearLight(true) && !light);
	deactivate(I2C_CAM0);
	activate(I2C_CAM1);
	assert(light == 1);
	failWrite = true;
	assert(!setRearLight(false) && light == 1);
	failWrite   = false;
	ignoreWrite = true;
	assert(!setRearLight(false) && light == 1);
	ignoreWrite = false;
	deactivate(I2C_CAM1);
	activate(I2C_CAM1);
	assert(light == 1);
	assert(setRearLight(false) && !light);
	deactivate(I2C_CAM1);
	assert(!light);
	puts("rear light: verified writes, inner/outer switching, sleep persistence, failed/ignored writes passed");
}
