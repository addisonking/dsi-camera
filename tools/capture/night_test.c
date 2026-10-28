#include "night.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rng = 7;
static int noise(void) {
	rng = rng * 1664525u + 1013904223u;
	return (int)((rng >> 24) % 17) - 8;
}

static void flat(uint8_t *frame, int y) {
	for(int p = 0; p < NIGHT_BYTES; p += 4) {
		frame[p] = frame[p + 2] = y;
		frame[p + 1]            = 110;
		frame[p + 3]            = 145;
	}
}

int main(void) {
	uint8_t *ref = malloc(NIGHT_BYTES), *next = malloc(NIGHT_BYTES), *out = malloc(NIGHT_BYTES);
	assert(ref && next && out);
	flat(ref, 60);
	NightStack *s = nightCreate(ref);
	assert(s);
	nightFinish(s, out);
	assert(memcmp(ref, out, NIGHT_BYTES) == 0);
	for(int n = 0; n < 3; n++)
		assert(nightAdd(s, ref));
	assert(!nightAdd(s, ref));
	nightFinish(s, out);
	assert(memcmp(ref, out, NIGHT_BYTES) == 0);
	nightFree(s);

	// Independent sensor noise should fall without changing chroma or brightness.
	for(int p = 0; p < NIGHT_BYTES; p += 2)
		ref[p] = 60 + noise();
	s = nightCreate(ref);
	for(int n = 0; n < 3; n++) {
		flat(next, 60);
		for(int p = 0; p < NIGHT_BYTES; p += 2)
			next[p] = 60 + noise();
		assert(nightAdd(s, next));
	}
	nightFinish(s, out);
	unsigned before = 0, after = 0;
	for(int p = 0; p < NIGHT_BYTES; p += 2) {
		before += (ref[p] - 60) * (ref[p] - 60);
		after += (out[p] - 60) * (out[p] - 60);
	}
	assert(after * 2 < before);
	nightFree(s);

	// Local motion must retain the first frame, without smearing a bright moon.
	flat(ref, 30);
	flat(next, 30);
	for(int y = 200; y < 240; y++)
		for(int x = 300; x < 340; x++)
			ref[(y * NIGHT_WIDTH + x) * 2] = 220;
	s = nightCreate(ref);
	assert(nightAdd(s, next));
	nightFinish(s, out);
	assert(memcmp(ref, out, NIGHT_BYTES) == 0);
	nightFree(s);

	// Full-frame translation aligns luma and keeps Cb/Cr in their own slots.
	for(int y = 0; y < NIGHT_HEIGHT; y++) {
		for(int x = 0; x < NIGHT_WIDTH; x += 2) {
			int p  = (y * NIGHT_WIDTH + x) * 2;
			ref[p] = ref[p + 2] = 40 + (rng >> 24) % 160;
			rng                 = rng * 1664525u + 1013904223u;
			ref[p + 1]          = 110;
			ref[p + 3]          = 145;
		}
	}
	flat(next, 20);
	for(int y = 0; y < NIGHT_HEIGHT - 3; y++)
		memcpy(next + ((y + 3) * NIGHT_WIDTH + 4) * 2, ref + y * NIGHT_WIDTH * 2, (NIGHT_WIDTH - 4) * 2);
	s = nightCreate(ref);
	assert(nightAdd(s, next));
	nightFinish(s, out);
	assert(memcmp(ref, out, NIGHT_BYTES) == 0);
	nightFree(s);

	flat(ref, 30);
	flat(next, 230);
	s = nightCreate(ref);
	assert(!nightAdd(s, next));
	nightFinish(s, ref); // in-place output is the capture path
	assert(ref[0] == 30);
	nightFree(s);
	free(ref);
	free(next);
	free(out);
	puts("night: identity, frame cap, noise reduction, motion rejection, translation, edges, scene changes passed");
}
