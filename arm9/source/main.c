#include "camera.h"
#include "night.h"
#include "ui_font.h"
#include "version.h"
#include "white_balance.h"

#include <calico/arm/cache.h>
#include <calico/nds/pm.h>
#include <calico/nds/pxi.h>
#include <calico/system/mailbox.h>
#include <calico/system/thread.h>
#include <dirent.h>
#include <fat.h>
#include <math.h>
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "dsi_photo.h"
#include "pit.h"
#include "stb_image_write.h"

// Photos are captured as raw YUV422 dumps (no encoding, instant save) and
// browsed with the built-in viewer. Exporting to the stock DSi Album (JPEG +
// signature + pit.bin) is done on demand from the viewer.
#define RAW_DIR "/photos"
#define RAW_SIZE (640 * 480 * 2)
#define EXPORT_FALLBACK_DIR "/DCIM/100NIN02"

#define YUV_TO_R(Y, Cr) clamp(Y + Cr + (Cr >> 2) + (Cr >> 3) + (Cr >> 5), 0, 0xFF)
#define YUV_TO_G(Y, Cb, Cr) \
	clamp(Y - ((Cb >> 2) + (Cb >> 4) + (Cb >> 5)) - ((Cr >> 1) + (Cr >> 3) + (Cr >> 4) + (Cr >> 5)), 0, 0xFF)
#define YUV_TO_B(Y, Cb) clamp(Y + Cb + (Cb >> 1) + (Cb >> 2) + (Cb >> 6), 0, 0xFF)

int clamp(int val, int min, int max) { return val < min ? min : (val > max ? max : val); }

static u16 *s_subGfx;    // bottom-screen bitmap, drawn by the ui* helpers
static Camera s_camera;  // currently active camera
static bool s_videoMode; // camera screen: photo vs video mode
static const unsigned s_shutterLines[] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 640};
#define SHUTTER_COUNT (sizeof(s_shutterLines) / sizeof(s_shutterLines[0]))
static unsigned s_shutter;
static unsigned s_gain;
static int s_warmth;
static bool s_nightBurst;
static bool s_rearLight = true;
static bool s_timestamp;    // burn the capture time into saved pixels
static char s_savedMsg[96]; // last "saved video" summary, kept after the redraw
static void uiStatus(const char *s);
static void uiRecordingScreen(void);
static void watermarkYuv(u16 *yuv, time_t t);
static void watermarkFrame(u16 *gfx, time_t t);
static void drainJobs(void);

static u16 uiCameraTap(int x, int y, bool fatInited) {
	if(x >= 170 && y >= 8 && y <= 34)
		return KEY_A;
	if(!fatInited)
		return 0;
	if(y >= 8 && y <= 34 && x >= 12 && x <= 112)
		return (x >= 61) != s_videoMode ? KEY_Y : 0;
	if(x >= 64 && x <= 192 && y >= 88 && y <= 120)
		return KEY_L;
	if(y >= 140 && y <= 161) {
		if(x >= 12 && x <= 96)
			return KEY_SELECT;
		if(x >= 174 && x <= 244)
			return KEY_X;
	}
	return 0;
}

static u16 uiConfirmTap(int x, int y) {
	if(y < 108 || y > 129)
		return 0;
	if(x >= 25 && x < 119)
		return KEY_B;
	if(x >= 133 && x < 231)
		return KEY_A;
	return 0;
}

static bool captureCancelled(void) {
	scanKeys();
	return (keysDown() & (KEY_B | KEY_TOUCH)) || (keysHeld() & KEY_LID) || pmShouldReset();
}

// The viewfinder is a continuous camera stream now. camera.c owns the state;
// these wrappers just avoid restarting a stream that is already running.
static void streamStart(void) {
	bool lightReady = cameraSetRearLight(s_rearLight);
	if(!cameraStreamRunning() && !cameraStreamStart())
		uiStatus(lightReady ? "PREVIEW FAILED - TRY AGAIN" : "PREVIEW AND REAR LIGHT FAILED");
	else if(!lightReady)
		uiStatus("REAR LIGHT SETTING FAILED");
}

static void streamStop(void) { cameraStreamStop(); }

// Lid closed: shut the camera off and sleep until it reopens. Usable from any
// screen; VRAM survives sleep so nothing needs redrawing after.
static void lidSleep(void) {
	bool wasStreaming = cameraStreamRequested();
	streamStop();
	drainJobs();
	if(pmShouldReset())
		return;
	cameraDeactivate(s_camera);
	// Saving and sensor standby can take long enough for the lid to reopen.
	scanKeys();
	if((keysHeld() & KEY_LID) && !pmShouldReset())
		pmEnterSleep();
	if(pmShouldReset())
		return;
	cameraActivate(s_camera);
	if(wasStreaming)
		streamStart();
}

// --- Video recording: 256x192 RGB555 frames from preview mode + mic audio,
// --- written as a custom .VID container by the worker thread.
//
// VID_MAGIC "DSIVID02" video chunks carry the capture time in milliseconds in
// their chunk index field, so playback is paced by when each frame was really
// taken. "DSIVID01" files (frame index only) still play.

#define VID_FPS 10 // capture throttle; the sensor itself runs slower than this
#define VID_W 256
#define VID_H 192
#define VID_FRAME_SIZE (VID_W * VID_H * 2)
#define VID_SLOTS 24 // ~2.4MB ring; a frame is dropped if it fills
#define AUD_CHUNKS 16

#define VID_MAGIC "DSIVID02"
#define VID_MAGIC_OLD "DSIVID01"
#define CHUNK_VIDEO 1
#define CHUNK_AUDIO 2
#define MIC_RATE_HZ 16364 // MICEX rate div1

// Mic ring shared with the ARM7 (see pxi_vars.h); ARM9 polls the completion
// counter and copies finished buffers out. Lives in this binary; the address
// is sent to the ARM7 before each CAM_MIC_START.
alignas(32) static u8 s_micRing[32 + MIC_RING_BUFS * MIC_BUF_BYTES];
static vu32 *s_micDone = (vu32 *)s_micRing;
static u32 s_micLastDone;

// Draws a raw 640x480 YUV422 frame downscaled (nearest-neighbour) to a
// dw x dh RGB555 image in `dst` with row stride `stride`.
static void blitYuvScaled(const u16 *yuv, u16 *dst, int stride, int dw, int dh) {
	for(int y = 0; y < dh; y++) {
		int sy = (y * 480) / dh;
		for(int x = 0; x < dw; x++) {
			int sx  = (x * 640) / dw & ~1; // keep YUV pair alignment
			u8 *val = (u8 *)(yuv + sy * 640 + sx);
			int Y   = val[(x & 1) ? 2 : 0];
			int Cb  = val[1] - 0x80;
			int Cr  = val[3] - 0x80;
			int r   = YUV_TO_R(Y, Cr) >> 3;
			int g   = YUV_TO_G(Y, Cb, Cr) >> 3;
			int b   = YUV_TO_B(Y, Cb) >> 3;
			// NDS 16-bit bitmap is aBBBBBGGGGGRRRRR (blue high, red low)
			dst[y * stride + x] = BIT(15) | (b << 10) | (g << 5) | r;
		}
	}
}

static void blitYuv(const u16 *yuv, u16 *gfx) { blitYuvScaled(yuv, gfx, 256, 256, 192); }

static void yuvToRgb(const u16 *yuv, u8 *rgb) {
	for(int py = 0; py < 480; py++) {
		for(int px = 0; px < 640; px += 2) {
			u8 *val = (u8 *)(yuv + py * 640 + px);
			int Y1  = val[0];
			int Cb  = val[1] - 0x80;
			int Y2  = val[2];
			int Cr  = val[3] - 0x80;

			u8 *dst = rgb + py * (640 * 3) + px * 3;
			dst[0]  = YUV_TO_R(Y1, Cr);
			dst[1]  = YUV_TO_G(Y1, Cb, Cr);
			dst[2]  = YUV_TO_B(Y1, Cb);
			dst[3]  = YUV_TO_R(Y2, Cr);
			dst[4]  = YUV_TO_G(Y2, Cb, Cr);
			dst[5]  = YUV_TO_B(Y2, Cb);
		}
	}
}

static void rawPath(char *out, int num) { sprintf(out, RAW_DIR "/IMG_%04d.YUV", num); }
static void thumbPath(char *out, int num) { sprintf(out, RAW_DIR "/IMG_%04d.THM", num); }
static void vidPath(char *out, int num) { sprintf(out, RAW_DIR "/VID_%04d.VID", num); }
static void vidThumbPath(char *out, int num) { sprintf(out, RAW_DIR "/VID_%04d.THM", num); }

// --- Settings: a small key=value file beside the photos so toggles survive a
// --- reboot. Read once at boot, rewritten whenever an option changes.
#define CFG_PATH RAW_DIR "/settings.cfg"

static void cfgLoad(void) {
	FILE *f = fopen(CFG_PATH, "r");
	if(!f)
		return;
	char line[64];
	while(fgets(line, sizeof(line), f)) {
		if(strncmp(line, "timestamp=", 10) == 0)
			s_timestamp = atoi(line + 10) != 0;
		else if(strncmp(line, "shutter=", 8) == 0)
			s_shutter = (unsigned)clamp(atoi(line + 8), 0, SHUTTER_COUNT - 1);
		else if(strncmp(line, "gain=", 5) == 0)
			s_gain = (unsigned)clamp(atoi(line + 5), 0, 3);
		else if(strncmp(line, "warmth=", 7) == 0)
			s_warmth = clamp(atoi(line + 7), -3, 3);
		else if(strncmp(line, "night_burst=", 12) == 0)
			s_nightBurst = atoi(line + 12) != 0;
		else if(strncmp(line, "rear_light=", 11) == 0)
			s_rearLight = atoi(line + 11) != 0;
	}
	fclose(f);
}

static void cfgSave(void) {
	FILE *f = fopen(CFG_PATH, "w");
	if(!f)
		return;
	fprintf(f,
			"timestamp=%d\nshutter=%u\ngain=%u\nwarmth=%d\nnight_burst=%d\nrear_light=%d\n",
			s_timestamp ? 1 : 0,
			s_shutter,
			s_gain,
			s_warmth,
			s_nightBurst ? 1 : 0,
			s_rearLight ? 1 : 0);
	fclose(f);
}

// Album grid layout: 4x3 cells of 64x48 thumbnails (raw RGB555, cached as
// .THM files beside the raws so the grid doesn't have to read 600KB per cell).
#define THUMB_W 64
#define THUMB_H 48
#define GRID_COLS 4
#define GRID_ROWS 3
#define GRID_PER_PAGE (GRID_COLS * GRID_ROWS)

typedef struct Photo {
	int num;
	time_t ts;    // capture time = the raw file's FAT mtime
	bool isVideo; // VID_%04d.VID instead of IMG_%04d.YUV
	u16 durSec;   // video duration in seconds (0 for photos)
} Photo;

