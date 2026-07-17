#ifndef PIT_H
#define PIT_H

#include <nds/ndstypes.h>

// Registers a new photo in the DSi Camera index file (pit.bin) so the stock
// Album actually shows it (the Album lists only photos in pit.bin, never scans
// the folder) with the given timestamp (seconds since 2000-01-01, local time).
// Appends an entry, bumps the file-number counter (rolling over to the next
// folder past 100 files), and fixes the CRC16.
// Returns the assigned photo file number (for HNI_%04d naming) and stores the
// folder number (for DCIM/%03dNIN02 naming) in *outFolder, or returns -1 on
// error (*outFolder untouched).
int pitAddPhoto(u32 dsiTimestamp, int *outFolder);

#endif // PIT_H
