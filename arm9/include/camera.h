#ifndef CAMERA_H
#define CAMERA_H

#include "pxi_vars.h"

#include <nds/ndstypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REG_CAM_MCNT *(vu16 *)0x04004200
#define REG_CAM_CNT *(vu16 *)0x04004202
#define REG_CAM_DAT *(vu16 *)0x04004204
#define REG_NDMA1SAD *(vu32 *)0x04004120
#define REG_NDMA1DAD *(vu32 *)0x04004124
#define REG_NDMA1TCNT *(vu32 *)0x04004128
#define REG_NDMA1WCNT *(vu32 *)0x0400412C
#define REG_NDMA1BCNT *(vu32 *)0x04004130
#define REG_NDMA1CNT *(vu32 *)0x04004138

// Preview frame (256x192 RGB555) in bytes and NDMA words.
#define CAM_PREVIEW_SIZE (256 * 192 * 2)
#define CAM_PREVIEW_WORDS (CAM_PREVIEW_SIZE / 4)

typedef enum { CAM_NONE, CAM_INNER, CAM_OUTER } Camera;

// Initializes the cameras
bool cameraInit(void);

// Activates a camera, the other is automatically deactivated if active
bool cameraActivate(Camera cam);

// Deactivates a camera
bool cameraDeactivate(Camera cam);

// Grabs a single frame into dst (RAM or VRAM) and leaves the camera stopped.
// The camera interface is (re)configured per call, so this is cheap only for
// snapshots. Returns false if the transfer did not finish (camera stalled).
bool cameraCapture(u16 *dst, CaptureMode mode);

// Continuous preview capture: the camera interface is started once and NDMA1
// is re-armed from its completion IRQ into one of two internal frame buffers,
// so the sensor never has to be stopped/flushed between frames. Only preview
// mode (256x192) is supported; call cameraCapture for 640x480 shots.
bool cameraStreamStart(void);

// Stops continuous capture (safe to call when not streaming).
void cameraStreamStop(void);

// True while the continuous stream is armed. Single source of truth for
// whether the viewfinder should be producing frames.
bool cameraStreamRunning(void);

// Returns the newest completed frame, or NULL if no new frame arrived since the
// last call. The pointer is one of two buffers that the camera alternates
// between, so it stays valid until roughly one frame period later - copy out
// what you need right away. Cache is invalidated for you.
u16 *cameraNextFrame(void);

// Number of camera FIFO overruns (data dropped between re-arms) since the
// stream started; nonzero means a frame or two of the stream was lost.
u32 cameraStreamOverruns(void);

#ifdef __cplusplus
}
#endif

#endif // CAMERA_H
