#include "exif.h"

#include "stb_image_write.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

void memBufWrite(void *context, void *data, int size) {
	MemBuf *mb = (MemBuf *)context;
	if(mb->len + size > mb->cap) {
		mb->cap  = (mb->len + size) * 2 + 64;
		mb->data = (u8 *)realloc(mb->data, mb->cap);
	}
	memcpy(mb->data + mb->len, data, size);
	mb->len += size;
}

static void put16(u8 *p, u16 v) {
	p[0] = v >> 8;
	p[1] = v & 0xFF;
}
static void put32(u8 *p, u32 v) {
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v & 0xFF;
}

// Writes one 12-byte IFD entry with a numeric value/offset.
static void putEntry(u8 *p, u16 tag, u16 type, u32 count, u32 valueOrOffset) {
	put16(p, tag);
	put16(p + 2, type);
	put32(p + 4, count);
	put32(p + 8, valueOrOffset);
}

// Same but for inline raw bytes (<=4 bytes), copied verbatim (not treated as a big-endian int).
static void putEntryRaw(u8 *p, u16 tag, u16 type, u32 count, const u8 *bytes, int nbytes) {
	put16(p, tag);
	put16(p + 2, type);
	put32(p + 4, count);
	u8 val[4] = {0, 0, 0, 0};
	memcpy(val, bytes, nbytes);
	memcpy(p + 8, val, 4);
}

