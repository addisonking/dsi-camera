#include "aptina.h"

#include "aptina_i2c.h"

u8 currentDevice = I2C_CAM0;
static bool s_cameraActive;
static bool s_rearLight = true;

bool setRearLight(bool enabled) {
	s_rearLight = enabled;
	u8 value    = enabled && s_cameraActive && currentDevice == I2C_CAM1;
	if(!i2cWriteRegister(I2C_PM, I2CREGPM_CAMLED, value) || (i2cReadRegister(I2C_PM, I2CREGPM_CAMLED) & 3) != value)
		return false;
	return true;
}

// Waits until the bits of the register are clear
void aptWaitClr(u8 device, u16 reg, u16 mask) {
	while(aptReadRegister(device, reg) & mask) {
		swiWaitForVBlank();
	}
}

// Waits until the bits of the register are set
void aptWaitSet(u8 device, u16 reg, u16 mask) {
	while((aptReadRegister(device, reg) & mask) != mask) {
		swiWaitForVBlank();
	}
}

// Clears the bits in the mask at the register
void aptClr(u8 device, u16 reg, u16 mask) {
	u16 temp = aptReadRegister(device, reg);
	aptWriteRegister(device, reg, temp & ~mask);
}

// Sets the bits in the mask at the register
void aptSet(u8 device, u16 reg, u16 mask) {
	u16 temp = aptReadRegister(device, reg);
	aptWriteRegister(device, reg, temp | mask);
}

// Reads an MCU value
u16 aptReadMcu(u8 device, u16 reg) {
	aptWriteRegister(device, 0x098C, reg);
	return aptReadRegister(device, 0x0990);
}

// Writes an MCU value
void aptWriteMcu(u8 device, u16 reg, u16 data) {
	aptWriteRegister(device, 0x098C, reg);
	aptWriteRegister(device, 0x0990, data);
}

// Waits until the bits of the MCU register are clear
void aptWaitMcuClr(u8 device, u16 reg, u16 mask) {
	while(aptReadMcu(device, reg) & mask) {
		swiWaitForVBlank();
	}
}

// Sets the bits in the mask at the MCU register
void aptSetMcu(u8 device, u16 reg, u16 mask) {
	u16 temp = aptReadMcu(device, reg);
	aptWriteMcu(device, reg, temp | mask);
}

