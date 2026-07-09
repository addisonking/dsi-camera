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
	CAM_GET_KEY7
} PxiCommand;

typedef enum {
	CAPTURE_MODE_PREVIEW = 1, // 256x192
	CAPTURE_MODE_CAPTURE = 2  // 640x480
} CaptureMode;

#ifdef __cplusplus
}
#endif

#endif // PXIVARS_H