void buildExifApp1(u8 *rgb640x480, u8 **outBuf, int *outLen) {
	// 1. Build a 160x120 thumbnail via a simple 4x box downsample
	int tw = 160, th = 120;
	u8 *thumbRgb = (u8 *)malloc(tw * th * 3);
	for(int y = 0; y < th; y++) {
		for(int x = 0; x < tw; x++) {
			int sx = x * 4, sy = y * 4;
			u8 *src = rgb640x480 + (sy * 640 + sx) * 3;
			u8 *dst = thumbRgb + (y * tw + x) * 3;
			dst[0]  = src[0];
			dst[1]  = src[1];
			dst[2]  = src[2];
		}
	}

	MemBuf thumbBuf = {0};
	stbi_write_jpg_to_func(memBufWrite, &thumbBuf, tw, th, 3, thumbRgb, 95);
	free(thumbRgb);

	// 2. Date/time string (falls back if the DSi RTC hasn't been synced by newlib)
	char dt[20];
	time_t t       = time(NULL);
	struct tm *tmv = localtime(&t);
	if(tmv && tmv->tm_year > 100)
		strftime(dt, sizeof(dt), "%Y:%m:%d %H:%M:%S", tmv);
	else
		strcpy(dt, "2024:01:01 00:00:00");

	// 3. Compute offsets (all relative to the start of the TIFF header, i.e. right after "Exif\0\0")
	const int P0               = 8; // IFD0 start
	const int IFD0_SIZE        = 2 + 8 * 12 + 4;
	const int IFD0_EXTRA_START = P0 + IFD0_SIZE;
	const int IFD0_EXTRA_SIZE  = 9 + 11 + 8 + 8 + 5 + 20 + 1;        // +1 pad byte for even alignment
	const int P1               = IFD0_EXTRA_START + IFD0_EXTRA_SIZE; // Exif SubIFD start

	const int SUBIFD_SIZE        = 2 + 8 * 12 + 4;
	const int SUBIFD_EXTRA_START = P1 + SUBIFD_SIZE;
	const int SUBIFD_EXTRA_SIZE  = 20 + 20;
	const int P2                 = SUBIFD_EXTRA_START + SUBIFD_EXTRA_SIZE; // IFD1 start

	const int IFD1_SIZE        = 2 + 6 * 12 + 4;
	const int IFD1_EXTRA_START = P2 + IFD1_SIZE;
	const int IFD1_EXTRA_SIZE  = 8 + 8;
	const int P3               = IFD1_EXTRA_START + IFD1_EXTRA_SIZE; // thumbnail JPEG bytes start

	const int TIFF_LEN = P3 + thumbBuf.len;

	u8 *tiff = (u8 *)calloc(1, TIFF_LEN);

	// TIFF header
	tiff[0] = 'M';
	tiff[1] = 'M';
	tiff[2] = 0;
	tiff[3] = 42;
	put32(tiff + 4, P0);

	// ---- IFD0 ----
	u8 *p = tiff + P0;
	put16(p, 8);
	p += 2; // 8 entries

	u8 *extra = tiff + IFD0_EXTRA_START;
	int eoff  = IFD0_EXTRA_START;

	memcpy(extra, "Nintendo", 9); // includes NUL
	putEntry(p, 0x010F, 2, 9, eoff);
	p += 12;
	extra += 9;
	eoff += 9;

	memcpy(extra, "NintendoDS", 11);
	putEntry(p, 0x0110, 2, 11, eoff);
	p += 12;
	extra += 11;
	eoff += 11;

	put32(extra, 72);
	put32(extra + 4, 1);
	putEntry(p, 0x011A, 5, 1, eoff);
	p += 12;
	extra += 8;
	eoff += 8;

	put32(extra, 72);
	put32(extra + 4, 1);
	putEntry(p, 0x011B, 5, 1, eoff);
	p += 12;
	extra += 8;
	eoff += 8;

	putEntry(p, 0x0128, 3, 1, 2u << 16);
	p += 12; // ResolutionUnit = inches

	memcpy(extra, "EINH", 5);
	putEntry(p, 0x0131, 2, 5, eoff);
	p += 12;
	extra += 5;
	eoff += 5;

	memcpy(extra, dt, 20);
	putEntry(p, 0x0132, 2, 20, eoff);
	p += 12;
	extra += 20;
	eoff += 20;

	putEntry(p, 0x8769, 4, 1, P1);
	p += 12; // ExifOffset -> SubIFD

	put32(p, P2); // next IFD = IFD1 (thumbnail)

	// ---- Exif SubIFD ----
	p = tiff + P1;
	put16(p, 8);
	p += 2;

	extra = tiff + SUBIFD_EXTRA_START;
	eoff  = SUBIFD_EXTRA_START;

	putEntryRaw(p, 0x9000, 7, 4, (const u8 *)"0220", 4);
	p += 12; // ExifVersion

	memcpy(extra, dt, 20);
	putEntry(p, 0x9003, 2, 20, eoff);
	p += 12;
	extra += 20;
	eoff += 20; // DateTimeOriginal

	memcpy(extra, dt, 20);
	putEntry(p, 0x9004, 2, 20, eoff);
	p += 12;
	extra += 20;
	eoff += 20; // DateTimeDigitized

	{
		u8 cc[4] = {1, 2, 3, 0};
		putEntryRaw(p, 0x9101, 7, 4, cc, 4);
		p += 12;
	} // ComponentsConfiguration

	putEntryRaw(p, 0xA000, 7, 4, (const u8 *)"0100", 4);
	p += 12; // FlashPixVersion

	putEntry(p, 0xA001, 3, 1, 1u << 16);
	p += 12; // ColorSpace = sRGB
	putEntry(p, 0xA002, 4, 1, 640);
	p += 12; // ExifImageWidth
	putEntry(p, 0xA003, 4, 1, 480);
	p += 12; // ExifImageLength

	put32(p, 0); // no next IFD

	// ---- IFD1 (thumbnail) ----
	p = tiff + P2;
	put16(p, 6);
	p += 2;

	extra = tiff + IFD1_EXTRA_START;
	eoff  = IFD1_EXTRA_START;

	putEntry(p, 0x0103, 3, 1, 6u << 16);
	p += 12; // Compression = old-style JPEG

	put32(extra, 72);
	put32(extra + 4, 1);
	putEntry(p, 0x011A, 5, 1, eoff);
	p += 12;
	extra += 8;
	eoff += 8;

	put32(extra, 72);
	put32(extra + 4, 1);
	putEntry(p, 0x011B, 5, 1, eoff);
	p += 12;
	extra += 8;
	eoff += 8;

	putEntry(p, 0x0128, 3, 1, 2u << 16);
	p += 12; // ResolutionUnit
	putEntry(p, 0x0201, 4, 1, P3);
	p += 12; // JPEGInterchangeFormat
	putEntry(p, 0x0202, 4, 1, thumbBuf.len);
	p += 12; // JPEGInterchangeFormatLength

	put32(p, 0); // no next IFD

	memcpy(tiff + P3, thumbBuf.data, thumbBuf.len);
	free(thumbBuf.data);

	// ---- Assemble final APP1 segment ----
	int app1Len = 2 /*length field*/ + 6 /*"Exif\0\0"*/ + TIFF_LEN;
	u8 *seg     = (u8 *)malloc(2 /*marker*/ + app1Len);
	seg[0]      = 0xFF;
	seg[1]      = 0xE1;
	put16(seg + 2, (u16)app1Len);
	memcpy(seg + 4, "Exif\0\0", 6);
	memcpy(seg + 10, tiff, TIFF_LEN);
	free(tiff);

	*outBuf = seg;
	*outLen = 2 + app1Len;
}