static int cmpPhoto(const void *a, const void *b) {
	const Photo *x = (const Photo *)a, *y = (const Photo *)b;
	if(x->ts != y->ts)
		return x->ts < y->ts ? -1 : 1;
	return x->num - y->num;
}

// List of the photos in RAW_DIR, sorted oldest-first. Caller frees *outPhotos.
static int scanRaw(Photo **outPhotos) {
	int cap = 64, count = 0;
	Photo *ph = (Photo *)malloc(cap * sizeof(Photo));

	DIR *pdir = opendir(RAW_DIR);
	if(pdir) {
		struct dirent *pent;
		while((pent = readdir(pdir))) {
			bool isVideo = strncmp(pent->d_name, "VID_", 4) == 0 && strstr(pent->d_name, ".VID");
			if(!isVideo && (strncmp(pent->d_name, "IMG_", 4) != 0 || !strstr(pent->d_name, ".YUV")))
				continue;
			if(count == cap)
				ph = (Photo *)realloc(ph, (cap *= 2) * sizeof(Photo));
			ph[count].num     = atoi(pent->d_name + 4);
			ph[count].isVideo = isVideo;
			ph[count].durSec  = 0;
			char path[40];
			if(isVideo)
				vidPath(path, ph[count].num);
			else
				rawPath(path, ph[count].num);
			struct stat st;
			ph[count].ts = stat(path, &st) == 0 ? st.st_mtime : 0;
			if(isVideo) {
				// Duration from the container header, for the album grid.
				FILE *vf = fopen(path, "rb");
				if(vf) {
					u32 vh[10];
					if(fread(vh, sizeof(vh), 1, vf) == 1 &&
					   (memcmp(vh, VID_MAGIC, 8) == 0 || memcmp(vh, VID_MAGIC_OLD, 8) == 0)) {
						// DSIVID02 stores the real length; DSIVID01 and
						// interrupted recordings fall back to frames/fps.
						bool hasDuration = memcmp(vh, VID_MAGIC, 8) == 0 && vh[5] > 1;
						u32 vsec         = hasDuration ? (u32)(((u64)vh[5] + 500) / 1000) : (vh[4] ? vh[8] / vh[4] : 0);
						u32 asec         = vh[6] ? vh[9] / vh[6] : 0;
						ph[count].durSec = (u16)(vsec > asec ? vsec : asec);
					}
					fclose(vf);
				}
			}
			count++;
		}
		closedir(pdir);
	}

	qsort(ph, count, sizeof(Photo), cmpPhoto);
	*outPhotos = ph;
	return count;
}

static int dayKey(time_t t) {
	struct tm *lt = localtime(&t);
	return lt ? lt->tm_year * 1000 + lt->tm_yday : 0;
}

// Async save pipeline: the main thread captures raw YUV frames and queues
// them here; a lower-priority worker thread writes them to SD in the
// background so burst shots are only limited by capture time. The same queue
// carries video-frame and audio chunks while recording.
#define JOB_PHOTO 0
// (JOB_ values 1 and 2 are CHUNK_VIDEO / CHUNK_AUDIO)

typedef struct Job {
	u32 type; // JOB_PHOTO / CHUNK_VIDEO / CHUNK_AUDIO
	u32 aux;  // photo num / chunk index (frame or audio block number)
	u32 slot; // ring/pool slot to return after writing (video/audio)
	u32 size; // payload bytes (video/audio)
	u16 *buf;
} Job;

#define JOB_QUEUE_LEN 32

static Thread s_workerThread;
alignas(8) static u8 s_workerStack[32 * 1024];
static Mailbox s_jobMailbox;
static u32 s_jobSlots[JOB_QUEUE_LEN];
// Single-writer counters (main queues, worker finishes) so no atomics needed.
static vu32 s_jobsQueued = 0;
static vu32 s_jobsDone   = 0;
#define JOBS_PENDING (s_jobsQueued - s_jobsDone)

static int s_nextRawNum = 1;
static int s_nextVidNum = 1;
static u8 s_camKey[16];

// Video frame ring (producer: record loop; consumer: worker).
static u16 s_vidRing[VID_SLOTS][VID_W * VID_H];
static Job s_vidJobs[VID_SLOTS];
static vu8 s_vidFree[VID_SLOTS]; // SPSC ring of free slot indices
static vu32 s_vidFreeHead = 0;   // written by worker
static vu32 s_vidFreeTail = 0;   // written by record loop

// Audio chunk pool (producer: record loop via micDrain; consumer: worker).
typedef struct AudioChunk {
	Job job;
	alignas(32) u8 data[MIC_BUF_BYTES];
} AudioChunk;
static AudioChunk s_audPool[AUD_CHUNKS];
static vu8 s_audFree[AUD_CHUNKS];
static vu32 s_audFreeHead = 0;
static vu32 s_audFreeTail = 0;

static FILE *s_vidFile    = NULL;
static vu32 s_audBlockIdx = 0;
static vu32 s_audDropped  = 0;

// Copies every mic buffer the ARM7 has completed into pooled audio chunks and
// queues them for the worker. Called from the record loop; never blocks.
static void micDrain(void) {
	armDCacheInvalidate(s_micRing, 32);
	u32 done = *s_micDone;
	while(s_micLastDone < done) {
		u8 *src = s_micRing + 32 + (s_micLastDone % MIC_RING_BUFS) * MIC_BUF_BYTES;
		armDCacheInvalidate(src, MIC_BUF_BYTES);
		if(s_audFreeTail == s_audFreeHead + AUD_CHUNKS) {
			s_audDropped++;
		} else {
			int idx       = s_audFree[s_audFreeTail++ % AUD_CHUNKS];
			AudioChunk *c = &s_audPool[idx];
			memcpy(c->data, src, MIC_BUF_BYTES);
			c->job.type = CHUNK_AUDIO;
			c->job.aux  = s_audBlockIdx++;
			c->job.slot = (u32)idx;
			c->job.size = MIC_BUF_BYTES;
			c->job.buf  = (u16 *)c->data;
			if(!mailboxTrySend(&s_jobMailbox, (u32)&c->job)) {
				s_audFreeTail--; // queue full, give the chunk back
				s_audBlockIdx--;
				s_audDropped++;
			} else {
				s_jobsQueued++;
			}
		}
		s_micLastDone++;
	}
}

static int workerMain(void *arg) {
	for(;;) {
		Job *job = (Job *)mailboxRecv(&s_jobMailbox);

		if(job->type == JOB_PHOTO) {
			char path[40];
			rawPath(path, job->aux);
			FILE *f = fopen(path, "wb");
			char msg[32];
			bool saved = false;
			if(f) {
				saved = fwrite(job->buf, 1, RAW_SIZE, f) == RAW_SIZE;
				if(fclose(f) != 0)
					saved = false;
				if(!saved)
					remove(path);
			}
			snprintf(msg, sizeof(msg), saved ? "Saved IMG_%04d" : "Could not save IMG_%04d", (int)job->aux);
			uiStatus(msg);

			// Thumbnail cache for the album grid.
			u16 thumb[THUMB_W * THUMB_H];
			blitYuvScaled(job->buf, thumb, THUMB_W, THUMB_W, THUMB_H);
			thumbPath(path, job->aux);
			f = saved ? fopen(path, "wb") : NULL;
			if(f) {
				fwrite(thumb, 1, sizeof(thumb), f);
				fclose(f);
			}

			free(job->buf);
			free(job);
		} else {
			// Video/audio chunk: header + payload into the open .VID file.
			if(s_vidFile) {
				u32 hdr[3] = {job->type, job->aux, job->size};
				fwrite(hdr, sizeof(hdr), 1, s_vidFile);
				fwrite(job->buf, job->size, 1, s_vidFile);
			}
			// Publish the slot before the counter: the main thread can preempt us.
			if(job->type == CHUNK_VIDEO) {
				s_vidFree[s_vidFreeHead % VID_SLOTS] = (u8)job->slot;
				s_vidFreeHead++;
			} else {
				s_audFree[s_audFreeHead % AUD_CHUNKS] = (u8)job->slot;
				s_audFreeHead++;
			}
		}
		s_jobsDone++;
	}
	return 0;
}

// Blocks until every queued frame has been written out. Call before exiting,
// sleeping, or opening the viewer so the photo list is complete.
static void drainJobs(void) {
	if(JOBS_PENDING)
		uiStatus("Saving...");
	while(JOBS_PENDING)
		swiWaitForVBlank();
}

// Night source frames remain in RAM; only the completed photo is queued.
static bool captureRaw(u16 *previewGfx) {
	u16 *yuv = NULL;
	Job *job = NULL;
	if(s_nightBurst)
		drainJobs();
	for(;;) {
		if(!yuv)
			yuv = (u16 *)memalign(32, RAW_SIZE);
		if(!job)
			job = (Job *)malloc(sizeof(Job));
		if(yuv && job)
			break;
		if(!JOBS_PENDING) {
			free(yuv);
			free(job);
			uiStatus("Not enough memory. Try again.");
			return false;
		}
		swiWaitForVBlank();
	}

	time_t shotTime   = time(NULL);
	NightStack *stack = NULL;
	u16 *next         = NULL;
	const char *error = "CAMERA FAILED - TRY AGAIN";
	if(s_nightBurst)
		uiStatus("Hold still  1/4  B/tap: cancel");
	if(!cameraCapture(yuv, CAPTURE_MODE_CAPTURE))
		goto fail;
	memcpy(yuv, yuv + 640, 640 * sizeof(u16));

	if(s_nightBurst) {
		next  = (u16 *)memalign(32, RAW_SIZE);
		stack = nightCreate((const u8 *)yuv);
		if(!next || !stack) {
			error = "Not enough memory. Try again.";
			goto fail;
		}
		for(int i = 1; i < NIGHT_FRAMES; i++) {
			if(captureCancelled()) {
				error = "Night Burst cancelled";
				goto fail;
			}
			char msg[40];
			sprintf(msg, "Hold still  %d/4  B/tap: cancel", i + 1);
			uiStatus(msg);
			if(!cameraCapture(next, CAPTURE_MODE_CAPTURE))
				goto fail;
			memcpy(next, next + 640, 640 * sizeof(u16));
			nightAdd(stack, (const u8 *)next);
		}
		if(captureCancelled()) {
			error = "Night Burst cancelled";
			goto fail;
		}
		uiStatus("Processing...");
		nightFinish(stack, (u8 *)yuv);
		nightFree(stack);
		free(next);
	}
	whiteBalanceYuv((u8 *)yuv, 640 * 480, s_warmth);
	if(s_timestamp)
		watermarkYuv(yuv, shotTime);
	if(previewGfx)
		blitYuv(yuv, previewGfx);

	job->type = JOB_PHOTO;
	job->aux  = s_nextRawNum++;
	job->buf  = yuv;
	while(!mailboxTrySend(&s_jobMailbox, (u32)job))
		swiWaitForVBlank();
	s_jobsQueued++;
	return true;

fail:
	nightFree(stack);
	free(next);
	free(yuv);
	free(job);
	uiStatus(error);
	return false;
}

