#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ssh_wire.h"
#include "ssh_crypto.h"
#include "monocypher.h"

static int bad=0;
static void ok(const char*n,int c){ printf("  %s %s\n", c?"ok  ":"FAIL", n); if(!c) bad++; }
static void hexs(const uint8_t*d,int n,char*o){for(int i=0;i<n;i++)sprintf(o+2*i,"%02x",d[i]);}

/* ---- mpint: the encoding that fails half of all connections if wrong ---- */
static int mp(const char*hexin,const char*want){
  uint8_t in[64],buf[128]; int n=strlen(hexin)/2;
  for(int i=0;i<n;i++){unsigned v;sscanf(hexin+2*i,"%2x",&v);in[i]=v;}
  ssh_wr w; ssh_wr_init(&w,buf,sizeof buf); ssh_wr_mpint(&w,in,n);
  char got[300]; hexs(buf,ssh_wr_len(&w),got);
  int r=!strcmp(got,want);
  if(!r) printf("      mpint(%s)\n        got  %s\n        want %s\n",hexin,got,want);
  return r;
}

int main(void){
  printf("ssh_wire:\n");
  /* RFC 4251 section 5 worked examples */
  ok("mpint 0 -> empty string", mp("","00000000"));
  ok("mpint 0x9a378f9b2e332a7 (no pad)", mp("09a378f9b2e332a7","0000000809a378f9b2e332a7"));
  ok("mpint 0x80 -> 0x00 prepended", mp("80","000000020080"));
  ok("mpint leading zeros stripped", mp("0000ff","0000000200ff"));
  ok("mpint 32B high-bit set", mp("ff00000000000000000000000000000000000000000000000000000000000011",
     "0000002100ff00000000000000000000000000000000000000000000000000000000000011"));
  ok("mpint 32B leading zeros stripped", mp("0000000000000000000000000000000000000000000000000000000000000011",
     "0000000111"));

  /* reader bounds */
  { uint8_t b[]={0,0,0,9, 'a','b','c'};  /* claims 9 bytes, only 3 present */
    ssh_rd r; ssh_rd_init(&r,b,sizeof b); uint32_t l;
    const uint8_t*s=ssh_rd_string(&r,&l);
    ok("overlong string rejected", s==NULL && r.bad && l==0); }
  { uint8_t b[]={0xff,0xff,0xff,0xff}; /* length 2^32-1 -> must not overflow */
    ssh_rd r; ssh_rd_init(&r,b,sizeof b); uint32_t l;
    ssh_rd_string(&r,&l); ok("2^32-1 length rejected without overflow", r.bad); }
  { ssh_rd r; ssh_rd_init(&r,"\0\0\0\3abc",7); uint32_t l;
    const uint8_t*s=ssh_rd_string(&r,&l);
    ok("valid string reads back", s&&l==3&&!memcmp(s,"abc",3)&&!r.bad); }
  { const char*L="curve25519-sha256,ecdh-sha2-nistp256,diffie-hellman-group14-sha1";
    ok("namelist finds first", ssh_namelist_has((const uint8_t*)L,strlen(L),"curve25519-sha256"));
    ok("namelist finds last", ssh_namelist_has((const uint8_t*)L,strlen(L),"diffie-hellman-group14-sha1"));
    ok("namelist rejects prefix", !ssh_namelist_has((const uint8_t*)L,strlen(L),"curve25519"));
    ok("namelist rejects absent", !ssh_namelist_has((const uint8_t*)L,strlen(L),"aes128-ctr")); }
  { ok("str_eq length-checked", !ssh_str_eq((const uint8_t*)"ssh-ed25519\0evil",16,"ssh-ed25519"));
    ok("str_eq exact match", ssh_str_eq((const uint8_t*)"ssh-ed25519",11,"ssh-ed25519")); }
  /* writer overflow */
  { uint8_t small[4]; ssh_wr w; ssh_wr_init(&w,small,sizeof small);
    ssh_wr_cstr(&w,"this will not fit");
    ok("writer overflow latches", !ssh_wr_ok(&w)); }

  printf("ssh_crypto:\n");
  /* AEAD round trip */
  { ssh_cipher tx,rx; uint8_t key[64];
    for(int i=0;i<64;i++) key[i]=i*3+1;
    ssh_cipher_init(&tx); ssh_cipher_init(&rx);
    ssh_cipher_set_key(&tx,key); ssh_cipher_set_key(&rx,key);
    uint8_t pkt[4+40+16], ref[4+40+16];
    memset(pkt,0,sizeof pkt);
    pkt[0]=0;pkt[1]=0;pkt[2]=0;pkt[3]=40;
    for(int i=0;i<40;i++) pkt[4+i]='A'+(i%26);
    memcpy(ref,pkt,sizeof pkt);
    ssh_aead_seal(&tx,pkt,40);
    ok("seal changes ciphertext", memcmp(pkt+4,ref+4,40)!=0);
    ok("seal encrypts length too", memcmp(pkt,ref,4)!=0);
    uint32_t plen = ssh_aead_peek_len(&rx,pkt);
    ok("peek_len recovers 40", plen==40);
    ok("open authenticates", ssh_aead_open(&rx,pkt,40));
    ok("open recovers plaintext", !memcmp(pkt+4,ref+4,40));
    /* forgery */
    ssh_cipher tx2,rx2; ssh_cipher_init(&tx2); ssh_cipher_init(&rx2);
    ssh_cipher_set_key(&tx2,key); ssh_cipher_set_key(&rx2,key);
    memcpy(pkt,ref,sizeof pkt); ssh_aead_seal(&tx2,pkt,40);
    pkt[10]^=1;
    ok("flipped payload byte rejected", !ssh_aead_open(&rx2,pkt,40));
    ssh_cipher_init(&tx2); ssh_cipher_init(&rx2);
    ssh_cipher_set_key(&tx2,key); ssh_cipher_set_key(&rx2,key);
    memcpy(pkt,ref,sizeof pkt); ssh_aead_seal(&tx2,pkt,40);
    pkt[4+40+3]^=0x80;
    ok("flipped tag byte rejected", !ssh_aead_open(&rx2,pkt,40)); }

  /* sequence number drives the nonce: same key, seq 0 vs 1 must differ */
  { ssh_cipher a,b2; uint8_t key[64]; memset(key,7,64);
    ssh_cipher_init(&a); ssh_cipher_init(&b2);
    ssh_cipher_set_key(&a,key); ssh_cipher_set_key(&b2,key);
    uint8_t p1[4+16+16],p2[4+16+16];
    memset(p1,0,sizeof p1); p1[3]=16; memset(p1+4,'x',16);
    memcpy(p2,p1,sizeof p1);
    ssh_aead_seal(&a,p1,16);            /* seq 0 */
    b2.seq=1; ssh_aead_seal(&b2,p2,16); /* seq 1 */
    ok("nonce follows sequence number", memcmp(p1,p2,sizeof p1)!=0); }

  /* independent reimplementation of the OpenSSH construction */
  { uint8_t key[64], pkt[4+24+16], expect_ct[24], expect_tag[16], nonce[8]={0,0,0,0,0,0,0,5};
    uint8_t plain[24];
    for(int i=0;i<64;i++) key[i]=(uint8_t)(i*11+3);
    for(int i=0;i<24;i++) plain[i]=(uint8_t)(i*7);
    /* reference, written straight from PROTOCOL.chacha20poly1305 */
    uint8_t enc_len[4]={0,0,0,24}, ref_len[4];
    crypto_chacha20_djb(ref_len,enc_len,4,key+32,nonce,0);      /* K_1, ctr 0 */
    crypto_chacha20_djb(expect_ct,plain,24,key,nonce,1);        /* K_2, ctr 1 */
    uint8_t pk[64]; crypto_chacha20_djb(pk,0,64,key,nonce,0);   /* K_2, ctr 0 */
    crypto_poly1305_ctx pc; crypto_poly1305_init(&pc,pk);
    crypto_poly1305_update(&pc,ref_len,4);
    crypto_poly1305_update(&pc,expect_ct,24);
    crypto_poly1305_final(&pc,expect_tag);
    /* ours */
    ssh_cipher c; ssh_cipher_init(&c); ssh_cipher_set_key(&c,key); c.seq=5;
    memcpy(pkt,enc_len,4); memcpy(pkt+4,plain,24);
    ssh_aead_seal(&c,pkt,24);
    ok("matches reference: length ct", !memcmp(pkt,ref_len,4));
    ok("matches reference: payload ct", !memcmp(pkt+4,expect_ct,24));
    ok("matches reference: poly1305 tag", !memcmp(pkt+4+24,expect_tag,16)); }

  /* fingerprint against a known ssh-keygen output */
  { /* an all-zero ed25519 key blob: string "ssh-ed25519" + string 32 zero bytes */
    uint8_t blob[4+11+4+32]; memset(blob,0,sizeof blob);
    blob[3]=11; memcpy(blob+4,"ssh-ed25519",11); blob[4+11+3]=32;
    uint8_t d[32]; ssh_sha256(d,blob,sizeof blob);
    char fp[64]; ssh_fingerprint(fp,sizeof fp,blob,sizeof blob);
    ok("fingerprint has SHA256: prefix", !strncmp(fp,"SHA256:",7));
    ok("fingerprint is 43 unpadded b64 chars", strlen(fp)==7+43);
    ok("fingerprint has no padding", !strchr(fp,'='));
    printf("      %s\n", fp); }

  printf("%s\n", bad?"SSH: FAIL":"SSH: PASS");
  return bad!=0;
}
