#include <stdio.h>
#include <string.h>
#include "ssh_sha256.h"
static void hex(const uint8_t*d,int n,char*o){for(int i=0;i<n;i++)sprintf(o+2*i,"%02x",d[i]);}
static int check(const char*name,const uint8_t*d,const char*want){
  char got[65]; hex(d,32,got);
  int ok=!strcmp(got,want);
  printf("  %s %s\n    got  %s\n", ok?"ok  ":"FAIL", name, got);
  if(!ok) printf("    want %s\n", want);
  return !ok;
}
int main(void){
  uint8_t h[32]; int bad=0;
  ssh_sha256(h,"",0);
  bad+=check("empty string", h,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  ssh_sha256(h,"abc",3);
  bad+=check("\"abc\"", h,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  ssh_sha256(h,"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",56);
  bad+=check("56-byte (2-block pad)", h,"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  /* exactly 64 bytes: forces the second padding block */
  char b64[65]; memset(b64,'a',64); b64[64]=0;
  ssh_sha256(h,b64,64);
  bad+=check("64 'a' (block boundary)", h,"ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  /* one million 'a' - streaming, odd chunk sizes */
  ssh_sha256_ctx c; ssh_sha256_init(&c);
  char chunk[997]; memset(chunk,'a',sizeof(chunk));
  long left=1000000;
  while(left){ long n = left<(long)sizeof(chunk)?left:(long)sizeof(chunk);
    ssh_sha256_update(&c,chunk,n); left-=n; }
  ssh_sha256_final(&c,h);
  bad+=check("1e6 'a' streamed in 997B chunks", h,"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  printf("%s\n", bad?"SHA-256: FAIL":"SHA-256: PASS");
  return bad;
}
