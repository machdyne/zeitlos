/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host tests for ecdsa.c, against signatures OpenSSL produced.
 *
 * The four positive cases are deliberately every combination of curve
 * and hash size, not just the two matching ones: SEC1 truncates a
 * hash longer than the order and uses a shorter one whole, and both
 * of those happen on real chains -- a P-384 key signing with SHA-256
 * is entirely legal and does appear.
 *
 * The negative cases are the ones that matter. A verifier missing the
 * on-curve check, or accepting r or s of zero, verifies every genuine
 * signature perfectly well and also accepts forgeries.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../ecdsa.h"
#include "ecdsa_vectors.h"
static int f,ck;
static void t(int c,const char*w){ck++;if(!c){f++;printf("FAIL: %s\n",w);}}
int main(void){
 t(ec_verify(EC_CURVE_P256,p256_pub,65,p256_r,32,p256_s,32,p256_h,32),"P-256 + SHA-256");
 t(ec_verify(EC_CURVE_P384,p384_pub,97,p384_r,48,p384_s,48,p384_h,48),"P-384 + SHA-384");
 t(ec_verify(EC_CURVE_P384,p384x_pub,97,p384x_r,48,p384x_s,48,p384x_h,32),"P-384 + SHA-256 (short hash)");
 t(ec_verify(EC_CURVE_P256,p256x_pub,65,p256x_r,32,p256x_s,32,p256x_h,48),"P-256 + SHA-384 (truncated)");
 {uint8_t h[48];memcpy(h,p384_h,48);h[0]^=1;
  t(!ec_verify(EC_CURVE_P384,p384_pub,97,p384_r,48,p384_s,48,h,48),"P-384 rejects wrong hash");}
 {uint8_t p[97];memcpy(p,p384_pub,97);p[50]^=1;
  t(!ec_verify(EC_CURVE_P384,p,97,p384_r,48,p384_s,48,p384_h,48),"P-384 rejects off-curve point");}
 {uint8_t z[48]={0};
  t(!ec_verify(EC_CURVE_P384,p384_pub,97,z,48,p384_s,48,p384_h,48),"P-384 rejects r=0");}
 t(!ec_verify(EC_CURVE_P384,p384_pub,97,p384_r,48,p384_s,48,p256_h,32),"P-384 rejects wrong message");
 t(!ec_verify(EC_CURVE_P256,p384_pub,97,p384_r,48,p384_s,48,p384_h,48),"P-256 rejects a P-384 point");
 t(ec_point_len(EC_CURVE_P256)==65 && ec_point_len(EC_CURVE_P384)==97,"point lengths");
 {clock_t t0=clock();for(int i=0;i<20;i++)ec_verify(EC_CURVE_P256,p256_pub,65,p256_r,32,p256_s,32,p256_h,32);
  double a=(double)(clock()-t0)/CLOCKS_PER_SEC*1000/20;
  t0=clock();for(int i=0;i<20;i++)ec_verify(EC_CURVE_P384,p384_pub,97,p384_r,48,p384_s,48,p384_h,48);
  double b=(double)(clock()-t0)/CLOCKS_PER_SEC*1000/20;
  printf("verify: P-256 %.2f ms, P-384 %.2f ms (%.1fx)\n",a,b,b/a);}
 printf("%s: %d checks, %d failures\n",f?"FAIL":"ok",ck,f);return f?1:0;}
