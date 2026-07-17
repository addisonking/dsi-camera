#include "camera.h"
#include "version.h"

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
#include <sys/statvfs.h>
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
static void uiStatus(const char *s);
void uiSpace(void);

// Lid closed: shut the camera off and sleep until it reopens. Usable from any
// screen; VRAM survives sleep so nothing needs redrawing after.
static void lidSleep(void) {
	cameraDeactivate(s_camera);
	pmEnterSleep();
	cameraActivate(s_camera);
}

// --- Video recording: 256x192 RGB555 frames from preview mode + mic audio,
// --- written as a custom .VID container by the worker thread.

#define VID_FPS 10 // what a DSi SD card can actually sustain (~1MB/s)
#define VID_W 256
#define VID_H 192
#define VID_FRAME_SIZE (VID_W * VID_H * 2)
#define VID_SLOTS 24 // ~2.4MB ring; a frame is dropped if it fills
#define AUD_CHUNKS 16

#define VID_MAGIC "DSIVID01"
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
			char path[40];
			if(isVideo)
				vidPath(path, ph[count].num);
			else
				rawPath(path, ph[count].num);
			struct stat st;
			ph[count].ts = stat(path, &st) == 0 ? st.st_mtime : 0;
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
static u8 s_vidFree[VID_SLOTS]; // SPSC ring of free slot indices
static vu32 s_vidFreeHead = 0;  // written by worker
static vu32 s_vidFreeTail = 0;  // written by record loop

// Audio chunk pool (producer: record loop via micDrain; consumer: worker).
typedef struct AudioChunk {
	Job job;
	alignas(32) u8 data[MIC_BUF_BYTES];
} AudioChunk;
static AudioChunk s_audPool[AUD_CHUNKS];
static u8 s_audFree[AUD_CHUNKS];
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
			if(f) {
				fwrite(job->buf, 1, RAW_SIZE, f);
				fclose(f);
				sprintf(msg, "SAVED IMG_%04d", job->aux);
			} else {
				sprintf(msg, "SAVE FAILED: IMG_%04d", job->aux);
			}
			uiStatus(msg);

			// Thumbnail cache for the album grid.
			u16 thumb[THUMB_W * THUMB_H];
			blitYuvScaled(job->buf, thumb, THUMB_W, THUMB_W, THUMB_H);
			thumbPath(path, job->aux);
			f = fopen(path, "wb");
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
			if(job->type == CHUNK_VIDEO)
				s_vidFree[s_vidFreeHead++ % VID_SLOTS] = (u8)job->slot;
			else
				s_audFree[s_audFreeHead++ % AUD_CHUNKS] = (u8)job->slot;
		}
		s_jobsDone++;
	}
	return 0;
}

// Blocks until every queued frame has been written out. Call before exiting,
// sleeping, or opening the viewer so the photo list is complete.
static void drainJobs(void) {
	if(JOBS_PENDING)
		uiStatus("SAVING...");
	while(JOBS_PENDING)
		swiWaitForVBlank();
}

