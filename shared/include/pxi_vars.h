#ifndef PXIVARS_H
#define PXIVARS_H

#ifdef __cplusplus
extern "C" {
#endif

#define PXI_CAMERA PxiChannel_User0

typedef enum {
	CAM_INIT,
	CAM0_ACTIVATE,
	CAM0_DEACTIVATE,
	CAM1_ACTIVATE,
	CAM1_DEACTIVATE,
	CAM_SET_MODE_PREVIEW,
	CAM_SET_MODE_CAPTURE,
	// The 16-byte DSi camera key (BIOS-derived) sent as 8 halfwords, because
	// PXI simple immediates are only 26-bit (a full 32-bit word gets truncated).
	CAM_GET_KEY0,
	CAM_GET_KEY1,
	CAM_GET_KEY2,
	CAM_GET_KEY3,
	CAM_GET_KEY4,
	CAM_GET_KEY5,
	CAM_GET_KEY6,
	CAM_GET_KEY7,
	// Custom mic recorder: the ARM7 NDMA-drains the MICEX FIFO into a ring of
	// buffers in main RAM; the ARM9 polls a completion counter. No PXI traffic
	// per buffer (SD transactions would starve it and the calico mic driver
	// would permanently stall). The ring lives in the ARM9 binary; its address
	// is sent to the ARM7 as two halfwords before CAM_MIC_START.
	CAM_MIC_ADDR_LO,
	CAM_MIC_ADDR_HI,
	CAM_MIC_START,
	CAM_MIC_STOP,
	// Power LED (BPTWL reg 0x63): OFF = blue, ON/BLINK = red (shutter/REC cue).
	CAM_LED_OFF,
	CAM_LED_ON,
	CAM_LED_BLINK,
	CAM_GET_EXPOSURE_ERROR_STAGE,
	CAM_GET_EXPOSURE_ERROR_REGISTER,
	CAM_GET_EXPOSURE_ERROR_EXPECTED,
	CAM_GET_EXPOSURE_ERROR_OBSERVED,
	CAM_REAR_LIGHT_OFF,
	CAM_REAR_LIGHT_ON
} PxiCommand;

// Packed exposure request fits PXI's 26-bit immediate; halfword mic addresses
// cannot be mistaken for one. Zero lines restores the sensor's auto exposure.
#define CAM_EXPOSURE_REQUEST (1u << 24)
#define CAM_EXPOSURE_LINES_MASK 0x3FFu
#define CAM_EXPOSURE_GAIN_SHIFT 10
#define CAM_EXPOSURE_MAX_LINES 640

typedef enum {
	EXPOSURE_ERROR_STAGE,
	EXPOSURE_ERROR_REGISTER,
	EXPOSURE_ERROR_EXPECTED,
	EXPOSURE_ERROR_OBSERVED,
	EXPOSURE_ERROR_FIELDS
} ExposureErrorField;

// Mic ring layout: u32 completion counter at the base, then MIC_RING_BUFS
// buffers of MIC_BUF_BYTES each (base + 32).
#define MIC_RING_BUFS 4
#define MIC_BUF_BYTES 16384 // 8192 samples pcm16 = 0.5s at 16364Hz

typedef enum {
	CAPTURE_MODE_PREVIEW = 1, // 256x192
	CAPTURE_MODE_CAPTURE = 2  // 640x480
} CaptureMode;

#ifdef __cplusplus
}
#endif

#endif // PXIVARS_H
