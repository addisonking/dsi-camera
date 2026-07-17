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
	CAM_MIC_STOP
} PxiCommand;

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