// Captures one full-res frame and queues it for background saving. Blocks
// only for the capture DMA itself (or for a queue slot if the SD card is
// >JOB_QUEUE_LEN frames behind). `previewGfx` is the top-screen bitmap; the
// frame is drawn to it so the preview updates during a burst.
static void captureRaw(u16 *previewGfx) {
	// Wait for previous transfer to finish
	while(cameraTransferActive())
		swiWaitForVBlank();

	u16 *yuv = NULL;
	Job *job = NULL;
	for(;;) {
		if(!yuv)
			yuv = (u16 *)malloc(RAW_SIZE);
		if(!job)
			job = (Job *)malloc(sizeof(Job));
		if(yuv && job)
			break;
		// Out of RAM: let the worker finish a frame and retry.
		swiWaitForVBlank();
	}

	cameraTransferStart(yuv, CAPTURE_MODE_CAPTURE);
	while(cameraTransferActive())
		swiWaitForVBlank();
	cameraTransferStop();

	// The sensor's first scanline carries garbage/embedded data (shows up as
	// green speckles); paper over it with the second line.
	memcpy(yuv, yuv + 640, 640 * sizeof(u16));

	if(previewGfx)
		blitYuv(yuv, previewGfx);

	job->type = JOB_PHOTO;
	job->aux  = s_nextRawNum++;
	job->buf  = yuv;
	while(!mailboxTrySend(&s_jobMailbox, (u32)job))
		swiWaitForVBlank(); // queue full, wait for the worker
	s_jobsQueued++;
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
			if(hdr[2] > bufSz) // corrupt chunk of the type we want
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
	if(fread(hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr, VID_MAGIC, 8) != 0)
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
	if(fread(hdr, sizeof(hdr), 1, fv) != 1 || memcmp(hdr, VID_MAGIC, 8) != 0) {
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

	uiStatus("LOADING...");

	// Prebuffer two ring halves of audio (zero-padded = silence).
	memset(s_playRing, 0, sizeof(s_playRing));
	u32 fedChunks  = 0, aIdx;
	bool audioMore = true;
	while(fedChunks < 2 && audioMore) {
		audioMore = vidReadChunk(fa, CHUNK_AUDIO, &aIdx, s_playRing + fedChunks * PLAY_RING_HALF, PLAY_RING_HALF * 2);
		if(audioMore)
			fedChunks++;
	}
	// Debug: hold Y while starting playback to loop the prebuffered first
	// second of file audio without ever feeding more (isolates the feed loop).
	if(keysHeld() & KEY_Y)
		audioMore = false;
	armDCacheFlush(s_playRing, sizeof(s_playRing));

	// Show the first frame immediately; stage the next one.
	u16 *stage = (u16 *)malloc(VID_FRAME_SIZE);
	u32 vIdx = 0, stageIdx = 0;
	bool videoMore   = vidReadChunk(fv, CHUNK_VIDEO, &vIdx, gfx, VID_FRAME_SIZE);
	bool stageFilled = false;
	if(videoMore) {
		u32 next;
		if(vidReadChunk(fv, CHUNK_VIDEO, &next, stage, VID_FRAME_SIZE)) {
			stageIdx    = next;
			stageFilled = true;
		} else {
			videoMore = false;
		}
	}

	if(!videoMore && totalSamples == 0) {
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
					1024,
					64,
					soundTimerFromHz(rate),
					SoundMode_Repeat,
					SoundFmt_Pcm16,
					s_playRing,
					0,
					sizeof(s_playRing) / 4);
	timerStart(0, ClockDivider_1024, 0, NULL); // free-running counter
	timerElapsed(0);                           // zero the baseline

	u32 durSec = totalFrames / fps;
	if(totalSamples / rate > durSec)
		durSec = totalSamples / rate;
	int lastSec = -1;

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

		// Show the staged frame once its pacing slot is due.
		if(stageFilled && (u64)played * fps >= (u64)stageIdx * rate) {
			memcpy(gfx, stage, VID_FRAME_SIZE);
			stageFilled = false;
			u32 next;
			if(vidReadChunk(fv, CHUNK_VIDEO, &next, stage, VID_FRAME_SIZE)) {
				stageIdx    = next;
				stageFilled = true;
			} else {
				videoMore = false;
			}
		}

		bool audioDone = !audioMore && played >= (u64)fedChunks * PLAY_RING_HALF;
		if(!videoMore && !stageFilled && audioDone)
			break;

		int sec = (int)(played / rate);
		if(sec != lastSec) {
			lastSec = sec;
			char msg[32];
			sprintf(msg,
					"PLAYING %d:%02d / %lu:%02lu",
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
	uiStatus("B: BACK");
}

// Records video + mic audio to a .VID container until L/R is tapped again (or
// the lid closes / power is tapped). Frames are paced to VID_FPS on the wall
// clock; if the SD card falls behind, video frames are dropped (never audio).
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
	hdr[5] = 1;
	hdr[6] = MIC_RATE_HZ;
	hdr[7] = 1; // s16le mono
	fwrite(hdr, sizeof(hdr), 1, f);
	s_vidFile = f;
	uiStatus("R1: FILE OPEN");

	for(int i = 0; i < VID_SLOTS; i++)
		s_vidFree[i] = (u8)i;
	for(int i = 0; i < AUD_CHUNKS; i++)
		s_audFree[i] = (u8)i;
	s_vidFreeHead = s_vidFreeTail = 0;
	s_audFreeHead = s_audFreeTail = 0;
	s_audBlockIdx = s_audDropped = 0;
	u32 framesKept = 0, framesDropped = 0, frameIdx = 0;

	// Start the ARM7 mic recorder (it NDMA-drains MICEX into the shared ring).
	uiStatus("R2: AMP ON");
	pmMicSetAmp(true, PmMicGain_80);
	uiStatus("R3: FLUSH");
	armDCacheFlush(s_micRing, sizeof(s_micRing));
	s_micLastDone = 0;
	uiStatus("R4: MIC START PXI");
	u32 micAddr = (u32)s_micRing;
	pxiSendAndReceive(PXI_CAMERA, CAM_MIC_ADDR_LO);
	pxiSendAndReceive(PXI_CAMERA, micAddr & 0xFFFF);
	pxiSendAndReceive(PXI_CAMERA, CAM_MIC_ADDR_HI);
	pxiSendAndReceive(PXI_CAMERA, micAddr >> 16);
	pxiSendAndReceive(PXI_CAMERA, CAM_MIC_START);
	uiStatus("R5: MIC RUNNING");

	// Paced by vblank count (~59.8Hz): keep a frame every keepEvery vblanks.
	u32 vbl       = 0;
	u32 keepEvery = 60 / VID_FPS;
	u32 lastKeep  = 0;
	int lastSec   = -1;
	bool stop     = false;

	while(!stop) {
		swiWaitForVBlank();
		vbl++;
		scanKeys();
		if(keysDown() & (KEY_L | KEY_R) || (keysHeld() & KEY_LID) || pmShouldReset())
			stop = true;

		if(!cameraTransferActive()) {
			if(vbl - lastKeep >= keepEvery) {
				lastKeep = lastKeep ? lastKeep + keepEvery : vbl;
				// frameIdx counts pacing slots (not kept frames), so dropped
				// frames leave index gaps that playback/conversion can
				// duplicate over — A/V sync survives drops.
				if(s_vidFreeTail == s_vidFreeHead + VID_SLOTS) {
					framesDropped++;
					frameIdx++;
				} else {
					int slot = s_vidFree[s_vidFreeTail++ % VID_SLOTS];
					memcpy(s_vidRing[slot], gfx, VID_FRAME_SIZE);
					Job *job  = &s_vidJobs[slot];
					job->type = CHUNK_VIDEO;
					job->aux  = frameIdx++;
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
			cameraTransferStart(gfx, CAPTURE_MODE_PREVIEW); // re-arm viewfinder
		}

		micDrain();

		int sec = (int)(vbl / 60);
		if(sec != lastSec) {
			lastSec = sec;
		}
		// Live telemetry every ~quarter second while debugging the mic path.
		if((vbl & 15) == 0) {
			char msg[40];
			sprintf(msg,
					"REC %d:%02d A%lu V%lu Q%lu D%lu",
					sec / 60,
					sec % 60,
					(unsigned long)s_micLastDone,
					(unsigned long)framesKept,
					(unsigned long)JOBS_PENDING,
					(unsigned long)s_audDropped);
			uiStatus(msg);
		}
	}

	pxiSendAndReceive(PXI_CAMERA, CAM_MIC_STOP);
	pmMicSetAmp(false, 0);
	micDrain(); // pick up the last completed buffers
	while(cameraTransferActive())
		swiWaitForVBlank();
	cameraTransferStop();

	// Let the worker finish the remaining chunks, then patch the totals.
	uiStatus("SAVING...");
	while(JOBS_PENDING)
		swiWaitForVBlank();
	hdr[8] = frameIdx;                            // timeline length in frames (incl. dropped slots)
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

	char msg[36];
	sprintf(msg, "SAVED VID_%04d%s", num, framesDropped ? " (DROPPED FRAMES)" : "");
	uiStatus(msg);
	if(s_audDropped)
		uiStatus("SAVED VID (DROPPED AUDIO)");
	uiSpace();
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

#define HEADER_H 14
#define ROW_H (THUMB_H + 2)
#define SECTION_GAP 4
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

// --- Bottom-screen UI: minimalist monochrome panels drawn with the pixel
// --- font instead of a scrolling console.

#define UI_BG (BIT(15) | RGB15(3, 3, 4))
#define UI_FG (BIT(15) | RGB15(28, 28, 29))
#define UI_DIM (BIT(15) | RGB15(13, 13, 15))

static void uiRect(int x, int y, int w, int h, u16 col) {
	for(int yy = y; yy < y + h; yy++)
		for(int xx = x; xx < x + w; xx++)
			s_subGfx[yy * 256 + xx] = col;
}

static void uiText(int x, int y, const char *s, u16 col) { drawText(s_subGfx, x, y, s, col); }

static void uiTextCenter(int y, const char *s, u16 col) { uiText((256 - (int)strlen(s) * 6) / 2, y, s, col); }

// Double-size text for headers.
static void uiText2(int x, int y, const char *s, u16 col) {
	for(; *s; s++, x += 12) {
		const u8 *g = glyph(*s);
		for(int cx = 0; cx < 5; cx++) {
			for(int cy = 0; cy < 7; cy++) {
				if(!(g[cx] >> cy & 1))
					continue;
				int xx = x + cx * 2, yy = y + cy * 2;
				s_subGfx[yy * 256 + xx]           = col;
				s_subGfx[yy * 256 + xx + 1]       = col;
				s_subGfx[(yy + 1) * 256 + xx]     = col;
				s_subGfx[(yy + 1) * 256 + xx + 1] = col;
			}
		}
	}
}

// Clears the screen and draws the title bar.
static void uiHeader(const char *title) {
	uiRect(0, 0, 256, SCREEN_H, UI_BG);
	uiText2((256 - (int)strlen(title) * 12) / 2, 12, title, UI_FG);
	uiRect(24, 34, 208, 1, UI_DIM);
}

// One-line status at the bottom of the screen (safe to call from the worker
// thread; it only touches its own strip).
static void uiStatus(const char *s) {
	uiRect(0, 178, 256, 14, UI_BG);
	uiTextCenter(182, s, UI_DIM);
}

// A right-aligned key hint column and left-aligned action, one row.
static void uiKey(int y, const char *key, const char *action) {
	uiText(116 - (int)strlen(key) * 6, y, key, UI_DIM);
	uiText(126, y, action, UI_FG);
}

// Free/total SD space, drawn just under the camera name. Safe to call from
// the worker thread; it only touches its own strip.
void uiSpace(void) {
	struct statvfs sv;
	uiRect(0, 58, 256, 10, UI_BG);
	if(statvfs("/", &sv) != 0 || !sv.f_blocks)
		return;
	u64 freeB = (u64)sv.f_bavail * sv.f_frsize;
	u64 totB  = (u64)sv.f_blocks * sv.f_frsize;
	char s[32];
	sprintf(s, "%.1f GB FREE OF %.1f", (double)freeB / 1e9, (double)totB / 1e9);
	uiTextCenter(58, s, UI_DIM);
}

static void uiCameraScreen(int cam, bool fatInited) {
	uiHeader("DSI CAMERA");
	uiTextCenter(44, cam == CAM_INNER ? "INNER CAMERA" : "OUTER CAMERA", UI_DIM);
	uiSpace();

	int y = 82;
	if(fatInited) {
		if(s_videoMode) {
			uiKey(y, "L/R", "RECORD VIDEO");
			uiKey(y += 16, "Y", "PHOTO MODE");
		} else {
			uiKey(y, "L/R", "TAKE PHOTOS");
			uiKey(y += 16, "Y", "VIDEO MODE");
		}
		uiKey(y += 16, "A", "SWAP CAMERA");
		uiKey(y += 16, "SELECT", "ALBUM");
	} else {
		uiTextCenter(y, "NO SD CARD - CANT SAVE", UI_FG);
		uiKey(y += 16, "A", "SWAP CAMERA");
	}
	uiKey(y += 16, "START", "EXIT");
}

// Centered modal question; waits for A (true) / B (false).
static bool uiConfirm(const char *msg) {
	uiRect(27, 63, 202, 58, UI_DIM);
	uiRect(28, 64, 200, 56, UI_BG);
	uiTextCenter(78, msg, UI_FG);
	uiTextCenter(100, "A: YES     B: NO", UI_DIM);
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
	static const char *mon[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
	Section *sec               = (Section *)malloc(count * sizeof(Section));
	int ns = 0, y = 0, i = 0;

	while(i < count) {
		sec[ns].start = i;
		sec[ns].y     = y;
		struct tm *lt = localtime(&ph[i].ts);
		if(lt)
			sprintf(sec[ns].label, "%s %d, %d", mon[lt->tm_mon], lt->tm_mday, 1900 + lt->tm_year);
		else
			strcpy(sec[ns].label, "UNKNOWN DATE");
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
	u16 bgCol = BIT(15) | RGB15(3, 3, 4);
	for(int i = 0; i < 256 * SCREEN_H; i++)
		gfx[i] = bgCol;

	for(int s = 0; s < nsec; s++) {
		int dy = sec[s].y - scroll;
		if(dy > -HEADER_H && dy < SCREEN_H)
			drawText(gfx, 3, dy + 3, sec[s].label, BIT(15) | RGB15(24, 24, 26));
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
		// Play marker on videos: ">" bottom-right of the cell.
		if(ph[i].isVideo) {
			char m[2] = ">";
			drawText(gfx, px[i] + THUMB_W - 8, dy + THUMB_H - 9, m, BIT(15) | RGB15(31, 31, 31));
		}
	}

	// Marked photos get an orange border.
	u16 orange = BIT(15) | RGB15(31, 20, 4);
	for(int i = 0; i < count; i++) {
		if(!marked || !marked[i] || i == sel)
			continue;
		int my = py[i] - scroll;
		if(my <= -THUMB_H || my >= SCREEN_H)
			continue;
		hline(gfx, px[i], my, THUMB_W, orange);
		hline(gfx, px[i], my + 1, THUMB_W, orange);
		hline(gfx, px[i], my + THUMB_H - 2, THUMB_W, orange);
		hline(gfx, px[i], my + THUMB_H - 1, THUMB_W, orange);
		for(int y = 0; y < THUMB_H; y++) {
			int yy = my + y;
			if(yy < 0 || yy >= SCREEN_H)
				continue;
			gfx[yy * 256 + px[i]]               = orange;
			gfx[yy * 256 + px[i] + 1]           = orange;
			gfx[yy * 256 + px[i] + THUMB_W - 2] = orange;
			gfx[yy * 256 + px[i] + THUMB_W - 1] = orange;
		}
	}

	// Selection border (orange instead of white when the photo is marked).
	u16 white = marked && marked[sel] ? orange : (BIT(15) | RGB15(31, 31, 31));
	int dy    = py[sel] - scroll;
	hline(gfx, px[sel], dy, THUMB_W, white);
	hline(gfx, px[sel], dy + 1, THUMB_W, white);
	hline(gfx, px[sel], dy + THUMB_H - 2, THUMB_W, white);
	hline(gfx, px[sel], dy + THUMB_H - 1, THUMB_W, white);
	for(int y = 0; y < THUMB_H; y++) {
		int yy = dy + y;
		if(yy < 0 || yy >= SCREEN_H)
			continue;
		gfx[yy * 256 + px[sel]]               = white;
		gfx[yy * 256 + px[sel] + 1]           = white;
		gfx[yy * 256 + px[sel] + THUMB_W - 2] = white;
		gfx[yy * 256 + px[sel] + THUMB_W - 1] = white;
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
				strcpy(when, "UNKNOWN DATE");
			sprintf(line, "%s_%04d", ph[*idx].isVideo ? "VID" : "IMG", ph[*idx].num);
			uiHeader(line);
			uiTextCenter(44, when, UI_DIM);
			sprintf(line, "%d/%d", *idx + 1, count);
			uiTextCenter(58, line, UI_DIM);
			int y = 88;
			uiKey(y, "< >", "BROWSE");
			if(ph[*idx].isVideo)
				uiKey(y += 16, "A", "PLAY");
			else
				uiKey(y += 16, "A", "SEND TO DSI ALBUM");
			uiKey(y += 16, "X", "DELETE");
			uiKey(y += 16, "B", "BACK");
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
				uiStatus("EXPORTING...");
				uiStatus(exportToAlbum(ph[*idx].num) ? "SENT TO DSI ALBUM" : "EXPORT FAILED");
			}
		} else if(pressed & KEY_X) {
			char msg[32];
			sprintf(msg, "DELETE %s_%04d?", ph[*idx].isVideo ? "VID" : "IMG", ph[*idx].num);
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
		uiHeader("ALBUM");
		uiStatus("LOADING...");
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
					sprintf(msg, "DELETE %d SELECTED PHOTO%s?", nMarked, nMarked == 1 ? "" : "S");
				else
					sprintf(msg, "DELETE IMG_%04d?", ph[sel].num);
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
				uiHeader("ALBUM");
				sprintf(line, "IMG_%04d  %d/%d%s", ph[sel].num, sel + 1, count, marked[sel] ? "  *" : "");
				uiTextCenter(44, line, UI_FG);
				if(nMarked) {
					sprintf(line, "%d SELECTED", nMarked);
					uiTextCenter(58, line, UI_DIM);
				}
				int y = 82;
				uiKey(y, "+", "NAVIGATE");
				uiKey(y += 16, "L/R", "JUMP DAY");
				uiKey(y += 16, "A", "VIEW");
				uiKey(y += 16, "Y", "SELECT");
				uiKey(y += 16, "X", "DELETE");
				uiKey(y += 16, "B", "CAMERA");
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
		uiStatus("NO PHOTOS YET");
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

	uiHeader("DSI CAMERA");
	uiStatus("BOOTING...");

	bool fatInited = fatInitDefault();
	if(fatInited) {
		mkdir(RAW_DIR, 0777);
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
		uiStatus("NO SD CARD");
	}

	uiStatus("INITIALIZING...");
	pxiWaitRemote(PXI_CAMERA); // Wait for ARM7 to initialize PXI
	cameraInit();
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

	uiCameraScreen(camera, fatInited);

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
				while(cameraTransferActive())
					swiWaitForVBlank();
				cameraTransferStop();
				cameraDeactivate(camera);
				drainJobs(); // don't sleep with photos half-written
				pmEnterSleep();
				cameraActivate(camera);
				continue;
			}
			if(!cameraTransferActive())
				cameraTransferStart(gfx, CAPTURE_MODE_PREVIEW);
			pressed = keysDown();
		} while(!pressed);

		if(pressed & KEY_A) {
			// Wait for previous transfer to finish
			while(cameraTransferActive())
				swiWaitForVBlank();
			cameraTransferStop();

			// Switch camera
			camera   = camera == CAM_INNER ? CAM_OUTER : CAM_INNER;
			s_camera = camera;
			cameraActivate(camera);

			uiCameraScreen(camera, fatInited);
		} else if(fatInited && pressed & KEY_Y) {
			s_videoMode = !s_videoMode;
			uiCameraScreen(camera, fatInited);
		} else if(fatInited && pressed & (KEY_L | KEY_R)) {
			if(s_videoMode) {
				// Tap L/R to record, tap again to stop.
				while(cameraTransferActive())
					swiWaitForVBlank();
				drainJobs(); // finish any pending photo writes first
				recordVideo(gfx, s_nextVidNum++);
				uiCameraScreen(camera, fatInited);
			} else {
				// Hold L/R to keep taking photos continuously.
				do {
					captureRaw(gfx);
					scanKeys();
					// Don't keep shooting into a closed lid (e.g. in a pocket).
				} while((keysHeld() & (KEY_L | KEY_R)) && !(keysHeld() & KEY_LID));
				uiSpace(); // refresh the free-space line after the burst
			}
		} else if(fatInited && pressed & KEY_SELECT) {
			// Stop the live preview and browse photos.
			while(cameraTransferActive())
				swiWaitForVBlank();
			cameraTransferStop();
			drainJobs(); // so the newest shots are in the list
			viewer(gfx, camera);
		} else if(pressed & KEY_START) {
			// Disable camera so the light turns off
			cameraDeactivate(camera);
			drainJobs();

			return 0;
		}
	}
}
