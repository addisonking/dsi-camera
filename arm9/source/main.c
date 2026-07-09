#include "camera.h"
#include "lodepng.h"
#include "version.h"

#include <calico/nds/pm.h>
#include <calico/nds/pxi.h>
#include <dirent.h>
#include <fat.h>
#include <math.h>
#include <nds.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "exif.h"
#include "dsi_photo.h"
#include "pit.h"

#define PHOTO_DIR "/DCIM/100NIN02"

#define YUV_TO_R(Y, Cr) clamp(Y + Cr + (Cr >> 2) + (Cr >> 3) + (Cr >> 5), 0, 0xFF)
#define YUV_TO_G(Y, Cb, Cr) \
	clamp(Y - ((Cb >> 2) + (Cb >> 4) + (Cb >> 5)) - ((Cr >> 1) + (Cr >> 3) + (Cr >> 4) + (Cr >> 5)), 0, 0xFF)
#define YUV_TO_B(Y, Cb) clamp(Y + Cb + (Cb >> 1) + (Cb >> 2) + (Cb >> 6), 0, 0xFF)

int clamp(int val, int min, int max) { return val < min ? min : (val > max ? max : val); }

int getImageNumber() {
	int highest = 0;

	mkdir("/DCIM", 0777);
	mkdir(PHOTO_DIR, 0777);

	DIR *pdir = opendir(PHOTO_DIR);
	if(pdir == NULL) {
		printf("Unable to open directory");
		return 1;
	} else {
		while(true) {
			struct dirent *pent = readdir(pdir);
			if(pent == NULL)
				break;

			if(strncmp(pent->d_name, "HNI_", 4) == 0) {
				int val = atoi(pent->d_name + 4);
				if(val > highest)
					highest = val;
			}
		}
		closedir(pdir);
	}

	return highest + 1;
}

// Captures one full-res frame from the active camera and saves it as a signed,
// Album-compatible DSi JPEG. Assumes a transfer may be in progress (waits for it).
// `previewGfx` is the top-screen bitmap (256-wide); the captured frame is drawn
// to it so the preview keeps updating during a burst.
void captureAndSave(u16 *previewGfx) {
	printf("Capturing... ");

	// Wait for previous transfer to finish
	while(cameraTransferActive())
		swiWaitForVBlank();

	// Get image
	u16 *yuv = (u16 *)malloc(640 * 480 * sizeof(u16));
	cameraTransferStart(yuv, CAPTURE_MODE_CAPTURE);
	while(cameraTransferActive())
		swiWaitForVBlank();
	cameraTransferStop();

	// Show the frame we just grabbed (nearest-neighbour 640x480 -> 256x192) so
	// the preview isn't frozen while we encode/sign during a burst. Capture
	// mode outputs raw YUV422, so convert to RGB555 here (the live preview uses
	// the camera's hardware YUV->RGB555, but capture bypasses it).
	if(previewGfx) {
		for(int y = 0; y < 192; y++) {
			int sy = (y * 480) / 192;
			for(int x = 0; x < 256; x++) {
				int sx = (x * 640) / 256 & ~1; // keep YUV pair alignment
				u8 *val = (u8 *)(yuv + sy * 640 + sx);
				int Y = val[(x & 1) ? 2 : 0];
				int Cb = val[1] - 0x80;
				int Cr = val[3] - 0x80;
				int r = YUV_TO_R(Y, Cr) >> 3;
				int g = YUV_TO_G(Y, Cb, Cr) >> 3;
				int b = YUV_TO_B(Y, Cb) >> 3;
				// NDS 16-bit bitmap is aBBBBBGGGGGRRRRR (blue high, red low)
				previewGfx[y * 256 + x] = BIT(15) | (b << 10) | (g << 5) | r;
			}
		}
	}

	printf("Done!\nSaving JPEG... ");

	// YUV422 -> RGB
	u8 *rgb = (u8 *)malloc(640 * 480 * 3);
	for(int py = 0; py < 480; py++) {
		for(int px = 0; px < 640; px += 2) {
			u8 *val = (u8 *)(yuv + py * 640 + px);

			// Get YUV values
			int Y1 = val[0];
			int Cb = val[1] - 0x80;
			int Y2 = val[2];
			int Cr = val[3] - 0x80;

			u8 *dst = rgb + py * (640 * 3) + px * 3;
			// First pixel R, G, B
			dst[0] = YUV_TO_R(Y1, Cr);
			dst[1] = YUV_TO_G(Y1, Cb, Cr);
			dst[2] = YUV_TO_B(Y1, Cb);
			// Second pixel R, G, B
			dst[3] = YUV_TO_R(Y2, Cr);
			dst[4] = YUV_TO_G(Y2, Cb, Cr);
			dst[5] = YUV_TO_B(Y2, Cb);
		}
	}
	free(yuv);

	// Fetch the 16-byte camera signing key from the ARM7 (it read it from the
	// BIOS-populated WRAM at boot), as 8 halfwords because PXI immediates are
	// only 26-bit (full words would be truncated).
	u8 key[16];
	u16 *kh = (u16 *)key;
	for(int i = 0; i < 8; i++)
		kh[i] = pxiSendAndReceive(PXI_CAMERA, CAM_GET_KEY0 + i);

	// Exif date/time string (falls back if the RTC isn't synced).
	char dt[20];
	time_t t = time(NULL);
	struct tm *tmv = localtime(&t);
	if(tmv && tmv->tm_year > 100)
		strftime(dt, sizeof(dt), "%Y:%m:%d %H:%M:%S", tmv);
	else
		strcpy(dt, "2024:01:01 00:00:00");

	// Register the photo in the DSi Camera index (pit.bin) so the stock Album
	// shows it with the right timestamp, and use its counter for the filename.
	// Falls back to a directory scan if pit.bin is missing/unwritable.
	u32 dsiTs = (u32)((u32)t - 946684800u); // seconds 1970->2000 epoch shift
	int imgNum = pitAddPhoto(dsiTs);
	if(imgNum < 0)
		imgNum = getImageNumber();

	// Nonce is stored in the file (not secret); just make it vary.
	u8 nonce[12];
	for(int i = 0; i < 12; i++)
		nonce[i] = (u8)(t >> ((i & 3) * 8)) ^ (u8)(imgNum * (i + 1));

	u8 *photo = NULL;
	int photoLen = 0;
	buildAndSignDsiPhoto(rgb, dt, key, nonce, &photo, &photoLen);
	free(rgb);

	char imgName[40];
	sprintf(imgName, PHOTO_DIR "/HNI_%04d.JPG", imgNum);
	FILE *f = fopen(imgName, "wb");
	if(f) {
		fwrite(photo, 1, photoLen, f);
		fclose(f);
	}
	free(photo);

	printf("Done!\nSaved to:\n%s\n\n", imgName);
}

