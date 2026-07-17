#ifndef DSI_PHOTO_H
#define DSI_PHOTO_H

#include <nds/ndstypes.h>

// Builds a complete, Album-compatible DSi photo from a 640x480 RGB buffer:
// the fixed DSi Exif header + a 160x120 JPEG thumbnail + the main JPEG, then
// AES-CCM signs the whole file. `datetime19` is a 19-char "YYYY:MM:DD HH:MM:SS"
// string, `key` the 16-byte camera key from the ARM7 BIOS, `nonce` 12 bytes.
// Returns a malloc'd file buffer via *outBuf and its length via *outLen; the
// caller must free(*outBuf).
void buildAndSignDsiPhoto(
	u8 *rgb640x480, const char *datetime19, const u8 key[16], const u8 nonce[12], u8 **outBuf, int *outLen);

#endif // DSI_PHOTO_H