// https://problemkaputt.de/gbatek.htm#dsiaptinacamerainitialization
void init(u8 device) {
	aptWriteRegister(device, 0x001A, 0x0003); // RESET_AND_MISC_CONTROL (issue reset)   ;\reset
	aptWriteRegister(device, 0x001A, 0x0000); // RESET_AND_MISC_CONTROL (release reset) ;/
	aptWriteRegister(device, 0x0018, 0x4028); // STANDBY_CONTROL (wakeup)               ;\.
	aptWriteRegister(device, 0x001E, 0x0201); // PAD_SLEW                               ; wakeup
	aptWriteRegister(device, 0x0016, 0x42DF); // CLOCKS_CONTROL                         ;
	aptWaitClr(device, 0x0018, 0x4000);       // STANDBY_CONTROL (wait for WakeupDone)  ;
	aptWaitSet(device, 0x301A, 0x0004);       // UNDOC_CORE_301A (wait for WakeupDone)  ;/
	aptWriteMcu(device, 0x002F0, 0x0000);     // UNDOC! RAM?
	aptWriteMcu(device, 0x002F2, 0x0210);     // UNDOC! RAM?
	aptWriteMcu(device, 0x002F4, 0x001A);     // UNDOC! RAM?
	aptWriteMcu(device, 0x02145, 0x02F4);     // UNDOC! SEQ?
	aptWriteMcu(device, 0x0A134, 0x0001);     // UNDOC! SEQ?
	aptSetMcu(device, 0xA115, 0x0002);        // SEQ_CAP_MODE (set bit1=video)
	aptWriteMcu(device, 0x2755, 0x0002);      // MODE_OUTPUT_FORMAT_A (bit5=0=YUV)      ;\select
	aptWriteMcu(device, 0x2757, 0x0002);      // MODE_OUTPUT_FORMAT_B                   ;/YUV mode
	aptWriteRegister(device, 0x0014, 0x2145); // PLL_CONTROL                            ;\.
	aptWriteRegister(device, 0x0010, 0x0111); // PLL_DIVIDERS                           ; match
	aptWriteRegister(device, 0x0012, 0x0000); // PLL_P_DIVIDERS                         ; PLL
	aptWriteRegister(device, 0x0014, 0x244B); // PLL_CONTROL                            ; to DSi
	aptWriteRegister(device, 0x0014, 0x304B); // PLL_CONTROL                            ; timings
	aptWaitSet(device, 0x0014, 0x8000);       // PLL_CONTROL (wait for PLL Lock okay)   ;
	aptClr(device, 0x0014, 0x0001);           // PLL_CONTROL (disable PLL Bypass)       ;/
	aptWriteMcu(device, 0x2703, 0x0100);      // MODE_OUTPUT_WIDTH_A              ;\Size A
	aptWriteMcu(device, 0x2705, 0x00C0);      // MODE_OUTPUT_HEIGHT_A             ;/ 256x192
	aptWriteMcu(device, 0x2707, 0x0280);      // MODE_OUTPUT_WIDTH_B              ;\Size B
	aptWriteMcu(device, 0x2709, 0x01E0);      // MODE_OUTPUT_HEIGHT_B             ;/ 640x480
	aptWriteMcu(device, 0x2715, 0x0001);      // MODE_SENSOR_ROW_SPEED_A          ;\.
	aptWriteMcu(device, 0x2719, 0x001A);      // MODE_SENSOR_FINE_CORRECTION_A    ;
	aptWriteMcu(device, 0x271B, 0x006B);      // MODE_SENSOR_FINE_IT_MIN_A        ; Sensor A
	aptWriteMcu(device, 0x271D, 0x006B);      // MODE_SENSOR_FINE_IT_MAX_MARGIN_A ;
	aptWriteMcu(device, 0x271F, 0x02C0);      // MODE_SENSOR_FRAME_LENGTH_A       ;
	aptWriteMcu(device, 0x2721, 0x034B);      // MODE_SENSOR_LINE_LENGTH_PCK_A    ;/
	aptWriteMcu(device, 0xA20B, 0x0000);      // AE_MIN_INDEX                     ;\AE min/max
	aptWriteMcu(device, 0xA20C, 0x0006);      // AE_MAX_INDEX                     ;/
	aptWriteMcu(device, 0x272B, 0x0001);      // MODE_SENSOR_ROW_SPEED_B          ;\.
	aptWriteMcu(device, 0x272F, 0x001A);      // MODE_SENSOR_FINE_CORRECTION_B    ;
	aptWriteMcu(device, 0x2731, 0x006B);      // MODE_SENSOR_FINE_IT_MIN_B        ; Sensor B
	aptWriteMcu(device, 0x2733, 0x006B);      // MODE_SENSOR_FINE_IT_MAX_MARGIN_B ;
	aptWriteMcu(device, 0x2735, 0x02C0);      // MODE_SENSOR_FRAME_LENGTH_B       ;
	aptWriteMcu(device, 0x2737, 0x034B);      // MODE_SENSOR_LINE_LENGTH_PCK_B    ;/
	aptSet(device, 0x3210, 0x0008);           // COLOR_PIPELINE_CONTROL (PGA pixel shading..)
	aptWriteMcu(device, 0xA208, 0x0000);      // UNDOC! RESERVED_AE_08
	aptWriteMcu(device, 0xA24C, 0x0020);      // AE_TARGETBUFFERSPEED
	aptWriteMcu(device, 0xA24F, 0x0070);      // AE_BASETARGET
	if(device == I2C_CAM0) {                  //                                  ;\.
		aptWriteMcu(device, 0x2717, 0x0024);  // MODE_SENSOR_READ_MODE_A          ; Read Mode
		aptWriteMcu(device, 0x272D, 0x0024);  // MODE_SENSOR_READ_MODE_B          ; with x-flip
	} else {                                  //                                  ; on internal
		aptWriteMcu(device, 0x2717, 0x0025);  // MODE_SENSOR_READ_MODE_A          ; camera
		aptWriteMcu(device, 0x272D, 0x0025);  // MODE_SENSOR_READ_MODE_B          ;/
	}
	if(device == I2C_CAM0) {                 //                                  ;\.
		aptWriteMcu(device, 0xA202, 0x0022); // AE_WINDOW_POS                    ;
		aptWriteMcu(device, 0xA203, 0x00BB); // AE_WINDOW_SIZE                   ;
	} else {                                 //                                  ;
		aptWriteMcu(device, 0xA202, 0x0000); // AE_WINDOW_POS                    ;
		aptWriteMcu(device, 0xA203, 0x00FF); // AE_WINDOW_SIZE                   ;/
	}
	aptSet(device, 0x0016, 0x0020);               // CLOCKS_CONTROL (set bit5=1, reserved)
	aptWriteMcu(device, 0xA115, 0x0072);          // SEQ_CAP_MODE (was already manipulated above)
	aptWriteMcu(device, 0xA11F, 0x0001);          // SEQ_PREVIEW_1_AWB                ;\.
	if(device == I2C_CAM0) {                      //                                  ;
		aptWriteRegister(device, 0x326C, 0x0900); // ;APERTURE_PARAMETERS             ;
		aptWriteMcu(device, 0xAB22, 0x0001);      // HG_LL_APCORR1                    ;
	} else {                                      //                                  ;
		aptWriteRegister(device, 0x326C, 0x1000); // APERTURE_PARAMETERS              ;
		aptWriteMcu(device, 0xAB22, 0x0002);      // HG_LL_APCORR1                    ;/.
	}
	aptWriteMcu(device, 0xA103, 0x0006);   // SEQ_CMD (06h=RefreshMode)
	aptWaitMcuClr(device, 0xA103, 0x000F); // SEQ_CMD (wait above to become ZERO)
	aptWriteMcu(device, 0xA103, 0x0005);   // SEQ_CMD (05h=Refresh)
	aptWaitMcuClr(device, 0xA103, 0x000F); // SEQ_CMD (wait above to become ZERO)
}