int main(int argc, char **argv) {
	consoleDemoInit();
	vramSetBankA(VRAM_A_MAIN_BG);
	videoSetMode(MODE_5_2D);
	int bg3Main = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 1, 0);

	printf("dsi-camera " VER_NUMBER "\n");

	bool fatInited = fatInitDefault();
	if(fatInited) {
		mkdir("/DCIM", 0777);
		mkdir("/DCIM/100DSI00", 0777);
	} else {
		printf("FAT init failed, photos cannot\nbe saved.\n");
	}

	printf("Initializing...\n");
	pxiWaitRemote(PXI_CAMERA); // Wait for ARM7 to initialize PXI
	cameraInit();

	Camera camera = CAM_OUTER;
	cameraActivate(camera);

	if(fatInited)
		printf("\nA: swap camera\nHold L/R: take photos\nSTART or POWER: exit\n");
	else
		printf("\nA: swap camera\nSTART or POWER: exit\n");

	while(1) {
		u16 pressed;
		do {
			swiWaitForVBlank();
			// A tap of the power button asks the app to exit; leaving returns
			// to the main menu (Unlaunch).
			if(pmShouldReset()) {
				cameraDeactivate(camera);
				return 0;
			}
			if(!cameraTransferActive())
				cameraTransferStart(bgGetGfxPtr(bg3Main), CAPTURE_MODE_PREVIEW);
			scanKeys();
			pressed = keysDown();
		} while(!pressed);

		if(pressed & KEY_A) {
			// Wait for previous transfer to finish
			while(cameraTransferActive())
				swiWaitForVBlank();
			cameraTransferStop();

			// Switch camera
			camera = camera == CAM_INNER ? CAM_OUTER : CAM_INNER;
			cameraActivate(camera);

			printf("Swapped to %s camera\n", camera == CAM_INNER ? "inner" : "outer");
		} else if(fatInited && pressed & (KEY_L | KEY_R)) {
			// Hold L/R to keep taking photos continuously.
			do {
				captureAndSave(bgGetGfxPtr(bg3Main));
				scanKeys();
			} while(keysHeld() & (KEY_L | KEY_R));
		} else if(pressed & KEY_START) {
			// Disable camera so the light turns off
			cameraDeactivate(camera);

			return 0;
		}
	}
}
