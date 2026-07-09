#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../arm9/source/stb_image_write.h"
#include "dsi_photo.h"
int main(){
  // synthetic 640x480 RGB gradient
  unsigned char* rgb=malloc(640*480*3);
  for(int y=0;y<480;y++)for(int x=0;x<640;x++){unsigned char*p=rgb+(y*640+x)*3;p[0]=x/3;p[1]=y/2;p[2]=(x^y);}
  unsigned char key[16]={0x70,0x88,0x52,0x06,0xDF,0xE5,0x01,0x6D,0x45,0xEA,0xC5,0x23,0x33,0xD6,0x44,0x6F};
  unsigned char nonce[12]={1,2,3,4,5,6,7,8,9,10,11,12};
  unsigned char* out=NULL; int len=0;
  buildAndSignDsiPhoto(rgb,"2026:07:09 12:00:00",key,nonce,&out,&len);
  FILE*f=fopen("out.jpg","wb"); fwrite(out,1,len,f); fclose(f);
  printf("wrote out.jpg %d bytes\n",len);
  return 0;
}
