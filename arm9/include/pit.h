#ifndef PIT_H
#define PIT_H

#include <nds/ndstypes.h>

// Registers a new photo in the DSi Camera index file (pit.bin) so the stock
// Album actually shows it (the Album lists only photos in pit.bin, never scans
// the folder) with the given timestamp (seconds since 2000-01-01, local time).
// Appends an entry, bumps the file-number counter, and fixes the CRC16.
// Returns the assigned photo file number (for HNI_%04d naming), or -1 on error.
int pitAddPhoto(u32 dsiTimestamp);

#endif // PIT_H
