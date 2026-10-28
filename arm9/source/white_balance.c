#include "white_balance.h"

static int clampByte(int value) { return value < 0 ? 0 : value > 255 ? 255 : value; }
static int roundedDivide(int value, int divisor) {
	return value < 0 ? -((-value + divisor / 2) / divisor) : (value + divisor / 2) / divisor;
}
static int clamp5(int value) { return value < 0 ? 0 : value > 31 ? 31 : value; }

void whiteBalanceYuv(uint8_t *frame, size_t pixels, int warmth) {
	if(!warmth)
		return;
	for(size_t i = 0; i + 1 < pixels; i += 2) {
		uint8_t *pair = frame + i * 2;
		int light     = (pair[0] + pair[2]) / 2;
		pair[1]       = clampByte(pair[1] - warmth * light / 64);
		pair[3]       = clampByte(pair[3] + warmth * light / 128);
	}
}

void whiteBalancePreview(const uint16_t *src, uint16_t *dst, size_t pixels, int warmth) {
	for(size_t i = 0; i < pixels; i++) {
		unsigned pixel = src[i];
		int r = pixel & 31, g = (pixel >> 5) & 31, b = (pixel >> 10) & 31;
		int light = (r * 77 + g * 150 + b * 29) / 256;
		r         = clamp5(r + roundedDivide(warmth * light, 90));
		b         = clamp5(b - roundedDivide(warmth * light, 37));
		dst[i]    = (pixel & 0x8000) | (b << 10) | (g << 5) | r;
	}
}
