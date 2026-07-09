#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "dsi_sign.h"
static unsigned char REAL[16]={0x70,0x88,0x52,0x06,0xDF,0xE5,0x01,0x6D,0x45,0xEA,0xC5,0x23,0x33,0xD6,0x44,0x6F};
int main(int c,char**v){
  FILE*f=fopen(v[1],"rb");fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
  unsigned char*d=malloc(n);fread(d,1,n,f);fclose(f);
  unsigned char want[16];memcpy(want,&d[0x196],16);
  unsigned char nonce[12];memcpy(nonce,&d[0x18A],12);
  // build candidate list
  unsigned char cand[8][16]; const char*name[8]; int nc=0;
  memcpy(cand[nc],REAL,16);name[nc++]="real";
  memset(cand[nc],0,16);name[nc++]="zeros";
  // word byte-swap
  for(int i=0;i<16;i+=4){cand[nc][i]=REAL[i+3];cand[nc][i+1]=REAL[i+2];cand[nc][i+2]=REAL[i+1];cand[nc][i+3]=REAL[i];}name[nc++]="word-byteswap";
  for(int i=0;i<16;i++)cand[nc][i]=REAL[15-i];name[nc++]="full-reverse";
  // 0xFF fill
  memset(cand[nc],0xFF,16);name[nc++]="0xFF";
  for(int k=0;k<nc;k++){
    unsigned char t[n]; memcpy(t,d,n);
    dsiSignPhoto(t,(unsigned)n,cand[k],nonce);
    printf("%-16s -> %s\n",name[k],memcmp(&t[0x196],want,16)==0?"MATCH":"no");
  }
  printf("device MAC: ");for(int i=0;i<16;i++)printf("%02X",want[i]);printf("\n");
  return 0;
}
