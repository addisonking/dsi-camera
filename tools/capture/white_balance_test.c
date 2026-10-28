#include "white_balance.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
	const uint8_t original[] = {0, 128, 0, 128, 128, 128, 128, 128, 255, 255, 255, 0};
	uint8_t frame[sizeof(original)];
	memcpy(frame, original, sizeof(frame));
	whiteBalanceYuv(frame, 6, 0);
	assert(memcmp(frame, original, sizeof(frame)) == 0);
	whiteBalanceYuv(frame, 6, 3);
	assert(frame[1] == 128 && frame[3] == 128);
	assert(frame[5] < 128 && frame[7] > 128);
	assert(frame[0] == 0 && frame[4] == 128 && frame[8] == 255);
	memcpy(frame, original, sizeof(frame));
	whiteBalanceYuv(frame, 6, -3);
	assert(frame[5] > 128 && frame[7] < 128);
	assert(frame[9] == 255 && frame[11] == 0);
	uint16_t preview[] = {0x8000, 0xC210, 0xFFFF};
	uint16_t out[3];
	whiteBalancePreview(preview, out, 3, 3);
	assert(out[0] == preview[0]);
	assert((out[1] & 31) >= (preview[1] & 31));
	assert(((out[1] >> 10) & 31) < ((preview[1] >> 10) & 31));
	whiteBalancePreview(preview, out, 3, -3);
	assert((out[1] & 31) <= (preview[1] & 31));
	assert(((out[1] >> 10) & 31) > ((preview[1] >> 10) & 31));
	puts("white balance: auto, warm, cool, black, clipping, preview");
	return 0;
}
