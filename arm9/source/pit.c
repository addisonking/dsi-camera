#include "pit.h"

#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The DSi Camera app's photo index, same path on every region.
#ifndef PIT_PATH
	#define PIT_PATH "/private/ds/app/484E494A/pit.bin"
#endif
#define PIT_ENTRY_START 0x18
#define PIT_ENTRY_SIZE 0x10

static u16 rd16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
static void wr16(u8 *p, u16 v) {
	p[0] = v;
	p[1] = v >> 8;
}
static void wr32(u8 *p, u32 v) {
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

// CRC16 with polynomial 0xA001, initial value 0 (same as the DS BIOS swiCRC16).
static u16 pitCrc(const u8 *data, long len) {
	u16 c = 0;
	for(long i = 0; i < len; i++) {
		c ^= data[i];
		for(int b = 0; b < 8; b++)
			c = (c & 1) ? ((c >> 1) ^ 0xA001) : (c >> 1);
	}
	return c;
}

int pitAddPhoto(u32 dsiTimestamp, int *outFolder) {
	FILE *f = fopen(PIT_PATH, "rb");
	if(!f)
		return -1;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	rewind(f);
	if(len < PIT_ENTRY_START + PIT_ENTRY_SIZE) {
		fclose(f);
		return -1;
	}
	u8 *d = (u8 *)malloc(len);
	if(!d) {
		fclose(f);
		return -1;
	}
	fread(d, 1, len, f);
	fclose(f);

	if(memcmp(d, "0TIP00_1", 8) != 0) {
		free(d);
		return -1;
	}

	u16 folderM100 = rd16(d + 0x0C);
	u16 nextFileM1 = rd16(d + 0x0E);
	int num        = nextFileM1 + 1; // file number to assign to this photo

	// A folder holds 100 files; past that, roll over to the next folder
	// (DCIM/<100+folder>NIN02) like the stock camera does.
	if(num > 100) {
		folderM100++;
		num = 1;
	}
	if(folderM100 > 0x3FF) { // folder field is 10 bits
		free(d);
		return -1;
	}

	// Find the first free entry slot (used bit clear).
	long slot = -1;
	for(long off = PIT_ENTRY_START; off + PIT_ENTRY_SIZE <= len; off += PIT_ENTRY_SIZE) {
		if(!(rd32(d + off + 0xC) & 1)) {
			slot = off;
			break;
		}
	}
	if(slot < 0) {
		free(d);
		return -1;
	}

	// flags: used | folder(bits1-10) | file-1(bits11-17); sticker/type/status 0.
	u32 flags = 1u | ((u32)folderM100 << 1) | ((u32)(num - 1) << 11);
	wr32(d + slot + 0x0, dsiTimestamp);
	wr32(d + slot + 0x4, 0);
	wr32(d + slot + 0x8, 0);
	wr32(d + slot + 0xC, flags);

	wr16(d + 0x0C, folderM100);
	wr16(d + 0x0E, (u16)num); // next photo file number minus 1

	// Recompute CRC16 over the whole file with the CRC field zeroed.
	d[0x14] = 0;
	d[0x15] = 0;
	wr16(d + 0x14, pitCrc(d, len));

	f = fopen(PIT_PATH, "wb");
	if(!f) {
		free(d);
		return -1;
	}
	fwrite(d, 1, len, f);
	fclose(f);
	free(d);
	if(outFolder)
		*outFolder = 100 + folderM100;
	return num;
}