void activate(u8 device) {
	aptClr(device, 0x0018, 0x0001);     // STANDBY_CONTROL (bit0=0=wakeup)       ;\.
	aptWaitClr(device, 0x0018, 0x4000); // STANDBY_CONTROL (wait for WakeupDone) ; Wakeup
	aptWaitSet(device, 0x301A, 0x0004); // UNDOC_CORE_301A (wait for WakeupDone) ;/
	aptWriteRegister(
		device, 0x3012, 0x0010);    // COARSE_INTEGRATION_TIME (Y Time)      ; set to large number for more brightness
	aptSet(device, 0x001A, 0x0200); // RESET_AND_MISC_CONTROL (Parallel On)  ;-Data on
	currentDevice  = device;
	s_cameraActive = true;
	setRearLight(s_rearLight);
}

void deactivate(u8 device) {
	aptClr(device, 0x001A, 0x0200);     // RESET_AND_MISC_CONTROL (Parallel Off)  ;-Data off
	aptSet(device, 0x0018, 0x0001);     // STANDBY_CONTROL (set bit0=1=Standby)   ;\.
	aptWaitSet(device, 0x0018, 0x4000); // STANDBY_CONTROL (wait for StandbyDone) ; Standby
	aptWaitClr(device, 0x301A, 0x0004); // UNDOC_CORE_301A (wait for StandbyDone) ;/
	s_cameraActive = false;
	i2cWriteRegister(I2C_PM, I2CREGPM_CAMLED, 0);

	currentDevice = I2C_CAM0;
}

// Save each sensor's automation and calibrated core values independently.
static const u16 s_autoRegs[] = {
	0xA117,
	0xA11D,
	0xA123,
	0xA129, // AE in every preview/transition state
	0xA118,
	0xA11E,
	0xA124,
	0xA12A, // flicker detection
	0xA119,
	0xA11F,
	0xA125,
	0xA12B, // white balance
	0xA11A,
	0xA120,
	0xA126,
	0xA12C, // histogram gain
	0xA115,
	0xA102 // capture options, live driver enable mask
};
#define AUTO_REG_COUNT (sizeof(s_autoRegs) / sizeof(s_autoRegs[0]))
static const u16 s_coreRegs[] = {0x3012, 0x3014, 0x302A, 0x302C, 0x302E, 0x3030};
#define CORE_REG_COUNT (sizeof(s_coreRegs) / sizeof(s_coreRegs[0]))
static u16 s_autoValues[2][AUTO_REG_COUNT];
static u16 s_coreValues[2][CORE_REG_COUNT];
static bool s_manual[2], s_coreValid[2];
static u16 s_exposureError[EXPOSURE_ERROR_FIELDS];