static void takePhotos(u16 *gfx) {
	streamStop();
	if(s_nightBurst && !cameraSetRearLight(false)) {
		cameraSetRearLight(s_rearLight);
		uiStatus("REAR LIGHT OFF FAILED - NO PHOTO");
		streamStart();
		return;
	}
	pxiSendAndReceive(PXI_CAMERA, CAM_LED_ON);
	do {
		if(!captureRaw(gfx))
			break;
		scanKeys();
	} while(!s_nightBurst && (keysHeld() & (KEY_L | KEY_R)) && !(keysHeld() & KEY_LID) && !pmShouldReset());
	pxiSendAndReceive(PXI_CAMERA, CAM_LED_OFF);
	if(s_nightBurst && !cameraSetRearLight(s_rearLight))
		uiStatus("REAR LIGHT RESTORE FAILED");
	streamStart();
}

// --- Video playback: audio streams through a looping ring while frames are
// --- blitted at their pacing-slot times. Hardware timer 0 free-runs at
// --- BUS_CLOCK/1024 and is the single A/V clock (polled via timerElapsed, no
// --- ISR); it and the sound channel derive from the same crystal, so sound
// --- and picture can't drift apart.

#define PLAY_RING_HALF 8192 // samples per ring half (16KB)
#define PLAY_TIMER_HZ (BUS_CLOCK >> 10)
alignas(32) static s16 s_playRing[2 * PLAY_RING_HALF];
static u32 s_playTicks;

// Reads the next chunk of `wantType` from f, skipping over chunks of the other
// type. Returns false at EOF/corruption.
static bool vidReadChunk(FILE *f, u32 wantType, u32 *outIdx, void *buf, u32 bufSz) {
	for(;;) {
		u32 hdr[3];
		if(fread(hdr, sizeof(hdr), 1, f) != 1)
			return false;
		if(hdr[0] == wantType) {
			// video chunks are fixed size; anything else is corruption
			if(hdr[2] > bufSz || (wantType == CHUNK_VIDEO && hdr[2] != VID_FRAME_SIZE))
				return false;
			*outIdx = hdr[1];
			return fread(buf, hdr[2], 1, f) == 1;
		}
		fseek(f, hdr[2], SEEK_CUR);
	}
}

// Reads a video's first frame into gfx (which is also its display bitmap).
static bool vidReadFirstFrame(FILE *f, u16 *gfx) {
	u32 hdr[10];
	if(fread(hdr, sizeof(hdr), 1, f) != 1)
		return false;
	if(memcmp(hdr, VID_MAGIC, 8) != 0 && memcmp(hdr, VID_MAGIC_OLD, 8) != 0)
		return false;
	u32 idx;
	return vidReadChunk(f, CHUNK_VIDEO, &idx, gfx, VID_FRAME_SIZE);
}

static void playVideo(u16 *gfx, int num) {
	char path[40];
	vidPath(path, num);
	FILE *fv = fopen(path, "rb");
	if(!fv) {
		uiStatus("CANT OPEN FILE");
		return;
	}
	u32 hdr[10];
	if(fread(hdr, sizeof(hdr), 1, fv) != 1) {
		fclose(fv);
		uiStatus("BAD VIDEO FILE");
		return;
	}
	// DSIVID02 stamps each video chunk with its capture time in ms; the older
	// DSIVID01 used a frame index paced at a fixed fps.
	bool timed = memcmp(hdr, VID_MAGIC, 8) == 0;
	if(!timed && memcmp(hdr, VID_MAGIC_OLD, 8) != 0) {
		fclose(fv);
		uiStatus("BAD VIDEO FILE");
		return;
	}
	u32 fps          = hdr[4] ? hdr[4] : VID_FPS;
	u32 rate         = hdr[6] ? hdr[6] : MIC_RATE_HZ;
	u32 totalFrames  = hdr[8];
	u32 totalSamples = hdr[9];
	FILE *fa         = fopen(path, "rb");
	if(!fa) {
		fclose(fv);
		uiStatus("CANT OPEN FILE");
		return;
	}
	fseek(fa, sizeof(hdr), SEEK_SET);

	uiStatus("Loading...");

	// Prebuffer two ring halves of audio (zero-padded = silence).
	memset(s_playRing, 0, sizeof(s_playRing));
	u32 fedChunks  = 0, aIdx;
	bool audioMore = true;
	while(fedChunks < 2 && audioMore) {
		audioMore = vidReadChunk(fa, CHUNK_AUDIO, &aIdx, s_playRing + fedChunks * PLAY_RING_HALF, PLAY_RING_HALF * 2);
		if(audioMore)
			fedChunks++;
	}
	armDCacheFlush(s_playRing, sizeof(s_playRing));

	// Show the first frame immediately; stage the next one.
	u16 *stage       = (u16 *)malloc(VID_FRAME_SIZE);
	u32 vIdx         = 0;
	u32 stageMs      = 0;
	bool videoMore   = vidReadChunk(fv, CHUNK_VIDEO, &vIdx, gfx, VID_FRAME_SIZE);
	bool haveFrame   = videoMore;
	bool stageFilled = false;
	if(videoMore) {
		u32 next;
		if(vidReadChunk(fv, CHUNK_VIDEO, &next, stage, VID_FRAME_SIZE)) {
			stageMs     = timed ? next : (u32)((u64)next * 1000 / fps);
			stageFilled = true;
		} else {
			videoMore = false;
		}
	}

	if(!haveFrame && totalSamples == 0) {
		uiStatus("EMPTY VIDEO");
		fclose(fa);
		fclose(fv);
		free(stage);
		return;
	}

	// Start the audio clock and the looping channel together.
	soundSetMixerVolume(127);
	s_playTicks = 0;
	soundPreparePcm(4 | SOUND_START,
					2047,
					64,
					soundTimerFromHz(rate),
					SoundMode_Repeat,
					SoundFmt_Pcm16,
					s_playRing,
					0,
					sizeof(s_playRing) / 4);
	timerStart(0, ClockDivider_1024, 0, NULL); // free-running counter
	timerElapsed(0);                           // zero the baseline

	u32 durMs  = timed && hdr[5] > 1 ? hdr[5] : (u32)((u64)totalFrames * 1000 / fps);
	u32 durSec = (u32)(((u64)durMs + 500) / 1000);
	if(totalSamples / rate > durSec)
		durSec = totalSamples / rate;
	int lastSec       = -1;
	bool audioStopped = false;

	for(;;) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & (KEY_B | KEY_A | KEY_START | KEY_SELECT) || (keysHeld() & KEY_LID) || pmShouldReset())
			break;

		s_playTicks += timerElapsed(0);
		u32 played       = (u32)((u64)s_playTicks * rate / PLAY_TIMER_HZ);
		u32 playedHalves = played / PLAY_RING_HALF;

		// Keep the ring filled one half ahead of playback.
		while(audioMore && fedChunks <= playedHalves + 1) {
			audioMore =
				vidReadChunk(fa, CHUNK_AUDIO, &aIdx, s_playRing + (fedChunks % 2) * PLAY_RING_HALF, PLAY_RING_HALF * 2);
			if(audioMore) {
				armDCacheFlush(s_playRing + (fedChunks % 2) * PLAY_RING_HALF, PLAY_RING_HALF * 2);
				fedChunks++;
			}
		}

		// Show the staged frame once its capture time is due.
		u32 playedMs = (u32)((u64)played * 1000 / rate);
		if(stageFilled && playedMs >= stageMs) {
			memcpy(gfx, stage, VID_FRAME_SIZE);
			stageFilled = false;
			u32 next;
			if(vidReadChunk(fv, CHUNK_VIDEO, &next, stage, VID_FRAME_SIZE)) {
				stageMs     = timed ? next : (u32)((u64)next * 1000 / fps);
				stageFilled = true;
			} else {
				videoMore = false;
			}
		}

		bool audioDone = !audioMore && played >= (u64)fedChunks * PLAY_RING_HALF;
		if(audioDone && !audioStopped) {
			// the looping channel would otherwise repeat the last half for as
			// long as the video outlasts the audio
			soundStop(BIT(4));
			audioStopped = true;
		}
		if(!videoMore && !stageFilled && audioDone && playedMs >= durMs)
			break;

		int sec = (int)(played / rate);
		if(sec != lastSec) {
			lastSec = sec;
			char msg[40];
			sprintf(msg,
					"Playing %d:%02d / %lu:%02lu",
					sec / 60,
					sec % 60,
					(unsigned long)(durSec / 60),
					(unsigned long)(durSec % 60));
			uiStatus(msg);
		}
	}

	timerStop(0);
	soundStop(BIT(4));
	fclose(fa);
	fclose(fv);
	free(stage);
	uiStatus("B to return");
}

