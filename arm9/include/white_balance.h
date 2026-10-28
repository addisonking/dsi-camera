#ifndef WHITE_BALANCE_H
#define WHITE_BALANCE_H

#include <stddef.h>
#include <stdint.h>

void whiteBalanceYuv(uint8_t *frame, size_t pixels, int warmth);
void whiteBalancePreview(const uint16_t *src, uint16_t *dst, size_t pixels, int warmth);

#endif
