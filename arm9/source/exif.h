#ifndef EXIF_H
#define EXIF_H

#include <nds.h>

typedef struct { u8 *data; int len; int cap; } MemBuf;

// stbi_write_func-compatible callback that appends into a MemBuf.
void memBufWrite(void *context, void *data, int size);

// Builds a full APP1 (Exif) segment (including the FFE1 marker + length) for a
// 640x480 photo, embedding a downsampled JPEG thumbnail. Returns malloc'd buffer
// via *outBuf, length via *outLen. Caller must free(*outBuf).
void buildExifApp1(u8 *rgb640x480, u8 **outBuf, int *outLen);

#endif