// Records video + mic audio to a .VID container until L/R is tapped again (or
// the lid closes / power is tapped). Frames are taken at the camera's own pace,
// throttled to VID_FPS, and each one is stamped with its capture time; if the
// SD card falls behind, video frames are dropped (never audio).
static void recordVideo(u16 *gfx, int num) {
	char path[40];
	vidPath(path, num);
	FILE *f = fopen(path, "wb");
	if(!f) {
		uiStatus("CANT OPEN FILE");
		return;
	}

	// Container header; the two totals are patched in at stop.
	u32 hdr[10] = {0};
	memcpy(hdr, VID_MAGIC, 8);
	hdr[2] = VID_W;
	hdr[3] = VID_H;
	hdr[4] = VID_FPS;
	hdr[5] = 0; // clip length in ms, patched in at stop (was the fps denominator)
	hdr[6] = MIC_RATE_HZ;
	hdr[7] = 1; // s16le mono
	fwrite(hdr, sizeof(hdr), 1, f);
	s_vidFile = f;

	for(int i = 0; i < VID_SLOTS; i++)
		s_vidFree[i] = (u8)i;
	for(int i = 0; i < AUD_CHUNKS; i++)
		s_audFree[i] = (u8)i;
	s_vidFreeHead = s_vidFreeTail = 0;
	s_audFreeHead = s_audFreeTail = 0;
	s_audBlockIdx = s_audDropped = 0;
	u32 framesKept = 0, framesDropped = 0;

	// Timestamps come off the same free-running clock playback uses, so frame
	// times can't drift against the audio the way a vblank count does. Start it
	// before the mic so audio can't lead video t=0.
	timerStart(0, ClockDivider_1024, 0, NULL);
	timerElapsed(0);
	u32 recTicks = 0;

	// Start the ARM7 mic recorder (it NDMA-drains MICEX into the shared ring).
	pmMicSetAmp(true, PmMicGain_80);
	armDCacheFlush(s_micRing, sizeof(s_micRing));
	s_micLastDone = 0;
	u32 micAddr   = (u32)s_micRing;
	pxiSendAndReceive(PXI_CAMERA, CAM_MIC_ADDR_LO);
	pxiSendAndReceive(PXI_CAMERA, micAddr & 0xFFFF);
	pxiSendAndReceive(PXI_CAMERA, CAM_MIC_ADDR_HI);
	pxiSendAndReceive(PXI_CAMERA, micAddr >> 16);
	pxiSendAndReceive(PXI_CAMERA, CAM_MIC_START);
	pxiSendAndReceive(PXI_CAMERA, CAM_LED_BLINK); // recording indicator

	// Use the viewfinder stream that is already running. Restarting it here
	// costs a CAM_CNT flush and leaves the first frames blank, which shows up
	// as a freeze right at the start of the clip.
	u32 ovStart = cameraStreamOverruns();
	streamStart();
	uiRecordingScreen();

	u32 keepEvery   = 1000 / VID_FPS;
	u32 nextKeep    = 0;
	u32 lastFrameMs = 0; // newest camera frame, for the worst-gap readout
	u32 maxGapMs    = 0;
	bool firstFrame = true;
	int lastSec     = -1;
	bool stop       = false;
	time_t recTime  = time(NULL); // watermark clock: start + elapsed seconds

	while(!stop) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_TOUCH) {
			touchPosition touch;
			touchRead(&touch);
			if(uiCameraTap(touch.px, touch.py, true) == KEY_L)
				stop = true;
		}
		if(keysDown() & (KEY_L | KEY_R) || (keysHeld() & KEY_LID) || pmShouldReset())
			stop = true;

		recTicks += timerElapsed(0);
		u32 nowMs = (u32)((u64)recTicks * 1000 / PLAY_TIMER_HZ);

		// The camera hands over whole frames; copying one to the screen keeps
		// the viewfinder live, and the throttled copy goes into the ring.
		u16 *frame = cameraNextFrame();
		if(frame) {
			memcpy(gfx, frame, VID_FRAME_SIZE);
			// Worst hole in the stream, including the wait before the first
			// frame: a frozen head shows up here, not in D/O.
			if(!lastFrameMs) {
				if(nowMs > maxGapMs)
					maxGapMs = nowMs;
			} else if(nowMs - lastFrameMs > maxGapMs) {
				maxGapMs = nowMs - lastFrameMs;
			}
			lastFrameMs = nowMs;

			if(firstFrame || nowMs >= nextKeep) {
				firstFrame = false;
				// Fixed schedule, not "100ms since the last kept frame": at a
				// sensor rate just above VID_FPS that would skip every second
				// frame and halve the recorded rate.
				nextKeep += keepEvery;
				if((s32)(nowMs - nextKeep) > 2 * (s32)keepEvery)
					nextKeep = nowMs + keepEvery; // fell behind; don't burst
				if(s_vidFreeTail == s_vidFreeHead + VID_SLOTS) {
					framesDropped++;
				} else {
					int slot = s_vidFree[s_vidFreeTail++ % VID_SLOTS];
					memcpy(s_vidRing[slot], frame, VID_FRAME_SIZE);
					// Stamped on the ring copy, never on gfx: the viewfinder
					// stays clean and the camera DMA can't race the text.
					if(s_timestamp)
						watermarkFrame(s_vidRing[slot], recTime + nowMs / 1000);
					Job *job  = &s_vidJobs[slot];
					job->type = CHUNK_VIDEO;
					job->aux  = nowMs; // capture time, ms into the clip
					job->slot = (u32)slot;
					job->size = VID_FRAME_SIZE;
					job->buf  = s_vidRing[slot];
					if(!mailboxTrySend(&s_jobMailbox, (u32)job)) {
						s_vidFreeTail--; // queue full, give the slot back
						framesDropped++;
					} else {
						s_jobsQueued++;
						framesKept++;
					}
				}
			}
		}

		micDrain();

		int sec = (int)(nowMs / 1000);
		if(sec != lastSec) {
			lastSec = sec;
			char msg[24];
			sprintf(msg, "%d:%02d", sec / 60, sec % 60);
			uiStatus(msg);
		}
	}

	timerStop(0);
	pxiSendAndReceive(PXI_CAMERA, CAM_MIC_STOP);
	pxiSendAndReceive(PXI_CAMERA, CAM_LED_OFF);
	pmMicSetAmp(false, 0);
	micDrain();   // pick up the last completed buffers
	streamStop(); // no waiting on a possibly wedged DMA

	// Let the worker finish the remaining chunks, then patch the totals.
	uiStatus("Saving...");
	while(JOBS_PENDING)
		swiWaitForVBlank();
	u32 durMs = (u32)((u64)recTicks * 1000 / PLAY_TIMER_HZ);
	u32 fps10 = durMs ? (framesKept * 10000 + durMs / 2) / durMs : VID_FPS * 10; // tenths
	if(fps10 == 0)
		fps10 = 1;
	hdr[4] = (fps10 + 5) / 10; // measured fps, for the header
	if(hdr[4] == 0)
		hdr[4] = 1;
	hdr[5] = durMs; // exact length; frames/fps is rounded and reads short
	hdr[8] = framesKept;
	hdr[9] = (s_audBlockIdx * MIC_BUF_BYTES) / 2; // total samples
	fseek(f, 0, SEEK_SET);
	fwrite(hdr, sizeof(hdr), 1, f);
	fclose(f);
	s_vidFile = NULL;

	// Thumbnail from the last viewfinder frame (RGB555 downscale).
	u16 thumb[THUMB_W * THUMB_H];
	for(int y = 0; y < THUMB_H; y++)
		for(int x = 0; x < THUMB_W; x++)
			thumb[y * THUMB_W + x] = gfx[(y * VID_H / THUMB_H) * 256 + x * VID_W / THUMB_W];
	vidThumbPath(path, num);
	f = fopen(path, "wb");
	if(f) {
		fwrite(thumb, 1, sizeof(thumb), f);
		fclose(f);
	}

	// fps and drop counts are the quickest way to see how the capture went.
	// uiCameraScreen() clears the status strip right after this returns, so
	// stash it and let the caller re-print it.
	u32 gapS = maxGapMs / 1000;
	if(gapS > 999)
		gapS = 999;
	char msg[96];
	snprintf(msg,
			 sizeof(msg),
			 "VID_%04d %lu.%luFPS G:%lu.%lus D:%lu A:%lu O:%lu",
			 num,
			 (unsigned long)(fps10 / 10),
			 (unsigned long)(fps10 % 10),
			 (unsigned long)gapS,
			 (unsigned long)((maxGapMs / 100) % 10),
			 (unsigned long)framesDropped,
			 (unsigned long)s_audDropped,
			 (unsigned long)(cameraStreamOverruns() - ovStart));
	uiStatus(msg);
	snprintf(s_savedMsg, sizeof(s_savedMsg), "%s", msg);
}

// Fallback numbering for Album export when pit.bin is unusable.
static int getExportNumber(void) {
	int highest = 0;
	DIR *pdir   = opendir(EXPORT_FALLBACK_DIR);
	if(!pdir)
		return 1;
	struct dirent *pent;
	while((pent = readdir(pdir))) {
		if(strncmp(pent->d_name, "HNI_", 4) == 0) {
			int val = atoi(pent->d_name + 4);
			if(val > highest)
				highest = val;
		}
	}
	closedir(pdir);
	return highest + 1;
}

// Converts one raw photo to a signed, Album-compatible DSi JPEG, registers it
// in pit.bin and saves it under DCIM. Slow (several seconds): JPEG encoding.
static bool exportToAlbum(int num) {
	char path[40];
	rawPath(path, num);

	FILE *f = fopen(path, "rb");
	if(!f)
		return false;
	u16 *yuv = (u16 *)malloc(RAW_SIZE);
	fread(yuv, 1, RAW_SIZE, f);
	fclose(f);

	// Photo timestamp = the raw file's FAT mtime (set when it was captured).
	struct stat st;
	time_t t = (stat(path, &st) == 0 && st.st_mtime > 0) ? st.st_mtime : time(NULL);

	u8 *rgb = (u8 *)malloc(640 * 480 * 3);
	yuvToRgb(yuv, rgb);
	free(yuv);

	char dt[20];
	struct tm *tmv = localtime(&t);
	if(tmv && tmv->tm_year > 100)
		strftime(dt, sizeof(dt), "%Y:%m:%d %H:%M:%S", tmv);
	else
		strcpy(dt, "2024:01:01 00:00:00");

	u32 dsiTs  = (u32)((u32)t - 946684800u); // seconds 1970->2000 epoch shift
	int folder = 100;
	int imgNum = pitAddPhoto(dsiTs, &folder);
	if(imgNum < 0)
		imgNum = getExportNumber();

	// Nonce is stored in the file (not secret); just make it vary.
	u8 nonce[12];
	for(int i = 0; i < 12; i++)
		nonce[i] = (u8)(t >> ((i & 3) * 8)) ^ (u8)(imgNum * (i + 1));

	u8 *photo    = NULL;
	int photoLen = 0;
	buildAndSignDsiPhoto(rgb, dt, s_camKey, nonce, &photo, &photoLen);
	free(rgb);

	char jpgName[40];
	sprintf(jpgName, "/DCIM/%03dNIN02", folder);
	mkdir(jpgName, 0777);
	sprintf(jpgName, "/DCIM/%03dNIN02/HNI_%04d.JPG", folder, imgNum);
	f       = fopen(jpgName, "wb");
	bool ok = f != NULL;
	if(f) {
		fwrite(photo, 1, photoLen, f);
		fclose(f);
	}
	free(photo);

	return ok;
}

// Soft delete: photos move to /photos/trash (never erased); pull them off the
// card or move them back by hand to restore.
static void softDelete(int num, bool isVideo) {
	char from[40], to[52];
	mkdir(RAW_DIR "/trash", 0777);
	if(isVideo) {
		vidPath(from, num);
		sprintf(to, RAW_DIR "/trash/VID_%04d.VID", num);
		rename(from, to);
		vidThumbPath(from, num);
		sprintf(to, RAW_DIR "/trash/VID_%04d.THM", num);
		rename(from, to);
	} else {
		rawPath(from, num);
		sprintf(to, RAW_DIR "/trash/IMG_%04d.YUV", num);
		rename(from, to);
		thumbPath(from, num);
		sprintf(to, RAW_DIR "/trash/IMG_%04d.THM", num);
		rename(from, to);
	}
}