static bool exposureFail(u16 stage, u16 reg, u16 expected, u16 observed) {
	if(!s_exposureError[EXPOSURE_ERROR_STAGE]) {
		s_exposureError[EXPOSURE_ERROR_STAGE]    = stage;
		s_exposureError[EXPOSURE_ERROR_REGISTER] = reg;
		s_exposureError[EXPOSURE_ERROR_EXPECTED] = expected;
		s_exposureError[EXPOSURE_ERROR_OBSERVED] = observed;
	}
	return false;
}

unsigned exposureError(ExposureErrorField field) { return field < EXPOSURE_ERROR_FIELDS ? s_exposureError[field] : 0; }

static bool exposureReadMcu(u16 reg, u16 *value) {
	if(!aptWriteRegister(currentDevice, 0x098C, reg))
		return exposureFail(1, reg, 0, 0xFFFF);
	*value = aptReadRegister(currentDevice, 0x0990);
	if(*value == 0xFFFF)
		return exposureFail(1, reg, 0, *value);
	return true;
}

static bool exposureWriteMcu(u16 reg, u16 value) {
	if(!aptWriteRegister(currentDevice, 0x098C, reg) || !aptWriteRegister(currentDevice, 0x0990, value))
		return exposureFail(2, reg, value, 0xFFFF);
	u16 observed;
	if(!exposureReadMcu(reg, &observed))
		return false;
	return observed == value || exposureFail(3, reg, value, observed);
}

bool setMode(CaptureMode mode) {
	if(!aptWriteRegister(currentDevice, 0x098C, 0xA103) || !aptWriteRegister(currentDevice, 0x0990, mode))
		return exposureFail(2, 0xA103, mode, 0xFFFF);
	u16 expected = mode == CAPTURE_MODE_PREVIEW ? 3 : 7;
	u16 state    = 0xFFFF;
	for(int i = 0; i <= 180; i++) {
		u16 command;
		if(!exposureReadMcu(0xA103, &command) || !exposureReadMcu(0xA104, &state))
			return false;
		// Command acceptance precedes context entry and its core-register reload.
		if(command == 0 && state == expected)
			return true;
		if(i < 180)
			swiWaitForVBlank();
	}
	return exposureFail(4, 0xA104, expected, state);
}

static bool exposureFreeze(int cam) {
	// SEQ_MODE is the live enable mask. Changing state presets alone leaves
	// the currently executing AE/AWB drivers free to overwrite manual values.
	if(!exposureWriteMcu(0xA102, s_autoValues[cam][AUTO_REG_COUNT - 1] & ~0x0F))
		return false;
	for(unsigned i = 0; i < AUTO_REG_COUNT - 1; i++) {
		u16 value = s_autoRegs[i] == 0xA115 ? s_autoValues[cam][i] & ~0x70 : 0;
		if(!exposureWriteMcu(s_autoRegs[i], value))
			return false;
	}
	return true;
}

static bool exposureWriteCore(const u16 *values) {
	u16 hold = aptReadRegister(currentDevice, 0x3022);
	if(hold == 0xFFFF)
		return exposureFail(1, 0x3022, 0, hold);
	if(hold & 0x0100) {
		hold &= ~0x0100;
		bool released = false;
		for(int i = 0; i < 3 && !released; i++)
			released = aptWriteRegister(currentDevice, 0x3022, hold) && aptReadRegister(currentDevice, 0x3022) == hold;
		if(!released)
			return exposureFail(2, 0x3022, hold, aptReadRegister(currentDevice, 0x3022));
	}
	bool written = aptWriteRegister(currentDevice, 0x3022, hold | 0x0100) &&
		aptReadRegister(currentDevice, 0x3022) == (hold | 0x0100);
	if(!written)
		exposureFail(2, 0x3022, hold | 0x0100, hold);
	for(unsigned i = 0; written && i < CORE_REG_COUNT; i++) {
		if(aptReadRegister(currentDevice, s_coreRegs[i]) == values[i])
			continue;
		written = aptWriteRegister(currentDevice, s_coreRegs[i], values[i]) != 0;
		if(!written)
			exposureFail(2, s_coreRegs[i], values[i], 0xFFFF);
	}
	bool released = false;
	for(int i = 0; i < 3 && !released; i++)
		released = aptWriteRegister(currentDevice, 0x3022, hold) && aptReadRegister(currentDevice, 0x3022) == hold;
	if(!released)
		exposureFail(2, 0x3022, hold, aptReadRegister(currentDevice, 0x3022));
	if(!written || !released)
		return false;
	// Core writes can latch at the next sensor frame, not the next LCD VBlank.
	for(int attempt = 0; attempt <= 180; attempt++) {
		unsigned i;
		u16 observed = 0;
		for(i = 0; i < CORE_REG_COUNT; i++) {
			observed = aptReadRegister(currentDevice, s_coreRegs[i]);
			if(observed != values[i])
				break;
		}
		if(i == CORE_REG_COUNT)
			return true;
		if(attempt == 180)
			return exposureFail(5, s_coreRegs[i], values[i], observed);
		swiWaitForVBlank();
	}
	return false;
}

