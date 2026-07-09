#ifndef DSI_SIGN_H
#define DSI_SIGN_H

#include <nds/ndstypes.h>

// Signs an assembled DSi-format JPEG in place: computes the AES-CCM MAC over the
// whole file (with the signature area zeroed + the file zero-padded to 16 bytes)
// and writes the 12-byte nonce and 16-byte MAC into the header at DSI_OFF_NONCE
// / DSI_OFF_MAC. `key` is the 16-byte DSi camera key read from the ARM7 BIOS;
// `nonce` is 12 bytes (any value, it is stored in the file). `file`/`size` are
// the real (unpadded) file bytes and length.
void dsiSignPhoto(u8 *file, u32 size, const u8 key[16], const u8 nonce[12]);

#endif // DSI_SIGN_H
