/*
 * Zeitlos -- host test for sha384.c.
 *
 * Three published FIPS 180-4 vectors. The point is not that SHA-384
 * is hard; it is that sha384.c reaches into Monocypher's
 * crypto_sha512_ctx to swap the IV, and that struct is documented as
 * subject to change without notice. A Monocypher update that moves
 * it fails HERE, loudly, rather than silently producing wrong digests
 * -- which in a signature check means rejecting every valid chain.
 */

#include <stdio.h>
#include <string.h>
#include "../sha384.h"
int main(void){uint8_t d[48];int bad=0;
 struct{const char*m;const char*want;}v[]={
  {"abc","cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"},
  {"","38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b"},
  {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
   "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039"}};
 for(int i=0;i<3;i++){char h[97];sha384(d,v[i].m,strlen(v[i].m));
  for(int j=0;j<48;j++)sprintf(h+2*j,"%02x",d[j]);
  if(strcmp(h,v[i].want)){printf("FAIL vector %d\n got  %s\n want %s\n",i,h,v[i].want);bad++;}}
 printf("%s: sha384, 3 published vectors\n",bad?"FAIL":"ok");return bad?1:0;}