static bool exposureRestore(int cam) {
	if(!s_manual[cam])
		return true;
	if(s_coreValid[cam] && !exposureWriteCore(s_coreValues[cam]))
		return false;
	// Restore SEQ_MODE last, after the core and state presets are consistent.
	for(unsigned i = 0; i < AUTO_REG_COUNT; i++)
		if(!exposureWriteMcu(s_autoRegs[i], s_autoValues[cam][i]))
			return false;
	s_manual[cam] = s_coreValid[cam] = false;
	return true;
}

bool setExposure(unsigned lines, unsigned gain) {
	for(unsigned i = 0; i < EXPOSURE_ERROR_FIELDS; i++)
		s_exposureError[i] = 0;
	if(lines > CAM_EXPOSURE_MAX_LINES || gain > 3)
		return exposureFail(6, 0x3012, CAM_EXPOSURE_MAX_LINES, lines);
	int cam = currentDevice == I2C_CAM0 ? 0 : 1;
	if(!lines)
		return exposureRestore(cam);
	if(!s_manual[cam]) {
		for(unsigned i = 0; i < AUTO_REG_COUNT; i++)
			if(!exposureReadMcu(s_autoRegs[i], &s_autoValues[cam][i]))
				return false;
		s_manual[cam] = true;
	}
	if(!exposureFreeze(cam))
		return false;
	if(!s_coreValid[cam]) {
		// Finish any auto-function work already in flight before sampling color.
		for(int i = 0; i < 12; i++)
			swiWaitForVBlank();
		for(unsigned i = 0; i < CORE_REG_COUNT; i++) {
			u16 value = aptReadRegister(currentDevice, s_coreRegs[i]);
			if(value == 0xFFFF || (i >= 2 && (value < 8 || value > 127)))
				return exposureFail(1, s_coreRegs[i], i >= 2 ? 8 : 0, value);
			s_coreValues[cam][i] = value;
		}
		s_coreValid[cam] = true;
	}
	u16 values[CORE_REG_COUNT];
	values[0] = lines;
	values[1] = 0x006B;
	// Preserve the sensor's calibrated color ratios. Writing GLOBAL_GAIN while
	// AWB runs both destroys those ratios and makes gain readback unstable.
	unsigned green   = s_coreValues[cam][2];
	unsigned factor  = ((8u << gain) * 256 + green / 2) / green;
	unsigned minimum = 0, maximum = 0xFFFF;
	for(unsigned i = 2; i < CORE_REG_COUNT; i++) {
		unsigned base = s_coreValues[cam][i];
		unsigned low = (8 * 256 + base - 1) / base, high = 127 * 256 / base;
		if(low > minimum)
			minimum = low;
		if(high < maximum)
			maximum = high;
	}
	if(factor < minimum)
		factor = minimum;
	if(factor > maximum)
		factor = maximum;
	for(unsigned i = 2; i < CORE_REG_COUNT; i++) {
		unsigned value = (s_coreValues[cam][i] * factor + 128) / 256;
		// The DSi rounds odd analog codes down even at low gain (13 -> 12).
		// Higher ranges have a four-code step. Verify the achievable value.
		value &= value >= 64 ? ~3u : ~1u;
		values[i] = value;
	}
	return exposureWriteCore(values);
}
