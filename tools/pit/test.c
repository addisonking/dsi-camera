#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pit.h"

static unsigned char data[56];

static void write_index(void) {
	FILE *f = fopen("pit.bin", "wb");
	assert(f);
	assert(fwrite(data, 1, sizeof(data), f) == sizeof(data));
	assert(fclose(f) == 0);
}

static void read_index(void) {
	FILE *f = fopen("pit.bin", "rb");
	assert(f);
	assert(fread(data, 1, sizeof(data), f) == sizeof(data));
	assert(fclose(f) == 0);
}

int main(void) {
	int folder = -1;
	assert(pitAddPhoto(0x12345678, &folder) == -1);
	assert(folder == -1);

	FILE *f = fopen("pit.bin", "wb");
	assert(f);
	assert(fwrite("0TIP00_1", 1, 8, f) == 8);
	assert(fclose(f) == 0);
	assert(pitAddPhoto(0x12345678, &folder) == -1);
	assert(folder == -1);

	write_index();
	assert(pitAddPhoto(0x12345678, &folder) == -1);
	assert(folder == -1);

	memcpy(data, "0TIP00_1", 8);
	write_index();
	assert(pitAddPhoto(0x12345678, &folder) == 1);
	assert(folder == 100);
	read_index();
	assert(data[14] == 1 && data[15] == 0);
	assert(data[20] == 0x6a && data[21] == 0xdb);
	assert(memcmp(data + 24, "\x78\x56\x34\x12", 4) == 0);
	assert(data[36] == 1);

	data[14] = 100;
	write_index();
	assert(pitAddPhoto(0x12345678, &folder) == 1);
	assert(folder == 101);
	read_index();
	assert(data[12] == 1 && data[13] == 0 && data[14] == 1 && data[15] == 0);
	assert(data[20] == 0x52 && data[21] == 0x34);
	assert(memcmp(data + 40, "\x78\x56\x34\x12", 4) == 0);
	assert(data[52] == 3);

	folder = -1;
	assert(pitAddPhoto(0x12345678, &folder) == -1);
	assert(folder == -1);

	memset(data, 0, sizeof(data));
	memcpy(data, "0TIP00_1", 8);
	data[14] = 99;
	write_index();
	assert(pitAddPhoto(0x12345678, &folder) == 100);
	assert(folder == 100);
	read_index();
	assert(data[12] == 0 && data[13] == 0 && data[14] == 100);
	assert(data[36] == 1 && data[37] == 0x18 && data[38] == 3);
	folder = -1;

	memset(data, 0, sizeof(data));
	memcpy(data, "0TIP00_1", 8);
	data[12] = 0xff;
	data[13] = 0x03;
	data[14] = 100;
	write_index();
	assert(pitAddPhoto(0x12345678, &folder) == -1);
	assert(folder == -1);
	read_index();
	assert(data[12] == 0xff && data[13] == 0x03 && data[14] == 100);
	assert(data[36] == 0);
	puts("photo index tests passed");
	return 0;
}
