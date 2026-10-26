#ifndef NIGHT_H
#define NIGHT_H

#include <stdbool.h>
#include <stdint.h>

#define NIGHT_WIDTH 640
#define NIGHT_HEIGHT 480
#define NIGHT_BYTES (NIGHT_WIDTH * NIGHT_HEIGHT * 2)
#define NIGHT_FRAMES 4

typedef struct NightStack NightStack;

// The reference stays owned by the caller and unchanged until finish.
NightStack *nightCreate(const uint8_t *reference);
bool nightAdd(NightStack *stack, const uint8_t *frame);
void nightFinish(NightStack *stack, uint8_t *out);
void nightFree(NightStack *stack);

#endif
