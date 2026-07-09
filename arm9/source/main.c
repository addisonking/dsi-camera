#include "camera.h"
#include "lodepng.h"
#include "version.h"

#include <dirent.h>
#include <fat.h>
#include <math.h>
#include <nds.h>
#include <stdio.h>
#include <sys/stat.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "exif.h"

#define PHOTO_DIR "/DCIM/100NIN02"

#define YUV_TO_R(Y, Cr) clamp(Y + Cr + (Cr >> 2) + (Cr >> 3) + (Cr >> 5), 0, 0xFF)
#define YUV_TO_G(Y, Cb, Cr) \
	clamp(Y - ((Cb >> 2) + (Cb >> 4) + (Cb >> 5)) - ((Cr >> 1) + (Cr >> 3) + (Cr >> 4) + (Cr >> 5)), 0, 0xFF)
#define YUV_TO_B(Y, Cb) clamp(Y + Cb + (Cb >> 1) + (Cb >> 2) + (Cb >> 6), 0, 0xFF)

int clamp(int val, int min, int max) { return val < min ? min : (val > max) ? max : val; }

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
		printf("\nA to swap, L/R to take picture\n");
	else
		printf("\nA to swap\n");

	while(1) {
		u16 pressed;
		do {
			swiWaitForVBlank();
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

			printf("Done!\nSaving BMP... ");

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

			char imgName[40];
			sprintf(imgName, PHOTO_DIR "/HNI_%04d.JPG", getImageNumber());

			// Build the Exif APP1 segment (needs the full-res RGB for its thumbnail)
			// before we touch/free that buffer.
			u8 *app1 = NULL;
			int app1Len = 0;
			buildExifApp1(rgb, &app1, &app1Len);

			// Encode the full photo to memory so we can splice the APP1
			// segment in right after the SOI marker.
			MemBuf mainBuf = {0};
			stbi_write_jpg_to_func(memBufWrite, &mainBuf, 640, 480, 3, rgb, 90);
			free(rgb);

			FILE *f = fopen(imgName, "wb");
			if(f) {
				u8 soi[2] = {0xFF, 0xD8};
				fwrite(soi, 1, 2, f);              // SOI
				fwrite(app1, 1, app1Len, f);        // our Exif APP1 segment
				fwrite(mainBuf.data + 2, 1, mainBuf.len - 2, f); // rest of stb's output (skip its own SOI)
				fclose(f);
			}
			free(app1);
			free(mainBuf.data);

			printf("Done!\nSaved to:\n%s\n\n", imgName);
		} else if(pressed & KEY_START) {
			// Disable camera so the light turns off
			cameraDeactivate(camera);

			return 0;
		}
	}
}
