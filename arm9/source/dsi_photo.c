#include "dsi_photo.h"

#include "dsi_sign.h"
#include "dsi_template.h"
#include "exif.h"
#include "stb_image_write.h"

#include <stdlib.h>
#include <string.h>

#define THUMB_W 160
#define THUMB_H 120
#define JPEG_QUALITY 95

static void put16be(u8 *p, u16 v) {
	p[0] = v >> 8;
	p[1] = v & 0xFF;
}
static void put32be(u8 *p, u32 v) {
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v & 0xFF;
}

void buildAndSignDsiPhoto(
	u8 *rgb640x480, const char *datetime19, const u8 key[16], const u8 nonce[12], u8 **outBuf, int *outLen) {
	// 1. Thumbnail: 160x120 via 4x box (nearest) downsample, then JPEG encode.
	u8 *thumbRgb = (u8 *)malloc(THUMB_W * THUMB_H * 3);
	for(int y = 0; y < THUMB_H; y++) {
		for(int x = 0; x < THUMB_W; x++) {
			u8 *src = rgb640x480 + ((y * 4) * 640 + (x * 4)) * 3;
			u8 *dst = thumbRgb + (y * THUMB_W + x) * 3;
			dst[0]  = src[0];
			dst[1]  = src[1];
			dst[2]  = src[2];
		}
	}
	MemBuf thumbBuf = {0};
	stbi_write_jpg_to_func(memBufWrite, &thumbBuf, THUMB_W, THUMB_H, 3, thumbRgb, JPEG_QUALITY);
	free(thumbRgb);

	// 2. Main image JPEG (stb, patched for 4:2:2 to match the DSi decoder).
	MemBuf mainBuf = {0};
	stbi_write_jpg_to_func(memBufWrite, &mainBuf, 640, 480, 3, rgb640x480, JPEG_QUALITY);

	// 3. Assemble: header + thumbnail + (main JPEG minus its leading SOI).
	int mainBody = mainBuf.len - 2; // skip stb's FF D8
	int fileLen  = DSI_HEADER_SIZE + thumbBuf.len + mainBody;
	u8 *file     = (u8 *)malloc(fileLen);

	memcpy(file, dsiHeader, DSI_HEADER_SIZE);
	memcpy(file + DSI_THUMB_OFFSET, thumbBuf.data, thumbBuf.len);
	memcpy(file + DSI_THUMB_OFFSET + thumbBuf.len, mainBuf.data + 2, mainBody);

	// 4. Patch the variable header fields.
	memcpy(file + DSI_OFF_DATETIME0, datetime19, 19);
	memcpy(file + DSI_OFF_DATETIME1, datetime19, 19);
	memcpy(file + DSI_OFF_DATETIME2, datetime19, 19);
	put32be(file + DSI_OFF_THUMB_LEN, (u32)thumbBuf.len);
	put16be(file + DSI_OFF_APP1LEN, (u16)((DSI_HEADER_SIZE + thumbBuf.len) - 4));

	free(thumbBuf.data);
	free(mainBuf.data);

	// 5. Sign the whole file.
	dsiSignPhoto(file, fileLen, key, nonce);

	*outBuf = file;
	*outLen = fileLen;
}
