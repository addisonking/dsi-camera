#include "night.h"

#include <stdlib.h>
#include <string.h>

struct NightStack {
	const uint8_t *reference;
	uint16_t *sum;
	uint8_t *count;
	unsigned frames;
};

NightStack *nightCreate(const uint8_t *reference) {
	NightStack *s = calloc(1, sizeof(*s));
	if(!s)
		return NULL;
	s->sum   = malloc(NIGHT_BYTES * sizeof(*s->sum));
	s->count = malloc(NIGHT_WIDTH * NIGHT_HEIGHT / 2);
	if(!s->sum || !s->count) {
		nightFree(s);
		return NULL;
	}
	s->reference = reference;
	s->frames    = 1;
	for(int i = 0; i < NIGHT_BYTES; i++)
		s->sum[i] = reference[i];
	memset(s->count, 1, NIGHT_WIDTH * NIGHT_HEIGHT / 2);
	return s;
}

// Sparse luma alignment. Even horizontal shifts preserve YUV422 chroma pairs.
// Prefer zero shift on a tie so a flat dark sky doesn't drift arbitrarily.
static unsigned alignmentError(const uint8_t *a, const uint8_t *b, int dx, int dy) {
	unsigned error = 0;
	for(int y = 16; y < NIGHT_HEIGHT - 16; y += 8) {
		for(int x = 16; x < NIGHT_WIDTH - 16; x += 8) {
			int p = (y * NIGHT_WIDTH + x) * 2;
			int q = ((y + dy) * NIGHT_WIDTH + x + dx) * 2;
			error += abs(a[p] - b[q]);
		}
	}
	return error;
}

bool nightAdd(NightStack *s, const uint8_t *frame) {
	if(s->frames >= NIGHT_FRAMES)
		return false;
	int dx = 0, dy = 0;
	unsigned best = alignmentError(s->reference, frame, 0, 0);
	for(int y = -8; y <= 8; y++) {
		for(int x = -8; x <= 8; x += 2) {
			unsigned error = alignmentError(s->reference, frame, x, y);
			if(error < best) {
				best = error;
				dx   = x;
				dy   = y;
			}
		}
	}
	// A changed scene or a large exposure jump should keep the reference.
	if(best > 28u * 56 * 76)
		return false;
	s->frames++;
	for(int y = 0; y < NIGHT_HEIGHT; y++) {
		int sy = y + dy;
		if(sy < 0 || sy >= NIGHT_HEIGHT)
			continue;
		for(int x = 0; x < NIGHT_WIDTH; x += 2) {
			int sx = x + dx;
			if(sx < 0 || sx >= NIGHT_WIDTH - 1)
				continue;
			int p              = (y * NIGHT_WIDTH + x) * 2;
			int q              = (sy * NIGHT_WIDTH + sx) * 2;
			const uint8_t *ref = s->reference + p;
			const uint8_t *src = frame + q;
			// Reject moving subjects locally instead of leaving ghost trails.
			if(abs(ref[0] - src[0]) > 24 || abs(ref[2] - src[2]) > 24 || abs(ref[1] - src[1]) > 32 ||
			   abs(ref[3] - src[3]) > 32)
				continue;
			for(int c = 0; c < 4; c++)
				s->sum[p + c] += src[c];
			s->count[p / 4]++;
		}
	}
	return true;
}

void nightFinish(NightStack *s, uint8_t *out) {
	for(int p = 0; p < NIGHT_BYTES; p += 4) {
		unsigned n = s->count[p / 4];
		for(int c = 0; c < 4; c++)
			out[p + c] = (s->sum[p + c] + n / 2) / n;
	}
}

void nightFree(NightStack *s) {
	if(!s)
		return;
	free(s->sum);
	free(s->count);
	free(s);
}
