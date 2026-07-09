#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "dsi_sign.h"
int main(){
  FILE*f=fopen("realphoto.jpg","rb"); fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
  unsigned char*d=malloc(n); fread(d,1,n,f); fclose(f);
  unsigned char want_mac[16]; memcpy(want_mac,&d[0x196],16);
  unsigned char nonce[12]; memcpy(nonce,&d[0x18A],12);
  unsigned char key[16]={0x70,0x88,0x52,0x06,0xDF,0xE5,0x01,0x6D,0x45,0xEA,0xC5,0x23,0x33,0xD6,0x44,0x6F};
  dsiSignPhoto(d,(unsigned)n,key,nonce);
  printf("computed MAC: "); for(int i=0;i<16;i++)printf("%02X",d[0x196+i]); printf("\n");
  printf("expected MAC: "); for(int i=0;i<16;i++)printf("%02X",want_mac[i]); printf("\n");
  printf(memcmp(&d[0x196],want_mac,16)==0?"MATCH ✅ signing is correct\n":"MISMATCH ❌\n");
  return 0;
}
