#include "dsi_sign.h"
#include "dsi_template.h"
#include "dsi.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// CMAC subkey doubling in GF(2^128) (little-endian words), matching the DSi
// hardware quirk reimplemented by MrNbaYoh's dsi_jpeg_signature_tool. Written
// byte-wise (via memcpy into aligned locals) because the ARM9 does rotated/
// garbage reads on unaligned u32/u64 loads, unlike x86.
static void weirdFunc(u8 block[16]) {
	u32 b[4];
	memcpy(b, block, 16);
	u32 tmp = b[3];
	u64 lo = (u64)b[0] | ((u64)b[1] << 32);
	u64 mid = (u64)b[1] | ((u64)b[2] << 32);
	u64 hi = (u64)b[2] | ((u64)b[3] << 32);
	b[3] = (u32)(hi >> 31);
	b[2] = (u32)(mid >> 31);
	b[1] = (u32)(lo >> 31);
	b[0] = b[0] << 1;
	if(tmp >> 31)
		b[0] ^= 0x87;
	memcpy(block, b, 16);
}

static void xorBlock(u8 dst[16], const u8 src[16]) {
	for(int i = 0; i < 16; i++)
		dst[i] ^= src[i];
}

void dsiSignPhoto(u8 *file, u32 size, const u8 key[16], const u8 nonce[12]) {
	u8 mutKey[16];
	memcpy(mutKey, key, 16);

	// Work on a zero-padded copy; the padding is signed but never written out.
	u32 total = (size + 0xF) & ~0xFu;
	u8 *buf = (u8 *)malloc(total);
	memcpy(buf, file, size);
	memset(buf + size, 0, total - size);
	memset(&buf[DSI_OFF_SIG], 0, 0x1C);

	u8 nonceCopy[12];
	memcpy(nonceCopy, nonce, 12);

	dsi_context ctrCtx, ccmCtx;
	u8 block[16];
	memset(block, 0, 16);
	dsi_init_ctr(&ctrCtx, mutKey, block);
	dsi_crypt_ctr_block(&ctrCtx, block, block);
	weirdFunc(block);

	u8 finalBytes = ((size - 1) & 0xF) + 1;
	if(finalBytes == 0x10) {
		xorBlock(block, &buf[size - finalBytes]);
	} else {
		u8 tmpBlock[16];
		memset(tmpBlock, 0, 16);
		memcpy(&tmpBlock[16 - finalBytes], &buf[size - finalBytes], finalBytes);
		tmpBlock[15 - finalBytes] = 0x80;
		weirdFunc(block);
		xorBlock(block, tmpBlock);
	}
	memcpy(&buf[size - finalBytes], block, 16);

	dsi_init_ccm(&ccmCtx, mutKey, 16, 0, total, nonceCopy);
	u8 *out = (u8 *)malloc(total);
	u8 mac[16];
	dsi_encrypt_ccm(&ccmCtx, buf, out, total, mac);

	memcpy(&file[DSI_OFF_NONCE], nonce, 12);
	memcpy(&file[DSI_OFF_MAC], mac, 16);

	free(buf);
	free(out);
}