// Waits for A (true) or B (false).
static bool confirmAB(void) {
	for(;;) {
		swiWaitForVBlank();
		scanKeys();
		u16 p = keysDown();
		if(p & KEY_TOUCH) {
			touchPosition touch;
			touchRead(&touch);
			p |= uiConfirmTap(touch.px, touch.py);
		}
		if(p & KEY_A)
			return true;
		if(p & KEY_B || pmShouldReset())
			return false;
	}
}

// Loads (or builds and caches) the 64x48 thumbnail for a photo.
static void getThumb(int num, bool isVideo, u16 *out) {
	char path[40];
	if(isVideo)
		vidThumbPath(path, num);
	else
		thumbPath(path, num);
	FILE *f = fopen(path, "rb");
	if(f) {
		fread(out, 1, THUMB_W * THUMB_H * 2, f);
		fclose(f);
		return;
	}

	// No cache (photo from an older build): build it from the raw.
	memset(out, 0, THUMB_W * THUMB_H * 2);
	if(isVideo)
		return; // videos always have a cache (written at record stop)
	rawPath(path, num);
	f = fopen(path, "rb");
	if(!f)
		return;
	u16 *yuv = (u16 *)malloc(RAW_SIZE);
	fread(yuv, 1, RAW_SIZE, f);
	fclose(f);
	blitYuvScaled(yuv, out, THUMB_W, THUMB_W, THUMB_H);
	free(yuv);

	thumbPath(path, num);
	f = fopen(path, "wb");
	if(f) {
		fwrite(out, 1, THUMB_W * THUMB_H * 2, f);
		fclose(f);
	}
}

// --- Album gallery: one continuous scrolling layout, photos in rows of 4
// --- grouped under per-day headers, with eased scrolling.

#define HEADER_H 23
#define ROW_H (THUMB_H + 2)
#define SECTION_GAP 7
#define SCREEN_H 192

