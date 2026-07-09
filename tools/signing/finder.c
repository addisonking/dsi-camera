// Finds the DSi camera JPEG signing key inside an ARM7 BIOS dump.
//
// It reuses the exact AES-CCM signing routine from MrNbaYoh's
// dsi_jpeg_signature_tool (itself based on neimod's taddy tool), then walks the
// BIOS byte-by-byte treating each window as a candidate 16-byte key. For each
// candidate it re-signs a real DSi photo using that photo's own stored nonce and
// checks whether the recomputed MAC matches the photo's stored MAC. A match
// means the candidate window is the real signing key, and its offset is what the
// on-device ARM7 code needs to read.
//
// Usage: finder <arm7_bios.bin> <real_photo.jpg>

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "dsi.h"

#define SIG_OFF 0x18A      // signature area (nonce+mac) in the DSi Exif header
#define NONCE_OFF 0x18A    // 12-byte nonce
#define MAC_OFF 0x196      // 16-byte MAC
#define SIG_LEN 0x1C

static void weird_func(unsigned int block[4]) {
	unsigned int tmp = block[3];
	block[3] = *((uint64_t *)block + 1) >> 31;
	block[2] = *(uint64_t *)(&block[1]) >> 31;
	block[1] = *(uint64_t *)block >> 31;
	block[0] *= 2;
	if(tmp >> 31)
		block[0] ^= 0x87;
}

static void xor_block(unsigned int block[4], unsigned int x[4]) {
	for(int i = 0; i < 4; i++)
		block[i] ^= x[i];
}

// Signs a copy of `file` (size `size`) with `key`, using the nonce already
// stored in the file, and writes the 16-byte MAC to `out_mac`.
static void sign_with_key(const unsigned char *file, unsigned int size,
                          unsigned char key[16], unsigned char out_mac[16]) {
	unsigned int total_size = (size + 0xF) & ~0xFu;
	unsigned char *in_buf = malloc(total_size);
	memcpy(in_buf, file, size);
	memset(in_buf + size, 0, total_size - size);
	memset(&in_buf[SIG_OFF], 0, SIG_LEN);

	unsigned char nonce[12];
	memcpy(nonce, &file[NONCE_OFF], 12);

	dsi_context ctr_ctx, ccm_ctx;
	unsigned char block[16];
	memset(block, 0, 16);
	dsi_init_ctr(&ctr_ctx, key, block);
	dsi_crypt_ctr_block(&ctr_ctx, block, block);
	weird_func((unsigned int *)block);

	unsigned char final_bytes = ((size - 1) & 0xF) + 1;
	if(final_bytes == 0x10) {
		xor_block((unsigned int *)block, (unsigned int *)&in_buf[size - final_bytes]);
	} else {
		unsigned char tmp_block[16];
		memset(tmp_block, 0, 16);
		memcpy(&tmp_block[16 - final_bytes], &in_buf[size - final_bytes], final_bytes);
		tmp_block[15 - final_bytes] = 0x80;
		weird_func((unsigned int *)block);
		xor_block((unsigned int *)block, (unsigned int *)tmp_block);
	}
	memcpy(&in_buf[size - final_bytes], block, 16);

	dsi_init_ccm(&ccm_ctx, key, 16, 0, total_size, nonce);
	unsigned char *out_buf = malloc(total_size);
	dsi_encrypt_ccm(&ccm_ctx, in_buf, out_buf, total_size, out_mac);

	free(in_buf);
	free(out_buf);
}

int main(int argc, char **argv) {
	if(argc != 3) {
		printf("Usage: %s <arm7_bios.bin> <real_photo.jpg>\n", argv[0]);
		return 1;
	}

	FILE *fb = fopen(argv[1], "rb");
	if(!fb) { printf("cannot open bios %s\n", argv[1]); return 1; }
	fseek(fb, 0, SEEK_END);
	long bios_size = ftell(fb);
	rewind(fb);
	unsigned char *bios = malloc(bios_size);
	fread(bios, 1, bios_size, fb);
	fclose(fb);

	FILE *fp = fopen(argv[2], "rb");
	if(!fp) { printf("cannot open photo %s\n", argv[2]); return 1; }
	fseek(fp, 0, SEEK_END);
	unsigned int psize = (unsigned int)ftell(fp);
	rewind(fp);
	unsigned char *photo = malloc(psize);
	fread(photo, 1, psize, fp);
	fclose(fp);

	unsigned char want_mac[16];
	memcpy(want_mac, &photo[MAC_OFF], 16);
	printf("photo size %u, target MAC: ", psize);
	for(int i = 0; i < 16; i++) printf("%02X", want_mac[i]);
	printf("\nscanning %ld BIOS bytes...\n", bios_size);

	for(long off = 0; off + 16 <= bios_size; off++) {
		unsigned char key[16];
		memcpy(key, bios + off, 16);
		unsigned char mac[16];
		sign_with_key(photo, psize, key, mac);
		if(memcmp(mac, want_mac, 16) == 0) {
			printf("\nFOUND KEY at BIOS offset 0x%lX:\n", off);
			for(int i = 0; i < 16; i++) printf("%02X ", key[i]);
			printf("\n");
			return 0;
		}
	}
	printf("no matching key found (photo may not be a genuine DSi capture, "
	       "or offsets differ)\n");
	return 2;
}