// 5x7 pixel font (columns, LSB = top row) for the date headers; covers
// uppercase, digits, comma and space.
static const u8 s_font[46][5] = {
	{0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
	{0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
	{0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
	{0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
	{0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
	{0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
	{0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
	{0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
	{0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, // A-Z
	{0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46},
	{0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
	{0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
	{0x06, 0x49, 0x49, 0x29, 0x1E},                                 // 0-9
	{0x00, 0x50, 0x30, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00}, // comma, space
	{0x02, 0x01, 0x51, 0x09, 0x06}, {0x00, 0x60, 0x60, 0x00, 0x00}, // ? .
	{0x20, 0x10, 0x08, 0x04, 0x02}, {0x00, 0x36, 0x36, 0x00, 0x00}, // / :
	{0x08, 0x08, 0x08, 0x08, 0x08}, {0x22, 0x14, 0x7F, 0x14, 0x22}, // - *
	{0x08, 0x1C, 0x2A, 0x08, 0x08}, {0x08, 0x08, 0x2A, 0x1C, 0x08}  // left/right arrow
};

static const u8 *glyph(char c) {
	if(c >= 'a' && c <= 'z')
		c -= 'a' - 'A';
	switch(c) {
		case '+': {
			static const u8 plus[5] = {0x08, 0x08, 0x3E, 0x08, 0x08};
			return plus;
		}
		case '?':
			return s_font[38];
		case '.':
			return s_font[39];
		case '/':
			return s_font[40];
		case ':':
			return s_font[41];
		case '-':
			return s_font[42];
		case '*':
			return s_font[43];
		case '<':
			return s_font[44];
		case '>':
			return s_font[45];
	}
	if(c >= 'A' && c <= 'Z')
		return s_font[c - 'A'];
	if(c >= '0' && c <= '9')
		return s_font[26 + c - '0'];
	if(c == ',')
		return s_font[36];
	return s_font[37];
}

static void drawText(u16 *gfx, int x, int y, const char *s, u16 col) {
	for(; *s && x < 250; s++, x += 6) {
		const u8 *g = glyph(*s);
		for(int cx = 0; cx < 5; cx++) {
			for(int cy = 0; cy < 7; cy++) {
				int yy = y + cy;
				if((g[cx] >> cy & 1) && yy >= 0 && yy < SCREEN_H)
					gfx[yy * 256 + x + cx] = col;
			}
		}
	}
}

// --- Timestamp watermark: when enabled, the capture time is burned into the
// --- saved pixels (photos and video frames) with the same 5x7 font, so it
// --- survives export and conversion. Never drawn into the live preview.

#define WM_SCALE 3 // font blown up to 15x21 for the 640x480 raws

// "YYYY-MM-DD HH:MM:SS", or empty if the clock can't be read.
static void wmFormat(char *out, size_t sz, time_t t) {
	struct tm *lt = localtime(&t);
	if(!lt || strftime(out, sz, "%Y-%m-%d %H:%M:%S", lt) == 0)
		out[0] = '\0';
}

static void yuvSetPixel(u16 *yuv, int x, int y, u8 luma) {
	if(x < 0 || x >= 640 || y < 0 || y >= 480)
		return;
	u8 *px = (u8 *)(yuv + y * 640 + x);
	px[0]  = luma;
	px[1]  = 0x80; // neutral chroma so the text stays grey
}

static void yuvDrawText(u16 *yuv, int x, int y, const char *s, u8 luma) {
	for(; *s; s++, x += 6 * WM_SCALE) {
		const u8 *g = glyph(*s);
		for(int cx = 0; cx < 5; cx++) {
			for(int cy = 0; cy < 7; cy++) {
				if(!(g[cx] >> cy & 1))
					continue;
				for(int sy = 0; sy < WM_SCALE; sy++)
					for(int sx = 0; sx < WM_SCALE; sx++)
						yuvSetPixel(yuv, x + cx * WM_SCALE + sx, y + cy * WM_SCALE + sy, luma);
			}
		}
	}
}

// Bottom-right of a 640x480 YUV422 frame, outlined in black (one stroke width
// in each direction) so it stays readable over a bright scene.
static void watermarkYuv(u16 *yuv, time_t t) {
	char s[24];
	wmFormat(s, sizeof(s), t);
	if(!s[0])
		return;
	int w = (int)strlen(s) * 6 * WM_SCALE - WM_SCALE; // minus the trailing gap
	int x = 640 - w - 14, y = 480 - 7 * WM_SCALE - 14;
	for(int dy = -1; dy <= 1; dy++)
		for(int dx = -1; dx <= 1; dx++)
			if(dx || dy)
				yuvDrawText(yuv, x + dx * WM_SCALE, y + dy * WM_SCALE, s, 0x10);
	yuvDrawText(yuv, x, y, s, 0xEB);
}

// The same stamp at 1x for a 256x192 RGB555 video frame.
static void watermarkFrame(u16 *gfx, time_t t) {
	char s[24];
	wmFormat(s, sizeof(s), t);
	if(!s[0])
		return;
	int x = 256 - (int)strlen(s) * 6 - 3, y = SCREEN_H - 11;
	drawText(gfx, x + 1, y + 1, s, BIT(15)); // drop shadow
	drawText(gfx, x, y, s, BIT(15) | RGB15(31, 31, 31));
}

// --- Bottom-screen UI, rendered directly into the RGB555 bitmap.

#define UI_BG BIT(15)
#define UI_FG (BIT(15) | RGB15(28, 28, 28))
#define UI_DIM (BIT(15) | RGB15(17, 17, 17))
#define UI_DISABLED (BIT(15) | RGB15(10, 10, 10))
#define UI_ALERT (BIT(15) | RGB15(28, 8, 8))

static void uiRect(int x, int y, int w, int h, u16 col) {
	for(int yy = clamp(y, 0, SCREEN_H); yy < clamp(y + h, 0, SCREEN_H); yy++)
		for(int xx = clamp(x, 0, 256); xx < clamp(x + w, 0, 256); xx++)
			s_subGfx[yy * 256 + xx] = col;
}

static int uiTextWidth(const char *s, const UiFont *font) {
	int width = 0;
	for(; *s; s++) {
		unsigned c = (unsigned char)*s;
		width += font->glyphs[(c >= 32 && c <= 126 ? c : '?') - 32].advance;
	}
	return width;
}

static void uiDrawText(u16 *gfx, int x, int y, const char *s, u16 col, const UiFont *font) {
	for(; *s && x < 256; s++) {
		unsigned c       = (unsigned char)*s;
		const UiGlyph *g = &font->glyphs[(c >= 32 && c <= 126 ? c : '?') - 32];
		int stride       = (g->width + 7) / 8;
		for(int cy = 0; cy < font->height; cy++) {
			int yy = y + cy;
			if(yy < 0 || yy >= SCREEN_H)
				continue;
			for(int cx = 0; cx < g->width; cx++) {
				int xx = x + cx;
				if(xx < 0 || xx >= 256)
					continue;
				if(font->pixels[g->offset + cy * stride + cx / 8] & BIT(cx % 8))
					gfx[yy * 256 + xx] = col;
			}
		}
		x += g->advance;
	}
}

static void uiText(int x, int y, const char *s, u16 col) { uiDrawText(s_subGfx, x, y, s, col, &ui_font_11); }

static void uiTextCenter(int y, const char *s, u16 col) { uiText((256 - uiTextWidth(s, &ui_font_11)) / 2, y, s, col); }

static void uiSmall(int x, int y, const char *s, u16 col) { uiDrawText(s_subGfx, x, y, s, col, &ui_font_10); }

static void uiHeader(const char *title) {
	uiRect(0, 0, 256, SCREEN_H, UI_BG);
	uiText(16, 14, title, UI_FG);
}

// The worker only draws inside these two status rows.
static void uiStatus(const char *s) {
	uiRect(0, 164, 256, 28, UI_BG);
	const UiFont *font = &ui_font_10;
	char line[96];
	snprintf(line, sizeof(line), "%s", s);
	size_t split = strlen(line);
	while(split && uiTextWidth(line, font) > 240)
		line[--split] = '\0';
	if(s[split]) {
		char *space = strrchr(line, ' ');
		if(space && space != line && uiTextWidth(s + (space - line) + 1, font) <= 240) {
			split       = space - line;
			line[split] = '\0';
		}
		uiSmall((256 - uiTextWidth(line, font)) / 2, 164, line, UI_DIM);
		const char *rest = s + split;
		if(*rest == ' ')
			rest++;
		snprintf(line, sizeof(line), "%s", rest);
		while(strlen(line) > 3 && uiTextWidth(line, font) > 240)
			line[strlen(line) - 1] = '\0';
		if(uiTextWidth(rest, font) > 240 && strlen(line) > 3)
			strcpy(line + strlen(line) - 3, "...");
	}
	uiSmall((256 - uiTextWidth(line, font)) / 2, 179, line, UI_DIM);
}

static void uiKey(int y, const char *key, const char *action) {
	uiSmall(16, y, key, UI_DIM);
	uiText(62, y - 1, action, UI_FG);
}

static void uiCameraScreen(int cam, bool fatInited) {
	uiHeader("");
	uiText(16, 14, "photo", s_videoMode ? UI_DIM : UI_FG);
	uiText(65, 14, "video", s_videoMode ? UI_FG : UI_DIM);
	uiSmall(186, 15, cam == CAM_INNER ? "inner / A" : "outer / A", UI_DIM);
	if(!fatInited) {
		uiTextCenter(98, "insert SD card to save", UI_DIM);
		return;
	}
	uiTextCenter(98, s_videoMode ? "L/R record" : s_nightBurst ? "L/R night photo" : "L/R capture", UI_FG);
	uiSmall(16, 146, "select album", UI_DIM);
	uiSmall(180, 146, "X controls", UI_DIM);
}

static void uiRecordingScreen(void) {
	uiHeader("");
	uiText(16, 14, "recording", UI_ALERT);
	uiTextCenter(98, "L/R stop", UI_FG);
}

enum { ADV_SHUTTER, ADV_GAIN, ADV_WHITE_BALANCE, ADV_NIGHT, ADV_REAR_LIGHT, ADV_TIMESTAMP, ADV_RESET, ADV_COUNT };
#define ADV_ROW_Y 40
#define ADV_ROW_H 17

static bool advancedRowEnabled(int row) {
	if(row == ADV_SHUTTER || row == ADV_WHITE_BALANCE || row == ADV_NIGHT)
		return !s_videoMode;
	if(row == ADV_GAIN)
		return !s_videoMode && s_shutter;
	if(row == ADV_REAR_LIGHT)
		return s_camera == CAM_OUTER;
	return true;
}

static int advancedMoveSelection(int selected, int direction) {
	do {
		selected = (selected + direction + ADV_COUNT) % ADV_COUNT;
	} while(!advancedRowEnabled(selected));
	return selected;
}

static void uiAdvancedScreen(int selected) {
	uiHeader("controls");
	uiSmall(204, 15, "B back", UI_DIM);
	const char *labels[] = {"shutter", "gain", "white balance", "night burst", "rear light", "timestamp", "reset"};
	char shutter[16], gain[16], warmth[16];
	if(!s_warmth || s_videoMode)
		strcpy(warmth, "auto");
	else
		sprintf(warmth, "%s %d", s_warmth > 0 ? "warm" : "cool", abs(s_warmth));
	if(!s_videoMode && s_shutter)
		sprintf(shutter, "%u line%s", s_shutterLines[s_shutter], s_shutterLines[s_shutter] == 1 ? "" : "s");
	else
		strcpy(shutter, "auto");
	if(!s_videoMode && s_shutter)
		sprintf(gain, "%ux", 1u << s_gain);
	else
		strcpy(gain, "auto");
	const char *values[] = {shutter,
							gain,
							warmth,
							!s_videoMode && s_nightBurst ? "on" : "off",
							s_camera == CAM_OUTER && s_rearLight ? "on" : "off",
							s_timestamp ? "on" : "off",
							"A"};
	for(int i = 0; i < ADV_COUNT; i++) {
		int y        = ADV_ROW_Y + i * ADV_ROW_H;
		bool enabled = advancedRowEnabled(i);
		bool active  = enabled && i == selected;
		if(active)
			uiText(6, y + 2, ">", UI_FG);
		uiText(18, y + 2, labels[i], enabled ? UI_FG : UI_DISABLED);
		uiText(238 - uiTextWidth(values[i], &ui_font_11),
			   y + 2,
			   values[i],
			   !enabled     ? UI_DISABLED
				   : active ? UI_FG
							: UI_DIM);
	}
	const char *hints[] = {"A for Auto. Manual unlocks gain.",
						   "Higher gain adds noise.",
						   "A for Auto. Left cools, right warms.",
						   "Four frames. Keep the camera still.",
						   "Off during Night Burst captures.",
						   "Add date and time to saved images.",
						   "A to restore default settings."};
	uiStatus(s_videoMode && !advancedRowEnabled(selected) ? "Photo controls unavailable in video." : hints[selected]);
}

static void uiExposureFailure(unsigned lines, unsigned gain) {
	unsigned stage = cameraExposureError(EXPOSURE_ERROR_STAGE);
	unsigned reg   = cameraExposureError(EXPOSURE_ERROR_REGISTER);
	char message[40];
	if(stage)
		snprintf(message, sizeof(message), "SENSOR E%u R%04X - TRY AUTO", stage, reg);
	else
		strcpy(message, "PREVIEW FAILED - TRY AGAIN");
	uiStatus(message);
	FILE *f = fopen(RAW_DIR "/exposure-errors.log", "a");
	if(!f)
		return;
	fprintf(f,
			"time=%ld camera=%d lines=%u gain=%u stage=%u reg=%04X expected=%04X observed=%04X\n",
			(long)time(NULL),
			s_camera,
			lines,
			gain,
			stage,
			reg,
			cameraExposureError(EXPOSURE_ERROR_EXPECTED),
			cameraExposureError(EXPOSURE_ERROR_OBSERVED));
	fclose(f);
}

static bool advancedMenu(u16 *gfx) {
	bool exitRequested = false;
	int selected       = s_videoMode ? ADV_TIMESTAMP : ADV_SHUTTER;
	bool dirty         = false;
	keysSetRepeat(14, 4);
	uiAdvancedScreen(selected);
	while(!pmShouldReset()) {
		swiWaitForVBlank();
		scanKeys();
		if(keysHeld() & KEY_LID) {
			lidSleep();
			continue;
		}
		u16 pressed = keysDownRepeat();
		u16 *frame  = pressed ? NULL : cameraNextFrame();
		if(frame) {
			if(!s_videoMode && s_warmth)
				whiteBalancePreview(frame, gfx, VID_W * VID_H, s_warmth);
			else
				memcpy(gfx, frame, VID_FRAME_SIZE);
		}
		bool tappedAction = false;
		if(keysDown() & KEY_TOUCH) {
			touchPosition touch;
			touchRead(&touch);
			if(touch.px >= 170 && touch.py < 35)
				break;
			if(touch.py >= ADV_ROW_Y && touch.py < ADV_ROW_Y + ADV_COUNT * ADV_ROW_H) {
				int row = (touch.py - ADV_ROW_Y) / ADV_ROW_H;
				if(!advancedRowEnabled(row))
					continue;
				selected = row;
				if(touch.px >= 128) {
					if(selected >= ADV_NIGHT)
						tappedAction = true;
					else
						pressed |= touch.px < 180 ? KEY_LEFT : KEY_RIGHT;
				}
			}
		}
		if(keysDown() & (KEY_B | KEY_X | KEY_START)) {
			exitRequested = (keysDown() & KEY_START) != 0;
			break;
		}
		if(!s_videoMode && (keysDown() & (KEY_L | KEY_R))) {
			takePhotos(gfx);
			continue;
		}
		if(!pressed)
			continue;
		unsigned oldShutter = s_shutter, oldGain = s_gain;
		int oldWarmth = s_warmth;
		bool oldNight = s_nightBurst, oldTimestamp = s_timestamp, oldRearLight = s_rearLight;
		if(pressed & KEY_UP)
			selected = advancedMoveSelection(selected, -1);
		else if(pressed & KEY_DOWN)
			selected = advancedMoveSelection(selected, 1);
		int direction = (pressed & KEY_LEFT) ? -1 : (pressed & KEY_RIGHT) ? 1 : 0;
		bool action   = tappedAction || (keysDown() & KEY_A);
		if(selected == ADV_SHUTTER) {
			if(direction)
				s_shutter = clamp((int)s_shutter + direction, 0, SHUTTER_COUNT - 1);
			if(action)
				s_shutter = 0;
		} else if(selected == ADV_GAIN) {
			if(action)
				s_gain = (s_gain + 1) % 4;
			else if(direction)
				s_gain = clamp((int)s_gain + direction, 0, 3);
		} else if(selected == ADV_WHITE_BALANCE) {
			if(action)
				s_warmth = 0;
			else if(direction)
				s_warmth = clamp(s_warmth + direction, -3, 3);
		} else if(selected == ADV_NIGHT && (action || direction)) {
			s_nightBurst = direction ? direction > 0 : !s_nightBurst;
		} else if(selected == ADV_TIMESTAMP && (action || direction)) {
			s_timestamp = direction ? direction > 0 : !s_timestamp;
		} else if(selected == ADV_REAR_LIGHT && (action || direction)) {
			s_rearLight = direction ? direction > 0 : !s_rearLight;
		} else if(selected == ADV_RESET && action) {
			s_shutter = s_gain = 0;
			s_warmth           = 0;
			s_nightBurst       = false;
			s_timestamp        = false;
			s_rearLight        = true;
		}
		bool changed            = !s_videoMode && (oldShutter != s_shutter || oldGain != s_gain);
		unsigned attemptedLines = s_shutterLines[s_shutter], attemptedGain = s_gain;
		uiAdvancedScreen(selected);
		if(changed)
			uiStatus("Applying settings...");
		bool ok = !changed || cameraSetExposure(attemptedLines, attemptedGain);
		if(!ok) {
			s_shutter    = oldShutter;
			s_gain       = oldGain;
			s_warmth     = oldWarmth;
			s_nightBurst = oldNight;
			s_timestamp  = oldTimestamp;
			s_rearLight  = oldRearLight;
		}
		bool lightOk = oldRearLight == s_rearLight || cameraSetRearLight(s_rearLight);
		if(!lightOk) {
			s_rearLight = oldRearLight;
			cameraSetRearLight(s_rearLight);
		}
		dirty |= oldShutter != s_shutter || oldGain != s_gain || oldWarmth != s_warmth || oldNight != s_nightBurst ||
			oldTimestamp != s_timestamp || oldRearLight != s_rearLight;
		if(changed || !lightOk)
			uiAdvancedScreen(selected);
		if(!ok)
			uiExposureFailure(attemptedLines, attemptedGain);
		else if(!lightOk)
			uiStatus("REAR LIGHT SETTING FAILED");
	}
	if(dirty)
		cfgSave();
	return exitRequested;
}

// Centered modal question; waits for A (true) / B (false).
static bool uiConfirm(const char *msg) {
	uiHeader("delete");
	uiTextCenter(69, msg, UI_FG);
	uiSmall(62, 88, "moves to SD trash", UI_DIM);
	uiSmall(48, 112, "B cancel", UI_DIM);
	uiSmall(156, 112, "A delete", UI_ALERT);
	return confirmAB();
}

typedef struct Section {
	int start; // first photo index
	int y;     // header's virtual y
	char label[24];
} Section;

// Lays out photos into rows of 4 under per-day headers. Fills px/py (virtual
// position per photo) and *outSec/*outNsec; returns the total virtual height.
static int buildLayout(const Photo *ph, int count, int *px, int *py, Section **outSec, int *outNsec) {
	static const char *mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	Section *sec               = (Section *)malloc(count * sizeof(Section));
	int ns = 0, y = 0, i = 0;

	while(i < count) {
		sec[ns].start = i;
		sec[ns].y     = y;
		struct tm *lt = localtime(&ph[i].ts);
		if(lt)
			sprintf(sec[ns].label, "%s %d, %d", mon[lt->tm_mon], lt->tm_mday, 1900 + lt->tm_year);
		else
			strcpy(sec[ns].label, "Unknown date");
		y += HEADER_H;

		int dk = dayKey(ph[i].ts), col = 0;
		while(i < count && dayKey(ph[i].ts) == dk) {
			px[i] = col * THUMB_W;
			py[i] = y;
			if(++col == GRID_COLS) {
				col = 0;
				y += ROW_H;
			}
			i++;
		}
		if(col)
			y += ROW_H;
		y += SECTION_GAP;
		ns++;
	}

	*outSec  = sec;
	*outNsec = ns;
	return y;
}

static void hline(u16 *gfx, int x, int y, int w, u16 col) {
	if(y < 0 || y >= SCREEN_H)
		return;
	for(int i = 0; i < w; i++)
		gfx[y * 256 + x + i] = col;
}

static void composeAlbum(u16 *gfx,
						 int count,
						 const Photo *ph,
						 const u16 *thumbs,
						 const Section *sec,
						 int nsec,
						 const int *px,
						 const int *py,
						 int scroll,
						 int sel,
						 const bool *marked) {
	u16 bgCol = UI_BG;
	for(int i = 0; i < 256 * SCREEN_H; i++)
		gfx[i] = bgCol;

	for(int s = 0; s < nsec; s++) {
		int dy = sec[s].y - scroll;
		if(dy > -HEADER_H && dy < SCREEN_H)
			uiDrawText(gfx, 6, dy + 3, sec[s].label, UI_FG, &ui_font_11);
	}

	for(int i = 0; i < count; i++) {
		int dy = py[i] - scroll;
		if(dy <= -THUMB_H || dy >= SCREEN_H)
			continue;
		for(int y = 0; y < THUMB_H; y++) {
			int yy = dy + y;
			if(yy >= 0 && yy < SCREEN_H)
				memcpy(gfx + yy * 256 + px[i], thumbs + i * THUMB_W * THUMB_H + y * THUMB_W, THUMB_W * 2);
		}
		// Videos get their duration bottom-right of the cell.
		if(ph[i].isVideo) {
			char m[8];
			sprintf(m, "%d:%02d", ph[i].durSec / 60, ph[i].durSec % 60);
			int tw = (int)strlen(m) * 6;
			int tx = px[i] + THUMB_W - tw - 2;
			for(int y = 0; y < 9; y++) {
				int yy = dy + THUMB_H - 10 + y;
				if(yy >= 0 && yy < SCREEN_H)
					hline(gfx, tx - 2, yy, tw + 3, BIT(15) | RGB15(2, 2, 3));
			}
			drawText(gfx, tx, dy + THUMB_H - 9, m, BIT(15) | RGB15(28, 28, 30));
		}
	}

	// Marked items are bright; other focus borders are muted.
	u16 markedColor = UI_FG;
	for(int i = 0; i < count; i++) {
		if(!marked || !marked[i] || i == sel)
			continue;
		int my = py[i] - scroll;
		if(my <= -THUMB_H || my >= SCREEN_H)
			continue;
		hline(gfx, px[i], my, THUMB_W, markedColor);
		hline(gfx, px[i], my + 1, THUMB_W, markedColor);
		hline(gfx, px[i], my + THUMB_H - 2, THUMB_W, markedColor);
		hline(gfx, px[i], my + THUMB_H - 1, THUMB_W, markedColor);
		for(int y = 0; y < THUMB_H; y++) {
			int yy = my + y;
			if(yy < 0 || yy >= SCREEN_H)
				continue;
			gfx[yy * 256 + px[i]]               = markedColor;
			gfx[yy * 256 + px[i] + 1]           = markedColor;
			gfx[yy * 256 + px[i] + THUMB_W - 2] = markedColor;
			gfx[yy * 256 + px[i] + THUMB_W - 1] = markedColor;
		}
	}

	// Keep the focused item distinct from marked items.
	u16 focus = marked && marked[sel] ? markedColor : UI_DIM;
	int dy    = py[sel] - scroll;
	hline(gfx, px[sel], dy, THUMB_W, focus);
	hline(gfx, px[sel], dy + 1, THUMB_W, focus);
	hline(gfx, px[sel], dy + THUMB_H - 2, THUMB_W, focus);
	hline(gfx, px[sel], dy + THUMB_H - 1, THUMB_W, focus);
	for(int y = 0; y < THUMB_H; y++) {
		int yy = dy + y;
		if(yy < 0 || yy >= SCREEN_H)
			continue;
		gfx[yy * 256 + px[sel]]               = focus;
		gfx[yy * 256 + px[sel] + 1]           = focus;
		gfx[yy * 256 + px[sel] + THUMB_W - 2] = focus;
		gfx[yy * 256 + px[sel] + THUMB_W - 1] = focus;
	}
}

// Moves the selection one visual row up (dir<0) or down (dir>0), keeping the
// column when possible. Rows are contiguous index ranges sharing one py.
static int moveRow(const int *px, const int *py, int count, int sel, int dir) {
	int cy = py[sel], col = px[sel] / THUMB_W;
	if(dir < 0) {
		int i = sel;
		while(i > 0 && py[i] == cy)
			i--;
		if(py[i] == cy)
			return sel;
		int ry = py[i], start = i;
		while(start > 0 && py[start - 1] == ry)
			start--;
		int len = i - start;
		return start + (col < len ? col : len);
	}
	int i = sel;
	while(i < count - 1 && py[i] == cy)
		i++;
	if(py[i] == cy)
		return sel;
	int ry = py[i], end = i;
	while(end < count - 1 && py[end + 1] == ry)
		end++;
	int len = end - i;
	return i + (col < len ? col : len);
}

// Full-screen view of one photo. Returns true if a photo was deleted (the
// caller must reload its list); *idx may change from browsing.
static bool fullView(u16 *gfx, const Photo *ph, int count, int *idx) {
	u16 *yuv     = (u16 *)malloc(RAW_SIZE);
	bool dirty   = true;
	bool deleted = false;

	while(!pmShouldReset()) {
		if(dirty) {
			char path[40];
			if(ph[*idx].isVideo) {
				// Show the video's first frame.
				vidPath(path, ph[*idx].num);
				FILE *f = fopen(path, "rb");
				if(f && vidReadFirstFrame(f, gfx)) {
					fclose(f);
				} else {
					if(f)
						fclose(f);
					memset(gfx, 0, 256 * 192 * 2);
				}
			} else {
				rawPath(path, ph[*idx].num);
				FILE *f = fopen(path, "rb");
				if(f) {
					fread(yuv, 1, RAW_SIZE, f);
					fclose(f);
					blitYuv(yuv, gfx);
				}
			}
			char when[40], line[40];
			struct tm *lt = localtime(&ph[*idx].ts);
			if(lt)
				strftime(when, sizeof(when), "%b %d, %Y  %H:%M", lt);
			else
				strcpy(when, "Unknown date");
			sprintf(line, "%s_%04d", ph[*idx].isVideo ? "VID" : "IMG", ph[*idx].num);
			uiHeader(line);
			uiTextCenter(44, when, UI_DIM);
			sprintf(line, "%d/%d", *idx + 1, count);
			uiTextCenter(58, line, UI_DIM);
			int y = 88;
			if(ph[*idx].isVideo)
				uiKey(y, "A", "play video");
			else
				uiKey(y, "A", "send to DSi Album");
			uiKey(y += 20, "X", "delete");
			uiSmall(16, 146, "left/right browse  B back", UI_DIM);
			dirty = false;
		}

		swiWaitForVBlank();
		scanKeys();
		if(keysHeld() & KEY_LID) {
			lidSleep();
			continue;
		}
		u16 pressed = keysDown();

		if(pressed & KEY_LEFT) {
			*idx  = (*idx + count - 1) % count;
			dirty = true;
		} else if(pressed & KEY_RIGHT) {
			*idx  = (*idx + 1) % count;
			dirty = true;
		} else if(pressed & KEY_A) {
			if(ph[*idx].isVideo) {
				playVideo(gfx, ph[*idx].num);
				dirty = true; // redraw the still + info afterwards
			} else {
				uiStatus("Sending to DSi Album...");
				uiStatus(exportToAlbum(ph[*idx].num) ? "Sent to DSi Album" : "Could not send photo. Try again.");
			}
		} else if(pressed & KEY_X) {
			char msg[32];
			sprintf(msg, "Delete %s_%04d?", ph[*idx].isVideo ? "VID" : "IMG", ph[*idx].num);
			if(uiConfirm(msg)) {
				softDelete(ph[*idx].num, ph[*idx].isVideo);
				deleted = true;
				break;
			}
			dirty = true;
		} else if(pressed & (KEY_B | KEY_SELECT | KEY_START)) {
			break;
		}
	}

	free(yuv);
	return deleted;
}

// The album: a scrolling gallery. D-pad: move, L/R: day, A: full view,
// Y: mark, X: delete (marked photos, or the selected one), B/SELECT: back.
static void viewer(u16 *gfx, int cam) {
	keysSetRepeat(14, 4); // hold d-pad to keep moving
	bool quit  = false;
	bool empty = false;
	int sel    = -1; // -1 = newest; preserved across reloads after a delete

	while(!quit) {
		Photo *ph;
		int count = scanRaw(&ph);
		if(count == 0) {
			empty = true;
			free(ph);
			break;
		}
		if(sel < 0 || sel >= count)
			sel = count - 1;

		int *px      = (int *)malloc(count * sizeof(int));
		int *py      = (int *)malloc(count * sizeof(int));
		bool *marked = (bool *)calloc(count, sizeof(bool));
		Section *sec;
		int nsec;
		int virtH     = buildLayout(ph, count, px, py, &sec, &nsec);
		int maxScroll = virtH - SCREEN_H;
		if(maxScroll < 0)
			maxScroll = 0;

		// Cache every thumbnail in RAM (~6KB each) so scrolling never hits the SD.
		u16 *thumbs = (u16 *)malloc(count * THUMB_W * THUMB_H * sizeof(u16));
		uiHeader("album");
		uiStatus("Loading...");
		for(int i = 0; i < count; i++)
			getThumb(ph[i].num, ph[i].isVideo, thumbs + i * THUMB_W * THUMB_H);

		int scroll = maxScroll, target = maxScroll;
		bool dirty = true, infoDirty = true, reload = false;

		while(!reload && !pmShouldReset()) {
			swiWaitForVBlank();
			scanKeys();
			if(keysHeld() & KEY_LID) {
				lidSleep();
				continue;
			}
			u16 pressed = keysDownRepeat();

			int prevSel = sel;
			if(pressed & KEY_LEFT)
				sel--;
			else if(pressed & KEY_RIGHT)
				sel++;
			else if(pressed & KEY_UP)
				sel = moveRow(px, py, count, sel, -1);
			else if(pressed & KEY_DOWN)
				sel = moveRow(px, py, count, sel, 1);
			else if(pressed & (KEY_L | KEY_R)) {
				// Jump to the previous/next day.
				int s = 0;
				while(s + 1 < nsec && sec[s + 1].start <= sel)
					s++;
				s += (pressed & KEY_L) ? -1 : 1;
				if(s < 0)
					s = 0;
				if(s >= nsec)
					s = nsec - 1;
				sel = sec[s].start;
			}
			if(sel < 0)
				sel = 0;
			if(sel >= count)
				sel = count - 1;
			if(sel != prevSel)
				infoDirty = true;

			u16 tapped = keysDown();
			if(tapped & KEY_A) {
				reload    = fullView(gfx, ph, count, &sel);
				dirty     = true;
				infoDirty = true;
			} else if(tapped & KEY_Y) {
				marked[sel] = !marked[sel];
				dirty       = true;
				infoDirty   = true;
			} else if(tapped & KEY_X) {
				int nMarked = 0;
				for(int i = 0; i < count; i++)
					nMarked += marked[i];
				char msg[36];
				if(nMarked)
					sprintf(msg, "Delete %d selected item%s?", nMarked, nMarked == 1 ? "" : "s");
				else
					sprintf(msg, "Delete %s_%04d?", ph[sel].isVideo ? "VID" : "IMG", ph[sel].num);
				if(uiConfirm(msg)) {
					if(nMarked) {
						for(int i = 0; i < count; i++)
							if(marked[i])
								softDelete(ph[i].num, ph[i].isVideo);
					} else {
						softDelete(ph[sel].num, ph[sel].isVideo);
					}
					reload = true;
				}
				dirty     = true;
				infoDirty = true;
			} else if(tapped & (KEY_B | KEY_SELECT)) {
				quit = true;
				break;
			}

			// Keep the selection (and its day header) on screen; ease toward it.
			if(py[sel] - HEADER_H < target)
				target = py[sel] - HEADER_H;
			if(py[sel] + THUMB_H + 4 - SCREEN_H > target)
				target = py[sel] + THUMB_H + 4 - SCREEN_H;
			if(target < 0)
				target = 0;
			if(target > maxScroll)
				target = maxScroll;

			if(scroll != target) {
				int d = target - scroll;
				scroll += d / 3 + (d > 0 ? 1 : -1);
				dirty = true;
			}

			if(dirty || sel != prevSel) {
				composeAlbum(gfx, count, ph, thumbs, sec, nsec, px, py, scroll, sel, marked);
				dirty = scroll != target;
			}

			if(infoDirty) {
				int nMarked = 0;
				for(int i = 0; i < count; i++)
					nMarked += marked[i];
				char line[36];
				uiHeader("album");
				sprintf(line,
						"%s_%04d  %d/%d%s",
						ph[sel].isVideo ? "VID" : "IMG",
						ph[sel].num,
						sel + 1,
						count,
						marked[sel] ? "  *" : "");
				uiTextCenter(44, line, UI_FG);
				if(nMarked) {
					sprintf(line, "%d selected", nMarked);
					uiTextCenter(58, line, UI_DIM);
				}
				uiKey(88, "A", "open");
				uiKey(108, "Y", "select");
				uiKey(128, "X", "delete");
				uiSmall(16, 146, "d-pad browse  B camera", UI_DIM);
				infoDirty = false;
			}
		}

		if(pmShouldReset())
			quit = true;
		free(thumbs);
		free(marked);
		free(sec);
		free(py);
		free(px);
		free(ph);
	} // reload loop

	uiCameraScreen(cam, true);
	if(empty)
		uiStatus("No photos yet. Press L/R to take one.");
}

int main(int argc, char **argv) {
	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankC(VRAM_C_SUB_BG);
	videoSetMode(MODE_5_2D);
	videoSetModeSub(MODE_5_2D);
	int bg3Main = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 1, 0);
	int bg3Sub  = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 1, 0);
	u16 *gfx    = bgGetGfxPtr(bg3Main);
	s_subGfx    = bgGetGfxPtr(bg3Sub);

	uiHeader("Camera");
	uiStatus("Starting camera...");

	bool fatInited = fatInitDefault();
	if(fatInited) {
		mkdir(RAW_DIR, 0777);
		cfgLoad();
		Photo *ph;
		int count = scanRaw(&ph);
		for(int i = 0; i < count; i++)
			if(ph[i].num >= s_nextRawNum)
				s_nextRawNum = ph[i].num + 1;
		free(ph);
		// Numbers of soft-deleted photos stay reserved so a fresh photo can't
		// collide with one already in the trash.
		DIR *tdir = opendir(RAW_DIR "/trash");
		if(tdir) {
			struct dirent *pent;
			while((pent = readdir(tdir))) {
				if(strncmp(pent->d_name, "IMG_", 4) != 0 && strncmp(pent->d_name, "VID_", 4) != 0)
					continue;
				int val = atoi(pent->d_name + 4);
				if(pent->d_name[0] == 'V') {
					if(val >= s_nextVidNum)
						s_nextVidNum = val + 1;
				} else if(val >= s_nextRawNum) {
					s_nextRawNum = val + 1;
				}
			}
			closedir(tdir);
		}
		// Video numbering continues past the highest VID_ file.
		DIR *vdir = opendir(RAW_DIR);
		if(vdir) {
			struct dirent *pent;
			while((pent = readdir(vdir))) {
				if(strncmp(pent->d_name, "VID_", 4) == 0) {
					int val = atoi(pent->d_name + 4);
					if(val >= s_nextVidNum)
						s_nextVidNum = val + 1;
				}
			}
			closedir(vdir);
		}
	} else {
		uiStatus("Insert an SD card to save");
	}

	uiStatus("Connecting camera...");
	pxiWaitRemote(PXI_CAMERA); // Wait for ARM7 to initialize PXI
	cameraInit();
	cameraSetRearLight(s_rearLight);
	soundInit(); // ARM9 interface to the ARM7 sound driver (video playback)
	soundPowerOn();
	soundSetMixerConfig(SoundOutSrc_Mixer, SoundOutSrc_Mixer, false, false);
	soundSetMixerVolume(127);

	// Fetch the 16-byte camera signing key from the ARM7 (it read it from the
	// BIOS-populated WRAM at boot), as 8 halfwords because PXI immediates are
	// only 26-bit (full words would be truncated).
	u16 *kh = (u16 *)s_camKey;
	for(int i = 0; i < 8; i++)
		kh[i] = pxiSendAndReceive(PXI_CAMERA, CAM_GET_KEY0 + i);

	// Background SD-writer thread, lower priority than the main thread so the
	// preview and capture stay responsive.
	mailboxPrepare(&s_jobMailbox, s_jobSlots, JOB_QUEUE_LEN);
	threadPrepare(&s_workerThread, workerMain, NULL, s_workerStack + sizeof(s_workerStack), MAIN_THREAD_PRIO + 4);
	threadStart(&s_workerThread);

	Camera camera = CAM_OUTER;
	s_camera      = camera;
	cameraActivate(camera);
	unsigned bootLines = s_shutterLines[s_shutter], bootGain = s_gain;
	bool exposureReady = cameraSetExposure(bootLines, bootGain);
	if(!exposureReady) {
		uiExposureFailure(bootLines, bootGain);
		s_shutter = s_gain = 0;
		cameraSetExposure(0, 0);
	}
	uiCameraScreen(camera, fatInited);
	if(!exposureReady)
		uiStatus("SAVED EXPOSURE SETTING FAILED");
	streamStart(); // continuous viewfinder
	scanKeys();    // buttons held over from the launcher aren't new presses

	while(1) {
		u16 pressed = 0;
		do {
			swiWaitForVBlank();
			// A tap of the power button asks the app to exit; leaving returns
			// to the main menu (Unlaunch).
			if(pmShouldReset()) {
				cameraDeactivate(camera);
				drainJobs();
				return 0;
			}
			scanKeys();
			// Lid closed: shut the camera off and sleep until it reopens.
			if(keysHeld() & KEY_LID) {
				lidSleep();
				continue;
			}
			// Blit whatever the camera has produced since the last vblank.
			pressed    = keysDown();
			u16 *frame = pressed ? NULL : cameraNextFrame();
			if(frame) {
				if(!s_videoMode && s_warmth)
					whiteBalancePreview(frame, gfx, VID_W * VID_H, s_warmth);
				else
					memcpy(gfx, frame, VID_FRAME_SIZE);
			}
			if(pressed & KEY_TOUCH) {
				touchPosition touch;
				touchRead(&touch);
				pressed |= uiCameraTap(touch.px, touch.py, fatInited);
			}
		} while(!pressed);

		if(pressed & KEY_A) {
			// Switch camera
			camera   = camera == CAM_INNER ? CAM_OUTER : CAM_INNER;
			s_camera = camera;
			cameraActivate(camera);
			uiCameraScreen(camera, fatInited);
			streamStart();
		} else if(fatInited && pressed & KEY_Y) {
			bool video  = !s_videoMode;
			s_videoMode = video;
			uiCameraScreen(camera, fatInited);
			if(!cameraSetExposure(video ? 0 : s_shutterLines[s_shutter], s_gain)) {
				s_videoMode = !video;
				uiCameraScreen(camera, fatInited);
				uiExposureFailure(video ? 0 : s_shutterLines[s_shutter], s_gain);
				continue;
			}
		} else if(fatInited && pressed & KEY_X) {
			if(advancedMenu(gfx)) {
				cameraDeactivate(camera);
				drainJobs();
				return 0;
			}
			uiCameraScreen(camera, fatInited);
			streamStart();
		} else if(fatInited && pressed & (KEY_L | KEY_R)) {
			if(s_videoMode) {
				// Tap L/R to record, tap again to stop.
				drainJobs(); // finish any pending photo writes first
				s_savedMsg[0] = '\0';
				recordVideo(gfx, s_nextVidNum++);
				uiCameraScreen(camera, fatInited);
				if(s_savedMsg[0])
					uiStatus(s_savedMsg); // survives the redraw above
				streamStart();            // recordVideo left the camera idle
			} else {
				takePhotos(gfx);
			}
		} else if(fatInited && pressed & KEY_SELECT) {
			// Stop the live preview and browse photos.
			streamStop();
			drainJobs(); // so the newest shots are in the list
			viewer(gfx, camera);
			streamStart();
		} else if(pressed & KEY_START) {
			// Disable camera so the light turns off
			cameraDeactivate(camera);
			drainJobs();

			return 0;
		}
	}
}
