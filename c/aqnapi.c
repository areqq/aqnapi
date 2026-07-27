/* aqnapi (wersja C) — niezależny, natywny port aqnapi.py, kompilowany przez
 * cosmocc do uniwersalnej binarki APE. Pokrywa WIĘKSZOŚĆ poleceń i jest 100%
 * bajtowo zgodny z Pythonem dla zaimplementowanego podzbioru.
 *
 * Offline: hash, fps, convert, fpsconv, merge, split, sync, config.
 * Sieć (HTTP, oba buildy): download, get, search, napiprojekt
 *   (download/fileinfo/search/upload 7z-AES + --login), napisy24
 *   (hash/download/getid/search/attach AddSubPrg). Wejście z URL (Range).
 * Wariant TLS (aqnapi-c-tls.com, monorepo + mbedtls) dodaje HTTPS: opensubtitles
 *   (login/search/download), napisy24 weblogin/upload/delete, update, URL https.
 *
 * Pokrywa 100% poleceń Pythona (parytet bajtowy dla zaimplementowanego zakresu).
 */
/* _GNU_SOURCE: fmemopen, strcasestr, strdup, strtok_r na glibc/musl (poza cosmo).
 * Cosmopolitan dostarcza je bez tego makra — ale zdefiniowanie nie szkodzi. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp/strncasecmp (POSIX) */
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
/* zlib: cosmo ma własną ścieżkę; poza cosmo — systemowy nagłówek. */
#ifdef __COSMOPOLITAN__
#include "third_party/zlib/zlib.h"
#else
#include <zlib.h>
#endif

#define VERSION "1.0.17"
#define CHUNK_10MB (10*1024*1024)
#define OSH_CHUNK 65536
#define DEFAULT_FPS 23.976
#define MAX_DISPLAY_MS 10000

/* ---------------------------------------------------------------- utils */
static void die(const char *msg){ fprintf(stderr, "Błąd: %s\n", msg); exit(1); }
static void *xmalloc(size_t n){ void*p=malloc(n?n:1); if(!p) die("brak pamięci"); return p; }
static void *xrealloc(void*p,size_t n){ p=realloc(p,n?n:1); if(!p) die("brak pamięci"); return p; }

/* rosnący bufor bajtów */
typedef struct { char *b; size_t len, cap; } SB;
static void sb_init(SB*s){ s->b=xmalloc(64); s->len=0; s->cap=64; s->b[0]=0; }
static void sb_ensure(SB*s,size_t add){ if(s->len+add+1>s->cap){ while(s->len+add+1>s->cap) s->cap*=2; s->b=xrealloc(s->b,s->cap);} }
static void sb_putn(SB*s,const char*p,size_t n){ sb_ensure(s,n); memcpy(s->b+s->len,p,n); s->len+=n; s->b[s->len]=0; }
static void sb_puts(SB*s,const char*p){ sb_putn(s,p,strlen(p)); }
static void sb_putc(SB*s,char c){ sb_ensure(s,1); s->b[s->len++]=c; s->b[s->len]=0; }

/* ---------------------------------------------------------------- MD5 (RFC 1321) */
typedef struct { uint32_t a,b,c,d; uint64_t len; unsigned char buf[64]; size_t n; } MD5;
static uint32_t md5_rol(uint32_t x,int c){ return (x<<c)|(x>>(32-c)); }
static void md5_block(MD5*m,const unsigned char*p){
    static const uint32_t K[64]={
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const int S[64]={7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    uint32_t M[16];
    for(int i=0;i<16;i++) M[i]=(uint32_t)p[i*4]|((uint32_t)p[i*4+1]<<8)|((uint32_t)p[i*4+2]<<16)|((uint32_t)p[i*4+3]<<24);
    uint32_t A=m->a,B=m->b,C=m->c,D=m->d;
    for(int i=0;i<64;i++){
        uint32_t F; int g;
        if(i<16){ F=(B&C)|(~B&D); g=i; }
        else if(i<32){ F=(D&B)|(~D&C); g=(5*i+1)&15; }
        else if(i<48){ F=B^C^D; g=(3*i+5)&15; }
        else { F=C^(B|~D); g=(7*i)&15; }
        F=F+A+K[i]+M[g]; A=D; D=C; C=B; B=B+md5_rol(F,S[i]);
    }
    m->a+=A; m->b+=B; m->c+=C; m->d+=D;
}
static void md5_init(MD5*m){ m->a=0x67452301;m->b=0xefcdab89;m->c=0x98badcfe;m->d=0x10325476;m->len=0;m->n=0; }
static void md5_update(MD5*m,const unsigned char*p,size_t n){
    m->len+=n;
    while(n){ size_t k=64-m->n; if(k>n)k=n; memcpy(m->buf+m->n,p,k); m->n+=k; p+=k; n-=k;
        if(m->n==64){ md5_block(m,m->buf); m->n=0; } }
}
static void md5_final(MD5*m,unsigned char out[16]){
    uint64_t bits=m->len*8; unsigned char pad=0x80; md5_update(m,&pad,1);
    unsigned char z=0; while(m->n!=56) md5_update(m,&z,1);
    unsigned char lb[8]; for(int i=0;i<8;i++) lb[i]=(bits>>(8*i))&0xff; md5_update(m,lb,8);
    uint32_t v[4]={m->a,m->b,m->c,m->d};
    for(int i=0;i<4;i++){ out[i*4]=v[i]&0xff; out[i*4+1]=(v[i]>>8)&0xff; out[i*4+2]=(v[i]>>16)&0xff; out[i*4+3]=(v[i]>>24)&0xff; }
}
static void hexlower(const unsigned char*in,int n,char*out){ static const char*h="0123456789abcdef"; for(int i=0;i<n;i++){ out[i*2]=h[in[i]>>4]; out[i*2+1]=h[in[i]&15]; } out[n*2]=0; }

/* ---------------------------------------------------------------- hasze plików */
static long file_size(const char*path){ struct stat st; if(stat(path,&st)!=0) return -1; return (long)st.st_size; }

/* URL http(s) jako wejście — pobieramy tylko potrzebne fragmenty (Range).
 * Definicje url_* są niżej (po https_fetch); tutaj deklaracje wyprzedzające. */
static int is_url(const char*s){ return s && (!strncmp(s,"http://",7)||!strncmp(s,"https://",8)); }
static unsigned char* url_read_range(const char*url,long start,long length,size_t*outlen);
static unsigned char* url_read_full(const char*url,size_t*outlen);
static long url_size(const char*url);
static void input_basename(const char*path,char*out,size_t osz);  /* def. niżej */
/* Rozmiar wejścia (URL lub plik lokalny). */
static long input_size(const char*path){ return is_url(path)? url_size(path) : file_size(path); }

static int oshash(const char*path,char out[17]){
    if(is_url(path)){
        long size=url_size(path); if(size<2*OSH_CHUNK) return -1;
        size_t hn=0,tn=0; unsigned char*head=url_read_range(path,0,OSH_CHUNK,&hn);
        if(!head||hn<OSH_CHUNK){ free(head); return -2; }
        unsigned char*tail=url_read_range(path,size-OSH_CHUNK,OSH_CHUNK,&tn);
        if(!tail||tn<OSH_CHUNK){ free(head); free(tail); return -2; }
        uint64_t h=(uint64_t)size, w;
        for(int i=0;i<OSH_CHUNK/8;i++){ memcpy(&w,head+i*8,8); h+=w; }
        for(int i=0;i<OSH_CHUNK/8;i++){ memcpy(&w,tail+i*8,8); h+=w; }
        free(head); free(tail);
        snprintf(out,17,"%016llx",(unsigned long long)h); return 0;
    }
    long size=file_size(path);
    if(size<2*OSH_CHUNK) return -1;
    FILE*f=fopen(path,"rb"); if(!f) return -2;
    uint64_t h=(uint64_t)size, w;
    for(int i=0;i<OSH_CHUNK/8;i++){ if(fread(&w,8,1,f)!=1){fclose(f);return -2;} h+=w; }
    fseek(f,size-OSH_CHUNK,SEEK_SET);
    for(int i=0;i<OSH_CHUNK/8;i++){ if(fread(&w,8,1,f)!=1){fclose(f);return -2;} h+=w; }
    fclose(f);
    snprintf(out,17,"%016llx",(unsigned long long)h);
    return 0;
}
static int md5_10mb(const char*path,char out[33]){
    MD5 m; md5_init(&m);
    if(is_url(path)){
        size_t n=0; unsigned char*buf=url_read_range(path,0,CHUNK_10MB,&n);
        if(!buf) return -1;
        md5_update(&m,buf,n<CHUNK_10MB?n:(size_t)CHUNK_10MB); free(buf);
        unsigned char d[16]; md5_final(&m,d); hexlower(d,16,out); return 0;
    }
    FILE*f=fopen(path,"rb"); if(!f) return -1;
    unsigned char*buf=xmalloc(1<<20); size_t total=0,r;
    while(total<CHUNK_10MB && (r=fread(buf,1,(size_t)(CHUNK_10MB-total<(1<<20)?CHUNK_10MB-total:(1<<20)),f))>0){ md5_update(&m,buf,r); total+=r; }
    free(buf); fclose(f);
    unsigned char d[16]; md5_final(&m,d); hexlower(d,16,out); return 0;
}
static void md5_bytes(const unsigned char*p,size_t n,char out[33]){ MD5 m; md5_init(&m); md5_update(&m,p,n); unsigned char d[16]; md5_final(&m,d); hexlower(d,16,out); }
/* Hash napisów Napisy24 (pole hs): rozmiar + suma WSZYSTKICH pełnych słów 8B LE; 16 hex WIELKIE. */
static void subtitle_hash(const unsigned char*data,size_t n,char out[17]){
    uint64_t h=(uint64_t)n; size_t m=(n/8)*8;
    for(size_t i=0;i<m;i+=8){ uint64_t w; memcpy(&w,data+i,8); h+=w; }
    snprintf(out,17,"%016llX",(unsigned long long)h);
}

/* ---------------------------------------------------------------- SHA-256 */
typedef struct { uint32_t s[8]; uint64_t len; unsigned char buf[64]; size_t n; } SHA256;
static uint32_t s_ror(uint32_t x,int c){ return (x>>c)|(x<<(32-c)); }
static void sha256_block(SHA256*h,const unsigned char*p){
    static const uint32_t K[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64]; for(int i=0;i<16;i++) w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for(int i=16;i<64;i++){ uint32_t s0=s_ror(w[i-15],7)^s_ror(w[i-15],18)^(w[i-15]>>3); uint32_t s1=s_ror(w[i-2],17)^s_ror(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
    uint32_t a=h->s[0],b=h->s[1],c=h->s[2],d=h->s[3],e=h->s[4],f=h->s[5],g=h->s[6],hh=h->s[7];
    for(int i=0;i<64;i++){ uint32_t S1=s_ror(e,6)^s_ror(e,11)^s_ror(e,25); uint32_t ch=(e&f)^(~e&g); uint32_t t1=hh+S1+ch+K[i]+w[i];
        uint32_t S0=s_ror(a,2)^s_ror(a,13)^s_ror(a,22); uint32_t mj=(a&b)^(a&c)^(b&c); uint32_t t2=S0+mj;
        hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
    h->s[0]+=a;h->s[1]+=b;h->s[2]+=c;h->s[3]+=d;h->s[4]+=e;h->s[5]+=f;h->s[6]+=g;h->s[7]+=hh;
}
static void sha256_init(SHA256*h){ uint32_t iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; memcpy(h->s,iv,32); h->len=0; h->n=0; }
static void sha256_update(SHA256*h,const unsigned char*p,size_t n){ h->len+=n; while(n){ size_t k=64-h->n; if(k>n)k=n; memcpy(h->buf+h->n,p,k); h->n+=k; p+=k; n-=k; if(h->n==64){ sha256_block(h,h->buf); h->n=0; } } }
static void sha256_final(SHA256*h,unsigned char out[32]){ uint64_t bits=h->len*8; unsigned char c=0x80; sha256_update(h,&c,1); unsigned char z=0; while(h->n!=56) sha256_update(h,&z,1);
    unsigned char lb[8]; for(int i=0;i<8;i++) lb[i]=(bits>>(8*(7-i)))&0xff; sha256_update(h,lb,8);
    for(int i=0;i<8;i++){ out[i*4]=h->s[i]>>24; out[i*4+1]=h->s[i]>>16; out[i*4+2]=h->s[i]>>8; out[i*4+3]=h->s[i]; } }

/* ---------------------------------------------------------------- AES-256 (szyfr bloku) */
static const unsigned char AES_SBOX[256]={
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
static const unsigned char AES_RCON[14]={0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1B,0x36,0x6C,0xD8,0xAB,0x4D};
static unsigned char aes_xtime(unsigned char a){ int r=a<<1; if(r&0x100) r^=0x11B; return r&0xFF; }
static unsigned char aes_mul(unsigned char a,unsigned char b){ unsigned char r=0; for(int i=0;i<8;i++){ if(b&1) r^=a; b>>=1; a=aes_xtime(a); } return r; }
typedef struct { unsigned char rk[15][16]; } AES;
static void aes_init(AES*x,const unsigned char*key){ int nk=8,nr=14; unsigned char w[60][4];
    for(int i=0;i<nk;i++) for(int j=0;j<4;j++) w[i][j]=key[i*4+j];
    for(int i=nk;i<4*(nr+1);i++){ unsigned char t[4]; for(int j=0;j<4;j++) t[j]=w[i-1][j];
        if(i%nk==0){ unsigned char tmp=t[0]; t[0]=AES_SBOX[t[1]]^AES_RCON[i/nk-1]; t[1]=AES_SBOX[t[2]]; t[2]=AES_SBOX[t[3]]; t[3]=AES_SBOX[tmp]; }
        else if(i%nk==4){ for(int j=0;j<4;j++) t[j]=AES_SBOX[t[j]]; }
        for(int j=0;j<4;j++) w[i][j]=w[i-nk][j]^t[j]; }
    for(int r=0;r<=nr;r++) for(int c=0;c<4;c++) for(int j=0;j<4;j++) x->rk[r][c*4+j]=w[r*4+c][j];
}
static void aes_encrypt_block(AES*x,unsigned char*st){ int nr=14;
    for(int i=0;i<16;i++) st[i]^=x->rk[0][i];
    for(int r=1;r<nr;r++){ for(int i=0;i<16;i++) st[i]=AES_SBOX[st[i]];
        unsigned char t[16]; for(int c=0;c<4;c++) for(int rr=0;rr<4;rr++) t[c*4+rr]=st[((c+rr)%4)*4+rr]; memcpy(st,t,16);
        for(int c=0;c<4;c++){ unsigned char a0=st[c*4],a1=st[c*4+1],a2=st[c*4+2],a3=st[c*4+3];
            st[c*4]=aes_mul(a0,2)^aes_mul(a1,3)^a2^a3; st[c*4+1]=a0^aes_mul(a1,2)^aes_mul(a2,3)^a3;
            st[c*4+2]=a0^a1^aes_mul(a2,2)^aes_mul(a3,3); st[c*4+3]=aes_mul(a0,3)^a1^a2^aes_mul(a3,2); }
        for(int i=0;i<16;i++) st[i]^=x->rk[r][i]; }
    for(int i=0;i<16;i++) st[i]=AES_SBOX[st[i]];
    unsigned char t[16]; for(int c=0;c<4;c++) for(int rr=0;rr<4;rr++) t[c*4+rr]=st[((c+rr)%4)*4+rr]; memcpy(st,t,16);
    for(int i=0;i<16;i++) st[i]^=x->rk[nr][i];
}

/* ---------------------------------------------------------------- 7z-AES (zapis) */
static void rand_bytes(unsigned char*b,size_t n){ FILE*f=fopen("/dev/urandom","rb"); if(f){ if(fread(b,1,n,f)!=n){} fclose(f); return; } for(size_t i=0;i<n;i++) b[i]=(unsigned char)(i*7+1); }
static void sevenzip_key(const char*pw,unsigned char out[32]){ size_t pl=strlen(pw); unsigned char*p16=xmalloc(pl*2?pl*2:1);
    for(size_t i=0;i<pl;i++){ p16[i*2]=pw[i]; p16[i*2+1]=0; }
    SHA256 h; sha256_init(&h); uint64_t counter=0;
    for(uint64_t i=0;i<(1ULL<<19);i++){ sha256_update(&h,p16,pl*2); unsigned char cb[8]; for(int k=0;k<8;k++) cb[k]=(counter>>(8*k))&0xff; sha256_update(&h,cb,8); counter++; }
    sha256_final(&h,out); free(p16); }
static unsigned char* aes_cbc(const unsigned char*key,const unsigned char*iv,const unsigned char*data,size_t n,size_t*outlen){
    size_t pad=(n%16)?(16-n%16):0, total=n+pad; unsigned char*buf=xmalloc(total?total:1); memcpy(buf,data,n); memset(buf+n,0,pad);
    AES x; aes_init(&x,key); unsigned char prev[16]; memcpy(prev,iv,16); unsigned char*out=xmalloc(total?total:1);
    for(size_t off=0;off<total;off+=16){ unsigned char blk[16]; for(int i=0;i<16;i++) blk[i]=buf[off+i]^prev[i]; aes_encrypt_block(&x,blk); memcpy(out+off,blk,16); memcpy(prev,blk,16); }
    free(buf); *outlen=total; return out; }
static void z7num(SB*b,uint64_t value){ int first=0,mask=0x80,ii=8; for(int k=0;k<8;k++){ if(value<(1ULL<<(7*(k+1)))){ first|=(int)((value>>(8*k))&0xFF); ii=k; break; } first|=mask; mask>>=1; }
    sb_putc(b,(char)(first&0xFF)); uint64_t v=value; for(int k=0;k<ii;k++){ sb_putc(b,(char)(v&0xFF)); v>>=8; } }
static unsigned char* write_7z_aes(const char*entry,const unsigned char*data,size_t dlen,size_t*outlen){
    unsigned char iv[16]; rand_bytes(iv,16); unsigned char key[32]; sevenzip_key("iBlm8NTigvru0Jr0",key);
    size_t enclen; unsigned char*enc=aes_cbc(key,iv,data,dlen,&enclen);
    uint32_t crc=crc32(0,data,dlen);
    unsigned char props[18]; props[0]=0x53; props[1]=0x0F; memcpy(props+2,iv,16);
    SB pack; sb_init(&pack); sb_putc(&pack,0x06); z7num(&pack,0); z7num(&pack,1); sb_putc(&pack,0x09); z7num(&pack,enclen); sb_putc(&pack,0x00);
    SB folder; sb_init(&folder); z7num(&folder,1); sb_putc(&folder,0x24); sb_putn(&folder,"\x06\xf1\x07\x01",4); z7num(&folder,18); sb_putn(&folder,(char*)props,18);
    SB unp; sb_init(&unp); sb_putc(&unp,0x07); sb_putc(&unp,0x0B); z7num(&unp,1); sb_putc(&unp,0x00); sb_putn(&unp,folder.b,folder.len);
        sb_putc(&unp,0x0C); z7num(&unp,dlen); sb_putc(&unp,0x0A); sb_putc(&unp,0x01);
        unsigned char cb[4]={(unsigned char)(crc&0xff),(unsigned char)((crc>>8)&0xff),(unsigned char)((crc>>16)&0xff),(unsigned char)((crc>>24)&0xff)}; sb_putn(&unp,(char*)cb,4); sb_putc(&unp,0x00);
    SB si; sb_init(&si); sb_putc(&si,0x04); sb_putn(&si,pack.b,pack.len); sb_putn(&si,unp.b,unp.len); sb_putc(&si,0x00);
    size_t nl=strlen(entry); SB name; sb_init(&name); for(size_t i=0;i<nl;i++){ sb_putc(&name,entry[i]); sb_putc(&name,0); } sb_putc(&name,0); sb_putc(&name,0);
    SB fi; sb_init(&fi); sb_putc(&fi,0x05); z7num(&fi,1); sb_putc(&fi,0x11); z7num(&fi,name.len+1); sb_putc(&fi,0x00); sb_putn(&fi,name.b,name.len); sb_putc(&fi,0x00);
    SB hdr; sb_init(&hdr); sb_putc(&hdr,0x01); sb_putn(&hdr,si.b,si.len); sb_putn(&hdr,fi.b,fi.len); sb_putc(&hdr,0x00);
    uint32_t hcrc=crc32(0,(unsigned char*)hdr.b,hdr.len);
    unsigned char sh[20]; for(int i=0;i<8;i++) sh[i]=(enclen>>(8*i))&0xff; for(int i=0;i<8;i++) sh[8+i]=(hdr.len>>(8*i))&0xff; for(int i=0;i<4;i++) sh[16+i]=(hcrc>>(8*i))&0xff;
    uint32_t shcrc=crc32(0,sh,20);
    SB arc; sb_init(&arc); sb_putn(&arc,"\x37\x7a\xbc\xaf\x27\x1c\x00\x04",8);
    unsigned char c4[4]={(unsigned char)(shcrc&0xff),(unsigned char)((shcrc>>8)&0xff),(unsigned char)((shcrc>>16)&0xff),(unsigned char)((shcrc>>24)&0xff)};
    sb_putn(&arc,(char*)c4,4); sb_putn(&arc,(char*)sh,20); sb_putn(&arc,(char*)enc,enclen); sb_putn(&arc,hdr.b,hdr.len);
    *outlen=arc.len; unsigned char*o=xmalloc(arc.len); memcpy(o,arc.b,arc.len);
    free(enc);free(pack.b);free(folder.b);free(unp.b);free(si.b);free(name.b);free(fi.b);free(hdr.b);free(arc.b); return o; }

/* ---------------------------------------------------------------- FPS z pliku */
static uint64_t rd_be(const unsigned char*p,int n){ uint64_t v=0; for(int i=0;i<n;i++) v=(v<<8)|p[i]; return v; }

/* EBML vint: zwróć wartość; jeśli want_id!=0 zachowaj marker (ID), inaczej wyczyść (rozmiar). */
static int ebml_read(FILE*f,uint64_t*out,int want_id){
    int c=fgetc(f); if(c<0) return -1;
    unsigned char first=(unsigned char)c; unsigned char mask=0x80; int len=1;
    while(len<=8 && !(first&mask)){ mask>>=1; len++; }
    if(len>8) return -1;
    uint64_t v=first;
    for(int i=1;i<len;i++){ int b=fgetc(f); if(b<0) return -1; v=(v<<8)|(unsigned char)b; }
    if(!want_id){ /* wyczyść bit-marker */ uint64_t clear=(uint64_t)1<<(7*len); v&=(clear-1); }
    *out=v; return len;
}
/* (fps, czas_s) z MKV: DefaultDuration ścieżki wideo + Info>Duration×TimecodeScale */
static void mkv_media(FILE*f,double*fps,double*dur){
    *fps=0; *dur=0; fseek(f,0,SEEK_SET);
    long track=0; uint64_t tc_scale=1000000; double dur_ticks=0;
    for(long guard=0;guard<2000000;guard++){
        uint64_t id,len;
        if(ebml_read(f,&id,1)<0) break;
        if(ebml_read(f,&len,0)<0) break;
        if(id==0x83){ int b=fgetc(f); track=(b<0)?0:b; }
        else if(id==0x23E383 && track==1 && *fps==0){
            unsigned char raw[4]; if(fread(raw,1,4,f)==4){ uint64_t ns=rd_be(raw,4); if(ns) *fps=1000000000.0/(double)ns; } }
        else if(id==0x2AD7B1){ /* TimecodeScale (uint) */
            if(len>=1 && len<=8){ unsigned char raw[8]; if(fread(raw,1,(size_t)len,f)==(size_t)len) tc_scale=rd_be(raw,(int)len); }
            else fseek(f,(long)len,SEEK_CUR); }
        else if(id==0x4489){ /* Duration (float BE 4/8 B) */
            if(len==4){ unsigned char raw[4]; if(fread(raw,1,4,f)==4){ uint32_t bits=(uint32_t)rd_be(raw,4); float ff; memcpy(&ff,&bits,4); dur_ticks=(double)ff; } }
            else if(len==8){ unsigned char raw[8]; if(fread(raw,1,8,f)==8){ uint64_t bits=rd_be(raw,8); double dd; memcpy(&dd,&bits,8); dur_ticks=dd; } }
            else fseek(f,(long)len,SEEK_CUR); }
        else if(id!=0x18538067 && id!=0x1654AE6B && id!=0xAE && id!=0x1549A966 && id!=0x83){
            fseek(f,(long)len,SEEK_CUR); }
        if(*fps!=0 && dur_ticks!=0) break;
    }
    if(dur_ticks>0) *dur=dur_ticks*(double)tc_scale/1e9;
}
/* (fps, czas_s) z AVI: dwMicroSecPerFrame (off 32) + dwTotalFrames (off 48) */
static void avi_media(FILE*f,double*fps,double*dur){ *fps=0; *dur=0;
    unsigned char b[4]; fseek(f,32,SEEK_SET); if(fread(b,1,4,f)!=4) return;
    uint32_t us=(uint32_t)b[0]|(b[1]<<8)|(b[2]<<16)|((uint32_t)b[3]<<24);
    if(us) *fps=1000000.0/(double)us;
    unsigned char b2[4]; fseek(f,48,SEEK_SET);
    if(fread(b2,1,4,f)==4 && us){ uint32_t fr=(uint32_t)b2[0]|(b2[1]<<8)|(b2[2]<<16)|((uint32_t)b2[3]<<24);
        if(fr) *dur=(double)fr*(double)us/1000000.0; } }

/* MP4/MOV ISO BMFF: znajdź trak wideo, policz fps z mdhd.timescale + stts */
static long box_next(FILE*f,long pos,long end,char type[5],long*payload){
    if(pos+8>end) return -1; fseek(f,pos,SEEK_SET);
    unsigned char h[8]; if(fread(h,1,8,f)!=8) return -1;
    uint64_t size=rd_be(h,4); memcpy(type,h+4,4); type[4]=0; long pl=pos+8;
    if(size==1){ unsigned char e[8]; if(fread(e,1,8,f)!=8) return -1; size=rd_be(e,8); pl=pos+16; }
    else if(size==0){ size=(uint64_t)(end-pos); }
    if(size<8) return -1; *payload=pl; return pos+(long)size;
}
static int bmff_find(FILE*f,long start,long end,const char*want,long*p_out,long*e_out){
    long pos=start; char t[5]; long pl;
    while((pos>=0)&&(pos<end)){ long nx=box_next(f,pos,end,t,&pl); if(nx<0) return 0;
        if(strcmp(t,want)==0){ *p_out=pl; *e_out=nx; return 1; }
        if(!strcmp(t,"moov")||!strcmp(t,"trak")||!strcmp(t,"mdia")||!strcmp(t,"minf")||!strcmp(t,"stbl")){
            if(bmff_find(f,pl,nx,want,p_out,e_out)) return 1; }
        pos=nx; }
    return 0;
}
/* (fps, czas_s) z MP4/MOV: ścieżka wideo, mdhd.timescale + stts */
static void mp4_media(FILE*f,long flen,double*fps,double*dur){ *fps=0; *dur=0;
    long pos=0; char t[5]; long pl;
    while(pos>=0 && pos<flen){ long nx=box_next(f,pos,flen,t,&pl); if(nx<0) break;
        if(!strcmp(t,"moov")){
            long tp,te,mp,me,hp,he,dhp,dhe,sp,se; long tpos=pl;
            char tt[5]; long tpl;
            while(tpos>=0 && tpos<nx){ long tnx=box_next(f,tpos,nx,tt,&tpl); if(tnx<0) break;
                if(!strcmp(tt,"trak")){ tp=tpl; te=tnx;
                    if(bmff_find(f,tp,te,"mdia",&mp,&me)){
                        if(bmff_find(f,mp,me,"hdlr",&hp,&he)){
                            fseek(f,hp+8,SEEK_SET); unsigned char hd[4];
                            if(fread(hd,1,4,f)==4 && memcmp(hd,"vide",4)==0){
                                if(bmff_find(f,mp,me,"mdhd",&dhp,&dhe)){
                                    fseek(f,dhp,SEEK_SET); int ver=fgetc(f); fgetc(f);fgetc(f);fgetc(f);
                                    unsigned char ts[4]; uint32_t timescale=0;
                                    if(ver==1){ unsigned char skip[16]; if(fread(skip,1,16,f)!=16) {} if(fread(ts,1,4,f)==4) timescale=(uint32_t)rd_be(ts,4);}
                                    else { unsigned char skip[8]; if(fread(skip,1,8,f)!=8){} if(fread(ts,1,4,f)==4) timescale=(uint32_t)rd_be(ts,4);}
                                    if(bmff_find(f,mp,me,"stts",&sp,&se)){
                                        fseek(f,sp,SEEK_SET); unsigned char vf[4]; if(fread(vf,1,4,f)!=4){}
                                        unsigned char cnt[4]; if(fread(cnt,1,4,f)!=4){}
                                        uint32_t nent=(uint32_t)rd_be(cnt,4);
                                        uint64_t tot_s=0,tot_d=0;
                                        for(uint32_t i=0;i<nent;i++){ unsigned char e8[8]; if(fread(e8,1,8,f)!=8) break;
                                            uint64_t c=rd_be(e8,4), d=rd_be(e8+4,4); tot_s+=c; tot_d+=c*d; }
                                        if(timescale && tot_d){ *fps=(double)tot_s*(double)timescale/(double)tot_d; *dur=(double)tot_d/(double)timescale; return; }
                                    }
                                }
                            }
                        }
                    }
                }
                tpos=tnx; }
        }
        pos=nx; }
}
/* Rozpoznaj kontener i odczytaj (fps, czas) z otwartego strumienia (flen = długość danych). */
static void media_from_stream(FILE*f,long flen,double*fps,double*dur){
    *fps=0; *dur=0; unsigned char m[8]; size_t r=fread(m,1,8,f); rewind(f);
    if(r>=4 && m[0]==0x1a&&m[1]==0x45&&m[2]==0xdf&&m[3]==0xa3) mkv_media(f,fps,dur);
    else if(r>=4 && !memcmp(m,"RIFF",4)) avi_media(f,fps,dur);
    else if(r>=8 && !memcmp(m+4,"ftyp",4)) mp4_media(f,flen,fps,dur);
}
/* Ile bajtów prefiksu pobrać z URL-a do odczytu FPS/czasu (jak w Pythonie: 8 MiB). */
#define FPS_URL_PREFIX (8*1024*1024)
/* (fps, czas_s) z pliku MKV/AVI/MP4/MOV; URL: prefiks przez Range. */
static void media_from_file(const char*path,double*fps,double*dur){
    *fps=0; *dur=0;
    if(is_url(path)){
        long size=url_size(path); if(size<=0) return;
        long want = size<FPS_URL_PREFIX ? size : FPS_URL_PREFIX;
        size_t n=0; unsigned char*buf=url_read_range(path,0,want,&n); if(!buf||!n){ free(buf); return; }
        FILE*f=fmemopen(buf,n,"rb"); if(f){ media_from_stream(f,(long)n,fps,dur); fclose(f); }
        free(buf); return;
    }
    FILE*f=fopen(path,"rb"); if(!f) return;
    fseek(f,0,SEEK_END); long fl=ftell(f); fseek(f,0,SEEK_SET);
    media_from_stream(f,fl,fps,dur); fclose(f);
}
static double fps_from_file(const char*path){ double fps,dur; media_from_file(path,&fps,&dur); return fps; }
static double duration_from_file(const char*path){ double fps,dur; media_from_file(path,&fps,&dur); return dur; }
/* sformatuj sekundy jako HH:MM:SS (pusty string dla <=0) */
static void hhmmss(double seconds,char*out,size_t osz){ if(seconds<=0){ if(osz)out[0]=0; return; }
    long s=(long)seconds; snprintf(out,osz,"%02ld:%02ld:%02ld",s/3600,(s%3600)/60,s%60); }
static double trusted_fps(double v){ return (v>22.0 && v<32.0)?v:0.0; }

/* ---------------------------------------------------------------- base64 dekoder */
static int b64val(int c){ if(c>='A'&&c<='Z')return c-'A'; if(c>='a'&&c<='z')return c-'a'+26;
    if(c>='0'&&c<='9')return c-'0'+52; if(c=='+')return 62; if(c=='/')return 63; return -1; }
static unsigned char* b64decode(const char*in,size_t inlen,size_t*outlen){
    unsigned char*out=xmalloc(inlen/4*3+4); size_t o=0; int buf=0,bits=0;
    for(size_t i=0;i<inlen;i++){ int v=b64val((unsigned char)in[i]); if(v<0) continue;
        buf=(buf<<6)|v; bits+=6; if(bits>=8){ bits-=8; out[o++]=(buf>>bits)&0xff; } }
    *outlen=o; return out;
}

/* ---------------------------------------------------------------- model napisów */
typedef struct { long start,end; char**lines; int nlines; } Cue;
typedef struct { Cue*a; int n,cap; } Cues;
static void cues_init(Cues*c){ c->a=NULL; c->n=0; c->cap=0; }
static Cue* cues_push(Cues*c){ if(c->n==c->cap){ c->cap=c->cap?c->cap*2:16; c->a=xrealloc(c->a,c->cap*sizeof(Cue)); }
    Cue*q=&c->a[c->n++]; q->start=0;q->end=0;q->lines=NULL;q->nlines=0; return q; }
static void cue_addline(Cue*q,const char*s,size_t n){ q->lines=xrealloc(q->lines,(q->nlines+1)*sizeof(char*)); char*d=xmalloc(n+1); memcpy(d,s,n); d[n]=0; q->lines[q->nlines++]=d; }

static char* rstrip_dup(const char*s){ /* kopia bez końcowych \r itd. (dla linii) */ size_t n=strlen(s); char*d=xmalloc(n+1); memcpy(d,s,n+1); return d; }

/* podziel na linie po '\n' (usuwając '\r'); zwraca tablicę wskaźników do kopii */
typedef struct { char**a; int n; } Lines;
static Lines split_lines(const char*text){
    Lines L; L.a=NULL; L.n=0; const char*p=text;
    while(1){ const char*nl=strchr(p,'\n'); size_t len= nl? (size_t)(nl-p):strlen(p);
        size_t l2=len; if(l2>0 && p[l2-1]=='\r') l2--;
        L.a=xrealloc(L.a,(L.n+1)*sizeof(char*)); char*d=xmalloc(l2+1); memcpy(d,p,l2); d[l2]=0; L.a[L.n++]=d;
        if(!nl) break; p=nl+1; }
    return L;
}
static void lines_free(Lines*L){ for(int i=0;i<L->n;i++) free(L->a[i]); free(L->a); }

static int is_ascii_ws(char c){ return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'||c=='\v'; }
static void strip_inplace(char*s){ size_t n=strlen(s),i=0; while(n>0&&is_ascii_ws(s[n-1])) s[--n]=0;
    while(s[i]&&is_ascii_ws(s[i])) i++; if(i) memmove(s,s+i,n-i+1); }

/* Dekoduj bajty jako UTF-8 z errors="replace" (jak CPython: substytucja
 * maksymalnych podciągów niepoprawnych sekwencji przez U+FFFD). Wejście/wyjście
 * NUL-zakończone; zwraca nowy bufor (malloc). */
static char* utf8_replace(const char*in){
    const unsigned char*p=(const unsigned char*)in; size_t n=strlen(in);
    SB o; sb_init(&o); size_t i=0;
    while(i<n){ unsigned char b=p[i];
        if(b<0x80){ sb_putc(&o,(char)b); i++; continue; }
        int need=0; unsigned lo1=0x80,hi1=0xBF;
        if(b>=0xC2&&b<=0xDF){ need=1; }
        else if(b==0xE0){ need=2; lo1=0xA0; }
        else if(b>=0xE1&&b<=0xEC){ need=2; }
        else if(b==0xED){ need=2; hi1=0x9F; }
        else if(b>=0xEE&&b<=0xEF){ need=2; }
        else if(b==0xF0){ need=3; lo1=0x90; }
        else if(b>=0xF1&&b<=0xF3){ need=3; }
        else if(b==0xF4){ need=3; hi1=0x8F; }
        else { sb_puts(&o,"\xEF\xBF\xBD"); i++; continue; }  /* zły bajt wiodący */
        size_t consumed=1; int ok=1;
        for(int k=0;k<need;k++){ unsigned lo=(k==0)?lo1:0x80, hi=(k==0)?hi1:0xBF;
            if(i+consumed<n && p[i+consumed]>=lo && p[i+consumed]<=hi) consumed++;
            else { ok=0; break; } }
        if(ok){ for(size_t k=0;k<consumed;k++) sb_putc(&o,(char)p[i+k]); i+=consumed; }
        else { sb_puts(&o,"\xEF\xBF\xBD"); i+=consumed; }  /* consumed>=1 */
    }
    sb_putc(&o,0); return o.b;
}

/* usuń tagi HTML (</?[A-Za-z][^>]*>) i klamry {...} — jak strip_format_tags */
static void strip_format_tags(char*s){
    char*o=s,*p=s;
    while(*p){
        if(*p=='<'){ const char*q=p+1; if(*q=='/') q++; if(isalpha((unsigned char)*q)){ const char*e=strchr(p,'>'); if(e){ p=e+1; continue; } } }
        if(*p=='{'){ const char*e=strchr(p,'}'); if(e){ p=e+1; continue; } }
        *o++=*p++;
    }
    *o=0;
}

/* usuń SDH/HI: [odgłosy], (opisy), etykieta MÓWCA:, nuty ♪♫# — jak strip_sdh_line */
static void strip_sdh_line(char*s){
    { char*o=s,*p=s; while(*p){ if(*p=='['){ char*e=strchr(p,']'); if(e){ p=e+1; continue; } }
        if(*p=='('){ char*e=strchr(p,')'); if(e){ p=e+1; continue; } } *o++=*p++; } *o=0; }
    { char*p=s; while(*p==' '||*p=='-')p++; char*st=p;
        if(isupper((unsigned char)*p)){ char*q=p+1; int len=1;
            while(*q&&len<=21&&(isupper((unsigned char)*q)||isdigit((unsigned char)*q)||*q==' '||*q=='.'||*q=='\''||*q=='-')){ q++; len++; }
            if(*q==':' && (q-st)>=2){ char*after=q+1; while(*after==' ')after++; memmove(s,after,strlen(after)+1); } } }
    { char*o=s,*p=s; while(*p){ if(*p=='#'){ p++; continue; }
        if((unsigned char)p[0]==0xE2&&(unsigned char)p[1]==0x99&&((unsigned char)p[2]==0xAA||(unsigned char)p[2]==0xAB)){ p+=3; continue; }
        *o++=*p++; } *o=0; }
    strip_inplace(s);
}

/* ---------------------------------------------------------------- czas -> tekst */
static void ms_to_srt(long ms,char out[16]){ if(ms<0) ms=0; long h=ms/3600000; ms-=h*3600000; long m=ms/60000; ms-=m*60000; long s=ms/1000; ms-=s*1000; snprintf(out,16,"%02ld:%02ld:%02ld,%03ld",h,m,s,ms); }

/* ---------------------------------------------------------------- parsery */
static int str_isdigit(const char*s){ if(!*s) return 0; for(;*s;s++) if(!isdigit((unsigned char)*s)) return 0; return 1; }
/* parse "H:MM:SS[,.]mmm" w dowolnym miejscu; zwraca ms lub -1 */
static long parse_srt_time(const char*s,const char**endp){
    while(*s && !(isdigit((unsigned char)*s))) s++;
    long h,m,sec,ms; int nn=0; char sep;
    if(sscanf(s,"%ld:%2ld:%2ld%c%3ld%n",&h,&m,&sec,&sep,&ms,&nn)>=5 && (sep==','||sep=='.')){
        if(endp)*endp=s+nn; return ((h*3600+m*60+sec)*1000)+ms; }
    return -1;
}

static void parse_srt(const char*text,Cues*out){
    /* podziel na bloki po pustej linii */
    Lines L=split_lines(text);
    int i=0;
    while(i<L.n){
        while(i<L.n && L.a[i][0]==0) i++;   /* pomiń puste separatory */
        int start=i; while(i<L.n && L.a[i][0]!=0) i++;   /* blok [start,i) */
        int blk=i-start; if(blk<=0) continue;
        int idx=start; char first[64]; snprintf(first,sizeof first,"%s",L.a[start]); strip_inplace(first);
        if(str_isdigit(first)) idx++;
        if(idx>=start+blk) continue;
        const char*arrow=strstr(L.a[idx],"-->"); if(!arrow) continue;
        const char*e1; long s_ms=parse_srt_time(L.a[idx],&e1); if(s_ms<0) continue;
        long e_ms=parse_srt_time(arrow, NULL); if(e_ms<0) continue;
        Cue*q=cues_push(out); q->start=s_ms; q->end=e_ms;
        for(int k=idx+1;k<start+blk;k++){ char*ln=rstrip_dup(L.a[k]); char*t=xmalloc(strlen(ln)+1); strcpy(t,ln); strip_inplace(t);
            if(t[0]!=0) cue_addline(q,ln,strlen(ln)); free(ln); free(t); }
    }
    lines_free(&L);
}

static void parse_microdvd(const char*text,double fps,Cues*out){
    Lines L=split_lines(text);
    for(int i=0;i<L.n;i++){ char*s=L.a[i]; char*t=xmalloc(strlen(s)+1); strcpy(t,s); strip_inplace(t);
        long sf,ef; int nn=0;
        if(sscanf(t,"{%ld}{%ld}%n",&sf,&ef,&nn)==2){
            const char*body=t+nn; long sm=(long)((double)sf*1000.0/fps), em=(long)((double)ef*1000.0/fps);
            Cue*q=cues_push(out); q->start=sm; q->end=em;
            /* split body na '|' */
            const char*p=body; while(1){ const char*bar=strchr(p,'|'); size_t len=bar?(size_t)(bar-p):strlen(p);
                char*seg=xmalloc(len+1); memcpy(seg,p,len); seg[len]=0;
                /* MicroDVD: usuń TYLKO klamry {..} (jak Python), NIE tagi <..> */
                { char*o=seg,*r=seg; while(*r){ if(*r=='{'){ char*ee=strchr(r,'}'); if(ee){ r=ee+1; continue; } } *o++=*r++; } *o=0; }
                cue_addline(q,seg,strlen(seg)); free(seg);
                if(!bar) break; p=bar+1; }
        }
        free(t);
    }
    lines_free(&L);
}

/* VTT: minimalne dekodowanie encji + usunięcie tagów <..> */
static void html_unescape(char*s){
    struct{const char*e;const char*r;} tab[]={{"&amp;","&"},{"&lt;","<"},{"&gt;",">"},{"&quot;","\""},{"&apos;","'"},{"&nbsp;"," "},{NULL,NULL}};
    char*o=s,*p=s; while(*p){
        if(p[0]=='&'&&p[1]=='#'){ /* encja numeryczna &#NNN; lub &#xHH; */
            const char*q=p+2; long cp=0; int hex=0; if(*q=='x'||*q=='X'){ hex=1; q++; }
            const char*st=q; while(*q && *q!=';'){ int d; if(*q>='0'&&*q<='9')d=*q-'0'; else if(hex&&*q>='a'&&*q<='f')d=*q-'a'+10; else if(hex&&*q>='A'&&*q<='F')d=*q-'A'+10; else { q=st; break; } cp=cp*(hex?16:10)+d; q++; }
            if(q>st&&*q==';'){ if(cp<0x80) *o++=(char)cp; else if(cp<0x800){ *o++=(char)(0xC0|(cp>>6)); *o++=(char)(0x80|(cp&0x3F)); } else { *o++=(char)(0xE0|(cp>>12)); *o++=(char)(0x80|((cp>>6)&0x3F)); *o++=(char)(0x80|(cp&0x3F)); } p=q+1; continue; } }
        if(*p=='&'){ int done=0; for(int i=0;tab[i].e;i++){ size_t el=strlen(tab[i].e); if(!strncmp(p,tab[i].e,el)){ for(const char*r=tab[i].r;*r;) *o++=*r++; p+=el; done=1; break; } } if(done) continue; }
        *o++=*p++; } *o=0;
}
static void vtt_clean(char*s){ /* usuń <...> */ char*o=s,*p=s; while(*p){ if(*p=='<'){ const char*e=strchr(p,'>'); if(e){ p=e+1; continue; } } *o++=*p++; } *o=0; html_unescape(s); }
static long parse_vtt_time(const char*s){
    long a,b,c,ms; int nn;
    if(sscanf(s,"%ld:%2ld:%2ld.%3ld%n",&a,&b,&c,&ms,&nn)>=4) return ((a*3600+b*60+c)*1000)+ms;   /* HH:MM:SS.mmm */
    if(sscanf(s,"%2ld:%2ld.%3ld%n",&a,&b,&ms,&nn)>=3) return ((a*60+b)*1000)+ms;                 /* MM:SS.mmm */
    return -1;
}
static void parse_vtt(const char*text,Cues*out){
    Lines L=split_lines(text); int i=0;
    while(i<L.n){
        while(i<L.n && L.a[i][0]==0) i++;
        int start=i; while(i<L.n && L.a[i][0]!=0) i++;
        int blk=i-start; if(blk<=0) continue;
        char head[16]; snprintf(head,sizeof head,"%s",L.a[start]); for(char*h=head;*h;h++)*h=toupper((unsigned char)*h);
        if(!strncmp(head,"WEBVTT",6)||!strncmp(head,"NOTE",4)||!strncmp(head,"STYLE",5)||!strncmp(head,"REGION",6)) continue;
        int ts=-1; for(int k=start;k<start+blk;k++) if(strstr(L.a[k],"-->")){ ts=k; break; }
        if(ts<0) continue;
        const char*arrow=strstr(L.a[ts],"-->");
        char left[32]; { const char*p=L.a[ts]; size_t n=arrow-p; if(n>=sizeof left)n=sizeof left-1; memcpy(left,p,n); left[n]=0; strip_inplace(left); }
        char right[64]; snprintf(right,sizeof right,"%s",arrow+3); strip_inplace(right);
        long s_ms=parse_vtt_time(left), e_ms=parse_vtt_time(right); if(s_ms<0||e_ms<0) continue;
        Cue*q=cues_push(out); q->start=s_ms; q->end=e_ms;
        for(int k=ts+1;k<start+blk;k++){ char*seg=xmalloc(strlen(L.a[k])+1); strcpy(seg,L.a[k]); vtt_clean(seg); cue_addline(q,seg,strlen(seg)); free(seg); }
    }
    lines_free(&L);
}

/* MPL2: [start][end]text (dziesiąte sekundy); '/' na początku linii = kursywa (usuwane) */
static void parse_mpl2(const char*text,Cues*out){
    Lines L=split_lines(text);
    for(int i=0;i<L.n;i++){ char*s=xmalloc(strlen(L.a[i])+1); strcpy(s,L.a[i]); strip_inplace(s);
        long a,b; int nn=0;
        if(sscanf(s,"[%ld][%ld]%n",&a,&b,&nn)==2){
            const char*body=s+nn; Cue*q=cues_push(out); q->start=a*100; q->end=b*100;
            const char*p=body; while(1){ const char*bar=strchr(p,'|'); size_t len=bar?(size_t)(bar-p):strlen(p);
                const char*seg=p; if(len>0&&seg[0]=='/'){ seg++; len--; }
                cue_addline(q,seg,len); if(!bar) break; p=bar+1; }
        }
        free(s);
    }
    lines_free(&L);
}
/* TMPlayer: hh:mm:ss[:=]text ; koniec = start następnej lub +3000 */
static void parse_tmplayer(const char*text,Cues*out){
    Lines L=split_lines(text); int first=out->n;
    for(int i=0;i<L.n;i++){ char*s=xmalloc(strlen(L.a[i])+1); strcpy(s,L.a[i]); strip_inplace(s);
        long h,m,sec; char sep; int nn=0;
        if(sscanf(s,"%ld:%2ld:%2ld%c%n",&h,&m,&sec,&sep,&nn)>=4 && (sep==':'||sep=='=')){
            const char*body=s+nn; long st=(h*3600+m*60+sec)*1000; Cue*q=cues_push(out); q->start=st; q->end=st+3000;
            const char*p=body; while(1){ const char*bar=strchr(p,'|'); size_t len=bar?(size_t)(bar-p):strlen(p);
                cue_addline(q,p,len); if(!bar) break; p=bar+1; }
        }
        free(s);
    }
    for(int i=first;i<out->n-1;i++) out->a[i].end=out->a[i+1].start;
    lines_free(&L);
}
/* ASS/SSA: sekcja [Events], Format: mapuje kolumny, Dialogue: czasy + Text (ostatnie pole) */
static long ass_ts(const char*s){ long h,m,sec,cs; if(sscanf(s,"%ld:%2ld:%2ld.%2ld",&h,&m,&sec,&cs)>=4) return (h*3600+m*60+sec)*1000+cs*10; return 0; }
static void parse_ass(const char*text,Cues*out){
    Lines L=split_lines(text); int in_events=0, idx_start=1, idx_end=2, idx_text=9;
    for(int i=0;i<L.n;i++){ char*s=xmalloc(strlen(L.a[i])+1); strcpy(s,L.a[i]); strip_inplace(s);
        if(s[0]=='['){ in_events = (strcasecmp(s,"[events]")==0); free(s); continue; }
        if(!in_events || s[0]==0){ free(s); continue; }
        if(!strncasecmp(s,"format:",7)){
            /* policz indeksy start/end/text */ int col=0; idx_start=1;idx_end=2;idx_text=9; int ncol=0;
            char*p=s+7; char*tok=strtok(p,","); while(tok){ while(*tok==' ')tok++;
                if(!strcasecmp(tok,"start")) idx_start=col; else if(!strcasecmp(tok,"end")) idx_end=col; else if(!strcasecmp(tok,"text")) idx_text=col;
                col++; ncol++; tok=strtok(NULL,","); }
            if(idx_text>=ncol) idx_text=ncol-1;
            free(s); continue;
        }
        if(!strncasecmp(s,"dialogue:",9)){
            char*p=s+9; /* podziel na pola po przecinku, ale Text (idx_text) w całości */
            char*fields[32]; int nf=0; char*cur=p;
            for(char*q=p; nf<idx_text && *q; q++){ if(*q==','){ *q=0; fields[nf++]=cur; cur=q+1; } }
            fields[nf++]=cur; /* reszta = Text */
            if(nf>idx_text && idx_start<nf && idx_end<nf){
                Cue*q=cues_push(out); q->start=ass_ts(fields[idx_start]); q->end=ass_ts(fields[idx_end]);
                char*txt=xmalloc(strlen(fields[idx_text])+1); strcpy(txt,fields[idx_text]);
                /* usuń {..}, zamień \N \n na nowe linie, \h na spację */
                char*tmp=xmalloc(strlen(txt)*1+1); { char*o=tmp,*r=txt; while(*r){ if(*r=='{'){ char*e=strchr(r,'}'); if(e){ r=e+1; continue; } } *o++=*r++; } *o=0; }
                /* split po \N / \n */ const char*r=tmp; while(1){ const char*br=NULL; for(const char*z=r; *z; z++){ if(z[0]=='\\'&&(z[1]=='N'||z[1]=='n')){ br=z; break; } }
                    size_t len= br?(size_t)(br-r):strlen(r); char*seg=xmalloc(len+1); size_t o=0; for(size_t j=0;j<len;j++){ if(r[j]=='\\'&&j+1<len&&r[j+1]=='h'){ seg[o++]=' '; j++; } else seg[o++]=r[j]; } seg[o]=0;
                    cue_addline(q,seg,strlen(seg)); free(seg); if(!br) break; r=br+2; }
                free(tmp); free(txt);
            }
        }
        free(s);
    }
    lines_free(&L);
}

/* wykrycie formatu — pełne (srt/microdvd/mpl2/tmplayer/vtt/ass) */
static int contains_ci(const char*hay,const char*needle){ size_t n=strlen(needle); for(const char*p=hay;*p;p++){ if(!strncasecmp(p,needle,n)) return 1; } return 0; }
static const char* detect_format(const char*text){
    const char*h=text; if((unsigned char)h[0]==0xef&&(unsigned char)h[1]==0xbb&&(unsigned char)h[2]==0xbf) h+=3;
    while(*h&&is_ascii_ws(*h)) h++;
    if(!strncasecmp(h,"WEBVTT",6)) return "vtt";
    if(contains_ci(text,"[script info]")||contains_ci(text,"[v4+ styles]")||contains_ci(text,"[v4 styles]")
       ||(contains_ci(text,"dialogue:")&&contains_ci(text,"[events]"))) return "ass";
    if(strstr(text,"-->")) return "srt";
    const char*p=text; while(*p){ const char*nl=strchr(p,'\n'); size_t len=nl?(size_t)(nl-p):strlen(p);
        /* pierwsza niepusta linia */ size_t j=0; while(j<len&&is_ascii_ws(p[j]))j++;
        if(j<len){ char c=p[j];
            if(c=='{') return "microdvd";
            if(c=='['){ long a,b; if(sscanf(p+j,"[%ld][%ld]",&a,&b)==2) return "mpl2"; }
            long H,M,S; char sp; if(sscanf(p+j,"%ld:%2ld:%2ld%c",&H,&M,&S,&sp)>=4&&(sp==':'||sp=='=')) return "tmplayer";
            break; }
        if(!nl) break; p=nl+1; }
    return "srt";
}

static void parse_any(const char*text,double fps,Cues*out){
    const char*fmt=detect_format(text);
    if(!strcmp(fmt,"microdvd")) parse_microdvd(text,fps,out);
    else if(!strcmp(fmt,"mpl2")) parse_mpl2(text,out);
    else if(!strcmp(fmt,"tmplayer")) parse_tmplayer(text,out);
    else if(!strcmp(fmt,"vtt")) parse_vtt(text,out);
    else if(!strcmp(fmt,"ass")) parse_ass(text,out);
    else parse_srt(text,out);
}

/* raport sanityzacji — te same pola i etykiety co SanitizeReport w Pythonie */
typedef struct { int tags,sdh,lng,overlaps,nonpos,shortx,empty,total; } SanReport;
static int san_any(const SanReport*r){ return r->tags||r->sdh||r->lng||r->overlaps||r->nonpos||r->shortx||r->empty; }
static void san_summary(const SanReport*r,SB*out){ int first=1;
    #define ADD(cond,fmt,val) do{ if(cond){ if(!first) sb_puts(out,", "); char t[64]; snprintf(t,sizeof t,fmt,val); sb_puts(out,t); first=0; } }while(0)
    ADD(r->tags,"usunięto tagi w %d",r->tags);
    ADD(r->sdh,"usunięto SDH w %d",r->sdh);
    ADD(r->lng,"skrócono %d zbyt długich",r->lng);
    ADD(r->overlaps,"naprawiono %d nakładek",r->overlaps);
    ADD(r->nonpos,"naprawiono %d złych czasów",r->nonpos);
    ADD(r->shortx,"wydłużono %d zbyt krótkich",r->shortx);
    ADD(r->empty,"usunięto %d pustych",r->empty);
    #undef ADD
}
typedef struct { int enabled, keep_tags, strip_sdh; long max_display_ms, min_display_ms; } SanOpts;
static const SanOpts SAN_DEFAULT = {1,0,0,MAX_DISPLAY_MS,0};

/* sanityzacja — jak sanitize_cues (z opcjami) */
static void sanitize(Cues*in,Cues*out,SanReport*rep,const SanOpts*o){
    memset(rep,0,sizeof *rep); cues_init(out);
    for(int i=0;i<in->n;i++){ Cue*c=&in->a[i];
        Cue tmp; tmp.lines=NULL; tmp.nlines=0; int changed=0, sdh=0;
        for(int k=0;k<c->nlines;k++){ char*s=xmalloc(strlen(c->lines[k])+1); strcpy(s,c->lines[k]);
            if(!o->keep_tags){ strip_format_tags(s); if(strcmp(s,c->lines[k])!=0) changed=1; }
            tmp.lines=xrealloc(tmp.lines,(tmp.nlines+1)*sizeof(char*)); tmp.lines[tmp.nlines++]=s; }
        if(changed) rep->tags++;
        if(o->strip_sdh){ int ch=0; for(int k=0;k<tmp.nlines;k++){ char*b=xmalloc(strlen(tmp.lines[k])+1); strcpy(b,tmp.lines[k]); strip_sdh_line(tmp.lines[k]); if(strcmp(b,tmp.lines[k])!=0) ch=1; free(b); } if(ch) rep->sdh++; }
        int keep=0; for(int k=0;k<tmp.nlines;k++){ strip_inplace(tmp.lines[k]); if(tmp.lines[k][0]!=0) tmp.lines[keep++]=tmp.lines[k]; else free(tmp.lines[k]); }
        tmp.nlines=keep;
        if(tmp.nlines==0){ free(tmp.lines); rep->empty++; continue; }
        long start=c->start,end=c->end; if(end<=start){ end=start+(o->min_display_ms?o->min_display_ms:1000); rep->nonpos++; }
        Cue*q=cues_push(out); q->start=start; q->end=end; q->lines=tmp.lines; q->nlines=tmp.nlines;
    }
    for(int i=0;i<out->n;i++){ Cue*c=&out->a[i]; long nxt = (i+1<out->n)? out->a[i+1].start : -1;
        if(nxt>=0 && c->end>nxt){ long v=c->start+1; c->end=(nxt>v)?nxt:v; rep->overlaps++; }
        if(c->end-c->start>o->max_display_ms){ c->end=c->start+o->max_display_ms; rep->lng++; }
        if(o->min_display_ms && c->end-c->start<o->min_display_ms){ long tgt=c->start+o->min_display_ms; if(nxt>=0&&tgt>nxt) tgt=nxt; if(tgt>c->end){ c->end=tgt; rep->shortx++; } }
    }
    rep->total=out->n;
}

/* emit SRT: BOM + treść LF (jak cues_to_srt + emit_srt) */
static void emit_srt(Cues*c,SB*out){
    SB body; sb_init(&body);
    for(int i=0;i<c->n;i++){ char t1[16],t2[16]; ms_to_srt(c->a[i].start,t1); ms_to_srt(c->a[i].end,t2);
        char num[16]; snprintf(num,sizeof num,"%d",i+1); sb_puts(&body,num); sb_putc(&body,'\n');
        sb_puts(&body,t1); sb_puts(&body," --> "); sb_puts(&body,t2); sb_putc(&body,'\n');
        if(c->a[i].nlines==0){ sb_putc(&body,'\n'); }
        for(int k=0;k<c->a[i].nlines;k++){ sb_puts(&body,c->a[i].lines[k]); sb_putc(&body,'\n'); }
        sb_putc(&body,'\n');
    }
    /* strip() całości + "\n" */
    size_t s=0,e=body.len; while(s<e&&is_ascii_ws(body.b[s]))s++; while(e>s&&is_ascii_ws(body.b[e-1]))e--;
    sb_putn(out,"\xef\xbb\xbf",3);
    sb_putn(out,body.b+s,e-s); sb_putc(out,'\n');
    free(body.b);
}

/* --- emittery formatów wyjściowych --- */
static void ms_to_vtt(long ms,char out[16]){ char t[16]; ms_to_srt(ms,t); for(char*p=t;*p;p++) if(*p==',')*p='.'; strcpy(out,t); }
static void ms_to_ass(long ms,char out[16]){ if(ms<0)ms=0; long h=ms/3600000; ms-=h*3600000; long m=ms/60000; ms-=m*60000; long s=ms/1000; long cs=(ms-s*1000)/10; snprintf(out,16,"%ld:%02ld:%02ld.%02ld",h,m,s,cs); }
static void emit_join(SB*b,char**lines,int n,const char*sep){ for(int k=0;k<n;k++){ if(k)sb_puts(b,sep); sb_puts(b,lines[k]); } if(n==0) sb_puts(b,""); }
static void cues_to_vtt(Cues*c,SB*out){ SB b; sb_init(&b); sb_puts(&b,"WEBVTT\n\n");
    for(int i=0;i<c->n;i++){ char t1[16],t2[16]; ms_to_vtt(c->a[i].start,t1); ms_to_vtt(c->a[i].end,t2);
        sb_puts(&b,t1); sb_puts(&b," --> "); sb_puts(&b,t2); sb_putc(&b,'\n');
        for(int k=0;k<c->a[i].nlines;k++){ sb_puts(&b,c->a[i].lines[k]); sb_putc(&b,'\n'); } if(c->a[i].nlines==0) sb_putc(&b,'\n');
        sb_putc(&b,'\n'); }
    size_t s=0,e=b.len; while(s<e&&is_ascii_ws(b.b[s]))s++; while(e>s&&is_ascii_ws(b.b[e-1]))e--; sb_putn(out,b.b+s,e-s); sb_putc(out,'\n'); free(b.b); }
static void cues_to_microdvd(Cues*c,double fps,SB*out){ for(int i=0;i<c->n;i++){ long sf=(long)(c->a[i].start*fps/1000.0+0.5), ef=(long)(c->a[i].end*fps/1000.0+0.5);
        char h[32]; snprintf(h,sizeof h,"{%ld}{%ld}",sf,ef); sb_puts(out,h); emit_join(out,c->a[i].lines,c->a[i].nlines,"|"); sb_putc(out,'\n'); } }
static const char*ASS_HEADER="[Script Info]\nScriptType: v4.00+\nCollisions: Normal\nPlayResX: 1920\nPlayResY: 1080\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,1,2,10,10,20,1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
static void cues_to_ass(Cues*c,SB*out){ sb_puts(out,ASS_HEADER); /* separatory jak "\n".join([HEADER,D1,...])+"\n" */
    for(int i=0;i<c->n;i++){ char t1[16],t2[16]; ms_to_ass(c->a[i].start,t1); ms_to_ass(c->a[i].end,t2);
        sb_putc(out,'\n'); sb_puts(out,"Dialogue: 0,"); sb_puts(out,t1); sb_putc(out,','); sb_puts(out,t2); sb_puts(out,",Default,,0,0,0,,"); emit_join(out,c->a[i].lines,c->a[i].nlines,"\\N"); }
    sb_putc(out,'\n'); }
static void emit_subtitle(Cues*c,const char*fmt,double fps,SB*out){
    if(!strcmp(fmt,"srt")) emit_srt(c,out);
    else if(!strcmp(fmt,"vtt")) cues_to_vtt(c,out);
    else if(!strcmp(fmt,"ass")) cues_to_ass(c,out);
    else if(!strcmp(fmt,"microdvd")) cues_to_microdvd(c,fps,out);
    else die("Nieobsługiwany format wyjściowy");
}
static const char* fmt_from_ext(const char*out,const char*explicit_fmt){
    if(explicit_fmt) return explicit_fmt;
    const char*d=strrchr(out,'.'); if(!d) return "srt";
    if(!strcasecmp(d,".vtt")) return "vtt"; if(!strcasecmp(d,".ass")||!strcasecmp(d,".ssa")) return "ass";
    if(!strcasecmp(d,".sub")||!strcasecmp(d,".txt")) return "microdvd"; return "srt";
}

/* --- dekodowanie wejścia (UTF-8, inaczej cp1250) --- */
static const uint16_t CP1250[128]={
0x20AC,0x0081,0x201A,0x0083,0x201E,0x2026,0x2020,0x2021,0x0088,0x2030,0x0160,0x2039,0x015A,0x0164,0x017D,0x0179,
0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x0098,0x2122,0x0161,0x203A,0x015B,0x0165,0x017E,0x017A,
0x00A0,0x02C7,0x02D8,0x0141,0x00A4,0x0104,0x00A6,0x00A7,0x00A8,0x00A9,0x015E,0x00AB,0x00AC,0x00AD,0x00AE,0x017B,
0x00B0,0x00B1,0x02DB,0x0142,0x00B4,0x00B5,0x00B6,0x00B7,0x00B8,0x0105,0x015F,0x00BB,0x013D,0x02DD,0x013E,0x017C,
0x0154,0x00C1,0x00C2,0x0102,0x00C4,0x0139,0x0106,0x00C7,0x010C,0x00C9,0x0118,0x00CB,0x011A,0x00CD,0x00CE,0x010E,
0x0110,0x0143,0x0147,0x00D3,0x00D4,0x0150,0x00D6,0x00D7,0x0158,0x016E,0x00DA,0x0170,0x00DC,0x00DD,0x0162,0x00DF,
0x0155,0x00E1,0x00E2,0x0103,0x00E4,0x013A,0x0107,0x00E7,0x010D,0x00E9,0x0119,0x00EB,0x011B,0x00ED,0x00EE,0x010F,
0x0111,0x0144,0x0148,0x00F3,0x00F4,0x0151,0x00F6,0x00F7,0x0159,0x016F,0x00FA,0x0171,0x00FC,0x00FD,0x0163,0x02D9};
static int is_utf8(const unsigned char*p,size_t n){ size_t i=0; while(i<n){ unsigned char c=p[i]; int e;
    if(c<0x80) e=0; else if((c>>5)==0x6) e=1; else if((c>>4)==0xE) e=2; else if((c>>3)==0x1E) e=3; else return 0;
    if(i+e>=n && e>0){ if(i+e>n-1+0 && (i+ (size_t)e)>=n) {} }
    for(int k=1;k<=e;k++){ if(i+k>=n) return 0; if((p[i+k]&0xC0)!=0x80) return 0; } i+=e+1; } return 1; }
static char* decode_text(const unsigned char*in,size_t n){
    /* pomiń BOM */ if(n>=3&&in[0]==0xef&&in[1]==0xbb&&in[2]==0xbf){ in+=3; n-=3; }
    if(is_utf8(in,n)){ char*d=xmalloc(n+1); memcpy(d,in,n); d[n]=0; return d; }
    SB b; sb_init(&b); for(size_t i=0;i<n;i++){ unsigned char c=in[i];
        uint32_t cp = (c<0x80)? c : CP1250[c-0x80];
        if(cp<0x80) sb_putc(&b,(char)cp);
        else if(cp<0x800){ sb_putc(&b,(char)(0xC0|(cp>>6))); sb_putc(&b,(char)(0x80|(cp&0x3F))); }
        else { sb_putc(&b,(char)(0xE0|(cp>>12))); sb_putc(&b,(char)(0x80|((cp>>6)&0x3F))); sb_putc(&b,(char)(0x80|(cp&0x3F))); } }
    return b.b;
}

/* pełny pipeline: bajty (dekodowane) -> format (rzuca błąd gdy 0 linii) */
static void convert_bytes(const unsigned char*bytes,size_t nbytes,double fps,
                          const char*outfmt,const SanOpts*opt,SB*out,SanReport*rep){
    char*text=decode_text(bytes,nbytes);
    Cues raw; cues_init(&raw); parse_any(text,fps,&raw);
    int nonws=0; for(const char*q=text;*q;q++) if(!is_ascii_ws(*q)){ nonws=1; break; }
    if(raw.n==0 && nonws) die("Napisy wyglądają na uszkodzone lub w nierozpoznanym formacie (0 rozpoznanych linii) — nie zapisuję.");
    memset(rep,0,sizeof *rep);
    Cues*use=&raw, clean;
    if(opt->enabled){ sanitize(&raw,&clean,rep,opt); use=&clean; } else { rep->total=raw.n; }
    emit_subtitle(use,outfmt,fps,out);
    free(text);
}
/* wypisz komunikat "Zapisano" + ew. "Korekty" (jak _save_subtitles) */
static void print_saved(const char*outp,size_t bytes,const SanReport*rep){
    printf("Zapisano: %s (%zu B, %d linii, SRT UTF-8+BOM/LF)\n",outp,bytes,rep->total);
    if(san_any(rep)){ SB s; sb_init(&s); san_summary(rep,&s); printf("  Korekty: %s\n",s.b); free(s.b); }
}

/* ---------------------------------------------------------------- HTTP (plain) */
static char* http_request(const char*host,const char*req,size_t reqlen,size_t*bodylen){
    struct addrinfo hints,*res=NULL; memset(&hints,0,sizeof hints); hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(host,"80",&hints,&res)!=0) return NULL;
    int fd=-1; for(struct addrinfo*a=res;a;a=a->ai_next){ fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol); if(fd<0) continue;
        if(connect(fd,a->ai_addr,a->ai_addrlen)==0) break; close(fd); fd=-1; }
    freeaddrinfo(res); if(fd<0) return NULL;
    size_t off=0; while(off<reqlen){ ssize_t w=write(fd,req+off,reqlen-off); if(w<=0){ close(fd); return NULL; } off+=w; }
    SB resp; sb_init(&resp); char buf[8192]; ssize_t r;
    while((r=read(fd,buf,sizeof buf))>0) sb_putn(&resp,buf,r);
    close(fd);
    char*sep=strstr(resp.b,"\r\n\r\n"); char*body; size_t bl;
    if(sep){ body=sep+4; bl=resp.len-(body-resp.b); } else { body=resp.b; bl=resp.len; }
    char*out=xmalloc(bl+1); memcpy(out,body,bl); out[bl]=0; *bodylen=bl; free(resp.b); return out;
}

/* napiprojekt mode=1 -> zwróć bajty napisów (base64 z <content>) albo NULL */
static unsigned char* np_download(const char*movie_hash,const char*lang,size_t*outlen){
    const char*host="www.napiprojekt.pl"; const char*boundary="----aqnapicafe0001";
    SB b; sb_init(&b);
    const char*fields[][2]={{"client","pynapi"},{"client_ver",VERSION},{"mode","1"},
        {"downloaded_subtitles_id",movie_hash},{"downloaded_subtitles_lang",lang},{"downloaded_subtitles_txt","1"}};
    for(int i=0;i<6;i++){ sb_puts(&b,"--"); sb_puts(&b,boundary); sb_puts(&b,"\r\n");
        sb_puts(&b,"Content-Disposition: form-data; name=\""); sb_puts(&b,fields[i][0]); sb_puts(&b,"\"\r\n\r\n");
        sb_puts(&b,fields[i][1]); sb_puts(&b,"\r\n"); }
    sb_puts(&b,"--"); sb_puts(&b,boundary); sb_puts(&b,"--\r\n");
    SB req; sb_init(&req); char hdr[512];
    snprintf(hdr,sizeof hdr,
        "POST /api/api-napiprojekt3.php HTTP/1.0\r\nHost: %s\r\nUser-Agent: aqnapi-c/%s\r\n"
        "Accept: */*\r\nContent-Type: multipart/form-data; boundary=%s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n", host,VERSION,boundary,b.len);
    sb_puts(&req,hdr); sb_putn(&req,b.b,b.len); free(b.b);
    size_t bl; char*body=http_request(host,req.b,req.len,&bl); free(req.b);
    if(!body) die("napiprojekt: błąd połączenia");
    char*c1=strstr(body,"<content>"); char*c2= c1? strstr(c1,"</content>"):NULL;
    if(!c1||!c2){ free(body); return NULL; }
    char*s=c1+9,*e=c2;
    /* zdejmij CDATA */
    char*cd=strstr(s,"<![CDATA["); if(cd && cd<e){ s=cd+9; char*ce=strstr(s,"]]>"); if(ce&&ce<e) e=ce; }
    unsigned char*data=b64decode(s,(size_t)(e-s),outlen); free(body); return data;
}
static double np_file_info_fps(const char*movie_hash){
    const char*host="napiprojekt.pl"; SB req; sb_init(&req); char hdr[512];
    snprintf(hdr,sizeof hdr,
        "GET /api/api.php?mode=file_info&client=dreambox&id=%s HTTP/1.0\r\nHost: %s\r\n"
        "User-Agent: aqnapi-c/%s\r\nConnection: close\r\n\r\n", movie_hash,host,VERSION);
    sb_puts(&req,hdr); size_t bl; char*body=http_request(host,req.b,req.len,&bl); free(req.b);
    if(!body) return 0; char*p=strstr(body,"<fps>"); double v=0; if(p) v=atof(p+5); free(body); return v;
}

/* napiprojekt upload (mode=512/1024) — multipart z archiwum 7z-AES; zwraca XML (malloc) */
static char* b64encode(const unsigned char*in,size_t n){ static const char*T="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char*out=xmalloc((n+2)/3*4+1); size_t o=0;
    for(size_t i=0;i<n;i+=3){ unsigned v=(unsigned)in[i]<<16; int pad=0; if(i+1<n)v|=(unsigned)in[i+1]<<8; else pad++; if(i+2<n)v|=in[i+2]; else pad++;
        out[o++]=T[(v>>18)&63]; out[o++]=T[(v>>12)&63]; out[o++]=pad>=2?'=':T[(v>>6)&63]; out[o++]=pad>=1?'=':T[v&63]; }
    out[o]=0; return out; }
/* hasło napiprojekt: XOR kluczem 3 -> base64 (jak encode_password / user_password) */
static char* np_encode_password(const char*pw){ size_t n=strlen(pw); unsigned char*x=xmalloc(n?n:1); for(size_t i=0;i<n;i++) x[i]=(unsigned char)pw[i]^3; char*e=b64encode(x,n); free(x); return e; }
/* napiprojekt upload. authenticate!=0 -> przypisz do konta (user_nick/user_password MAŁĄ literą, XOR+base64). */
static char* np_upload_http(const char*movie_hash,const unsigned char*subbytes,size_t sublen,
                            const char*lang,const char*author,int corrected,const char*comment,int testing,
                            int authenticate,const char*user,const char*password){
    char entry[64],arcname[64]; snprintf(entry,sizeof entry,"%s.txt",movie_hash); snprintf(arcname,sizeof arcname,"%s.zip",movie_hash);
    size_t arclen; unsigned char*arc=write_7z_aes(entry,subbytes,sublen,&arclen);
    char subs_md5[33]; md5_bytes(subbytes,sublen,subs_md5);
    const char*eauthor = (author&&author[0]) ? author : ((authenticate&&user)?user:"");  /* pusty autor -> login */
    const char*bnd="----aqnapicafe0002"; SB b; sb_init(&b);
    #define TF(name,val) do{ sb_puts(&b,"--"); sb_puts(&b,bnd); sb_puts(&b,"\r\nContent-Disposition: form-data; name=\""); sb_puts(&b,name); sb_puts(&b,"\"\r\n\r\n"); sb_puts(&b,val); sb_puts(&b,"\r\n"); }while(0)
    TF("client","pynapi"); TF("client_ver",VERSION); TF("mode",corrected?"1024":"512");
    TF("SubtitlesHash",subs_md5); TF("SubtitlesAutor",eauthor); TF("SubtitlesLang",lang);
    if(authenticate && user && user[0] && password){ TF("user_nick",user); char*ep=np_encode_password(password); TF("user_password",ep); free(ep); }
    if(comment&&comment[0]) TF("SubtitlesComment",comment); if(testing) TF("OnlyTesting","1");
    #undef TF
    sb_puts(&b,"--"); sb_puts(&b,bnd); sb_puts(&b,"\r\nContent-Disposition: form-data; name=\"subtitles\"; filename=\"");
    sb_puts(&b,arcname); sb_puts(&b,"\"\r\nContent-Type: subtitles/zip\r\n\r\n"); sb_putn(&b,(char*)arc,arclen); sb_puts(&b,"\r\n");
    sb_puts(&b,"--"); sb_puts(&b,bnd); sb_puts(&b,"--\r\n"); free(arc);
    SB req; sb_init(&req); char hdr[512];
    snprintf(hdr,sizeof hdr,"POST /api/api-napiprojekt3.php HTTP/1.0\r\nHost: www.napiprojekt.pl\r\nUser-Agent: aqnapi-c/%s\r\nAccept: */*\r\nContent-Type: multipart/form-data; boundary=%s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",VERSION,bnd,b.len);
    sb_puts(&req,hdr); sb_putn(&req,b.b,b.len); free(b.b);
    size_t bl; char*body=http_request("www.napiprojekt.pl",req.b,req.len,&bl); free(req.b);
    return body;
}

static const char* basename_of(const char*p);   /* fwd */

/* GET po czystym HTTP/1.0 (z Referer) */
static char* http_get_url(const char*host,const char*path,size_t*len){
    SB req; sb_init(&req); char hdr[1200];
    snprintf(hdr,sizeof hdr,"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: aqnapi-c/%s\r\nReferer: http://%s\r\nAccept: */*\r\nConnection: close\r\n\r\n",path,host,VERSION,host);
    sb_puts(&req,hdr); char*b=http_request(host,req.b,req.len,len); free(req.b); return b;
}
static char* url_encode(const char*s){ static const char*H="0123456789ABCDEF"; SB b; sb_init(&b);
    for(const unsigned char*p=(const unsigned char*)s;*p;p++){ if(isalnum(*p)||*p=='-'||*p=='_'||*p=='.'||*p=='~') sb_putc(&b,*p);
        else { sb_putc(&b,'%'); sb_putc(&b,H[*p>>4]); sb_putc(&b,H[*p&15]); } } return b.b; }

/* zaciemnianie pól napisy24 (obf): XOR maską, reverse, UPPER hex */
static char* n24_obf(const char*s){ size_t n=strlen(s); unsigned char*enc=xmalloc(n?n:1);
    for(size_t i=0;i<n;i++){ int mask=(0x7F+(int)((i+1)*(i+1)))&0xFF; enc[i]=(unsigned char)s[i]^mask; }
    static const char*H="0123456789ABCDEF"; char*out=xmalloc(n*2+1);
    for(size_t i=0;i<n;i++){ unsigned char b=enc[n-1-i]; out[i*2]=H[b>>4]; out[i*2+1]=H[b&15]; } out[n*2]=0; free(enc); return out; }

/* wyciągnij największy plik napisowy z archiwum ZIP (central dir + inflate/stored) */
static unsigned char* zip_extract(const unsigned char*z,size_t zn,size_t*outlen){
    if(zn<22) return NULL; long e=-1; long lim=(long)zn-22-65536; if(lim<0)lim=0;
    for(long i=(long)zn-22;i>=lim;i--){ if(z[i]==0x50&&z[i+1]==0x4b&&z[i+2]==0x05&&z[i+3]==0x06){ e=i; break; } }
    if(e<0) return NULL;
    uint32_t cd_off=z[e+16]|(z[e+17]<<8)|(z[e+18]<<16)|((uint32_t)z[e+19]<<24);
    uint16_t cnt=z[e+10]|(z[e+11]<<8);
    long p=cd_off, best_lho=-1; uint32_t best_us=0,best_cs=0; uint16_t best_m=0; int have=0;
    for(int k=0;k<cnt && p+46<=(long)zn;k++){ if(!(z[p]==0x50&&z[p+1]==0x4b&&z[p+2]==0x01&&z[p+3]==0x02)) break;
        uint16_t method=z[p+10]|(z[p+11]<<8);
        uint32_t csize=z[p+20]|(z[p+21]<<8)|(z[p+22]<<16)|((uint32_t)z[p+23]<<24);
        uint32_t usize=z[p+24]|(z[p+25]<<8)|(z[p+26]<<16)|((uint32_t)z[p+27]<<24);
        uint16_t nlen=z[p+28]|(z[p+29]<<8), elen=z[p+30]|(z[p+31]<<8), clen=z[p+32]|(z[p+33]<<8);
        uint32_t lho=z[p+42]|(z[p+43]<<8)|(z[p+44]<<16)|((uint32_t)z[p+45]<<24);
        const char*name=(const char*)z+p+46;
        int is_url=(nlen>=4)&&!strncasecmp(name+nlen-4,".url",4);
        if(!is_url && (!have || usize>best_us)){ have=1; best_us=usize; best_cs=csize; best_m=method; best_lho=lho; }
        p+=46+nlen+elen+clen; }
    if(best_lho<0||best_lho+30>(long)zn) return NULL;
    const unsigned char*L=z+best_lho; if(!(L[0]==0x50&&L[1]==0x4b&&L[2]==0x03&&L[3]==0x04)) return NULL;
    uint16_t lnlen=L[26]|(L[27]<<8), lelen=L[28]|(L[29]<<8);
    const unsigned char*data=L+30+lnlen+lelen;
    unsigned char*o=xmalloc(best_us+1);
    if(best_m==0){ memcpy(o,data,best_us); o[best_us]=0; *outlen=best_us; return o; }
    z_stream s; memset(&s,0,sizeof s); if(inflateInit2(&s,-15)!=Z_OK){ free(o); return NULL; }
    s.next_in=(unsigned char*)data; s.avail_in=best_cs; s.next_out=o; s.avail_out=best_us;
    inflate(&s,Z_FINISH); inflateEnd(&s); *outlen=s.total_out; o[s.total_out]=0; return o;
}

/* napisy24 CheckSubAgent (bez logowania) -> bajty ZIP lub NULL */
static unsigned char* n24_checksub_agent(const char*movie,const char*lang,size_t*outlen){
    char osh[17]; if(oshash(movie,osh)!=0) return NULL; char md[33]; md5_10mb(movie,md);
    for(char*p=osh;*p;p++)*p=toupper((unsigned char)*p);
    char fs[32]; snprintf(fs,sizeof fs,"%ld",input_size(movie)); char fnb[512]; input_basename(movie,fnb,sizeof fnb); const char*fn=fnb;
    char*efn=url_encode(fn);
    SB body; sb_init(&body); char tmp[512];
    snprintf(tmp,sizeof tmp,"postAction=CheckSub&ua=dmnapi&ap=4lumen28&fh=%s&md=%s&fs=%s&fn=%s&nl=%s",osh,md,fs,efn,lang); sb_puts(&body,tmp); free(efn);
    SB req; sb_init(&req); char hdr[512];
    snprintf(hdr,sizeof hdr,"POST /run/CheckSubAgent.php HTTP/1.0\r\nHost: napisy24.pl\r\nUser-Agent: Mozilla/4.0\r\nAccept: */*\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",body.len);
    sb_puts(&req,hdr); sb_putn(&req,body.b,body.len); free(body.b);
    size_t bl; char*resp=http_request("napisy24.pl",req.b,req.len,&bl); free(req.b);
    if(!resp) die("napisy24: błąd połączenia");
    /* format: OK-N|meta||<zip> */
    char*sep=NULL; for(size_t i=0;i+1<bl;i++) if(resp[i]=='|'&&resp[i+1]=='|'){ sep=resp+i; break; }
    int count=0; sscanf(resp,"OK-%d",&count);
    if(count<=0||!sep){ free(resp); return NULL; }
    size_t zlen=bl-(sep+2-resp); unsigned char*zip=xmalloc(zlen); memcpy(zip,sep+2,zlen);
    unsigned char*sub=zip_extract(zip,zlen,outlen); free(zip); free(resp); return sub;
}

/* napisy24 download.php?napisId=N -> bajty ZIP -> napisy */
static unsigned char* n24_download_id(const char*id,size_t*outlen){
    char path[128]; char*eid=url_encode(id); snprintf(path,sizeof path,"/run/pages/download.php?napisId=%s",eid); free(eid);
    size_t bl; char*body=http_get_url("napisy24.pl",path,&bl);
    if(!body){ die("napisy24: błąd połączenia"); }
    if(bl<2||body[0]!='P'||body[1]!='K'){ free(body); return NULL; }
    unsigned char*sub=zip_extract((unsigned char*)body,bl,outlen); free(body); return sub;
}

/* ---------------------------------------------------------------- I/O plików */
static char* read_file(const char*path,size_t*len){ FILE*f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); char*b=xmalloc(n+1); size_t r=fread(b,1,n,f); b[r]=0; fclose(f); if(len)*len=r; return b; }
static void write_file(const char*path,const char*data,size_t len){ FILE*f=fopen(path,"wb"); if(!f) die("nie mogę zapisać pliku wyjściowego"); fwrite(data,1,len,f); fclose(f); }

static const char* basename_of(const char*p){ const char*s=strrchr(p,'/'); return s?s+1:p; }
/* Nazwa pliku z wejścia (URL: część path bez query; lokalnie: basename) -> out. */
static void input_basename(const char*path,char*out,size_t osz){
    if(is_url(path)){
        const char*q=strchr(path,'?'); size_t plen=q?(size_t)(q-path):strlen(path);
        const char*sl=path; for(size_t i=0;i<plen;i++) if(path[i]=='/') sl=path+i+1;
        size_t bl=plen-(size_t)(sl-path); if(bl>=osz) bl=osz-1; memcpy(out,sl,bl); out[bl]=0;
    } else { snprintf(out,osz,"%s",basename_of(path)); }
}
/* Człon nazwy wyjściowej (bez ostatniego rozszerzenia) — malloc.
 * Dla URL-a bierze nazwę pliku z URL-a (bieżący katalog), nie ścieżkę serwera. */
static char* input_stem(const char*path){
    char nb[512]; const char*nm;
    if(is_url(path)){ input_basename(path,nb,sizeof nb); nm=nb; } else nm=path;
    const char*dot=strrchr(nm,'.'); size_t base = dot? (size_t)(dot-nm):strlen(nm);
    char*d=xmalloc(base+1); memcpy(d,nm,base); d[base]=0; return d; }

static char* default_out(const char*movie,const char*explicit_out){ if(explicit_out){ char*d=xmalloc(strlen(explicit_out)+1); strcpy(d,explicit_out); return d; }
    char*st=input_stem(movie); char*d=xmalloc(strlen(st)+5); sprintf(d,"%s.srt",st); free(st); return d; }

/* Wczytaj całe wejście (URL http(s) lub plik lokalny). Bufor jest NUL-zakończony. */
static char* read_input(const char*path,size_t*len){
    if(is_url(path)){ size_t n=0; unsigned char*b=url_read_full(path,&n); if(!b) return NULL; if(len)*len=n; return (char*)b; }
    return read_file(path,len);
}

/* ---------------------------------------------------------------- polecenia */
static int cmd_hash(const char*path){
    char osh[17]; int r=oshash(path,osh);
    if(r==-1){ fprintf(stderr,"Błąd: plik za mały na hash OSH (min. %d B): %s\n",2*OSH_CHUNK,path); return 1; }
    if(r==-2){ fprintf(stderr,"Brak pliku: %s\n",path); return 1; }
    char md[33]; if(md5_10mb(path,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",path); return 1; }
    char nm[512]; input_basename(path,nm,sizeof nm);
    printf("OSH (fh)        : %s\n",osh);
    printf("MD5-10MiB (md)  : %s\n",md);
    printf("rozmiar         : %ld B\n",input_size(path));
    printf("nazwa           : %s\n",nm);
    return 0;
}
static int cmd_fps(const char*path){ double f,dur; media_from_file(path,&f,&dur);
    if(f==0){ printf("Nie udało się odczytać FPS z pliku (obsługa: MKV/AVI/MP4/MOV)\n"); return 1; }
    const char*note = trusted_fps(f)? "":"  (poza bramką 22<fps<32 — traktowane jako niepewne)";
    printf("FPS: %.3f%s\n",f,note);
    if(dur>0){ char t[16]; hhmmss(dur,t,sizeof t); printf("Czas: %s\n",t); }
    return 0;
}
static double resolve_fps(const char*movie,double server_fps,double flag_fps){
    if(movie){ double f=trusted_fps(fps_from_file(movie)); if(f) return f; }
    if(trusted_fps(server_fps)) return server_fps;
    if(flag_fps>0) return flag_fps;
    return DEFAULT_FPS;
}
#include <math.h>
static long py_round(double x){ double f=floor(x); double d=x-f; if(d<0.5) return (long)f; if(d>0.5) return (long)f+1; long fl=(long)f; return (fl%2==0)?fl:fl+1; }
static void sync_transform(long*tgt,long*ref,int n,double*scale,double*offset){
    if(n==0){ *scale=1; *offset=0; return; }
    if(n==1){ *scale=1; *offset=(double)(ref[0]-tgt[0]); return; }
    double sx=0,sy=0,sxx=0,sxy=0; for(int i=0;i<n;i++){ sx+=tgt[i]; sy+=ref[i]; sxx+=(double)tgt[i]*tgt[i]; sxy+=(double)tgt[i]*ref[i]; }
    double denom=(double)n*sxx-sx*sx; if(denom==0){ *scale=1; *offset=(sy-sx)/n; return; }
    *scale=((double)n*sxy-sx*sy)/denom; *offset=(sy-*scale*sx)/n;
}
static void apply_sync_c(Cues*in,double scale,double offset,Cues*out){ cues_init(out);
    for(int i=0;i<in->n;i++){ Cue*c=&in->a[i]; long ns=py_round(c->start*scale+offset), ne=py_round(c->end*scale+offset);
        if(ns<0)ns=0; if(ne<0)ne=0; Cue*q=cues_push(out); q->start=ns; q->end=ne;
        for(int k=0;k<c->nlines;k++) cue_addline(q,c->lines[k],strlen(c->lines[k])); } }
/* wczytaj + zdekoduj + sparsuj; błąd gdy 0 linii (jak _load_cues) */
static void load_cues_c(const char*path,double fps,Cues*out){ size_t n; char*data=read_input(path,&n);
    if(!data){ fprintf(stderr,"Brak pliku: %s\n",path); exit(1); }
    char*text=decode_text((unsigned char*)data,n); free(data); cues_init(out); parse_any(text,fps,out); free(text);
    if(out->n==0){ char m[512]; snprintf(m,sizeof m,"Nie rozpoznano napisów w pliku: %s",path); die(m); } }
static long parse_user_time(const char*s){ /* hh:mm:ss[,.]mmm | mm:ss | sekundy */
    while(*s==' ')s++; long h,m,sec,ms; char sep; int nn;
    if(sscanf(s,"%ld:%2ld:%2ld%c%3ld",&h,&m,&sec,&sep,&ms)>=5&&(sep==','||sep=='.')) return ((h*3600+m*60+sec)*1000)+ms;
    if(sscanf(s,"%ld:%2ld%n",&m,&sec,&nn)>=2 && s[nn]==0) return (m*60+sec)*1000;
    char*end; double f=strtod(s,&end); if(end!=s&&*end==0) return (long)py_round(f*1000); return -1; }
static const char* ext_for(const char*fmt){ if(!strcmp(fmt,"vtt"))return"vtt"; if(!strcmp(fmt,"ass"))return"ass"; if(!strcmp(fmt,"microdvd"))return"sub"; return"srt"; }

static int cmd_convert(const char*in,const char*out,const char*movie,double flag_fps,const char*fmt_flag,SanOpts opt){
    size_t n; char*data=read_input(in,&n); if(!data){ fprintf(stderr,"Brak pliku: %s\n",in); return 1; }
    double fps=resolve_fps(movie,0,flag_fps);
    char*outp=default_out(in,out); const char*fmt=fmt_from_ext(outp,fmt_flag);
    SB o; sb_init(&o); SanReport rep; convert_bytes((unsigned char*)data,n,fps,fmt,&opt,&o,&rep); free(data);
    write_file(outp,o.b,o.len);
    if(!strcmp(fmt,"srt")) print_saved(outp,o.len,&rep);
    else printf("Zapisano: %s (%d linii, format %s)\n",outp,rep.total,fmt);
    free(outp); free(o.b); return 0;
}
static int cmd_download(const char*movie,const char*lang,const char*out,double flag_fps,SanOpts opt){
    char md[33]; if(md5_10mb(movie,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); return 1; }
    char L[8]; snprintf(L,sizeof L,"%s",lang?lang:"PL"); for(char*p=L;*p;p++)*p=toupper((unsigned char)*p);
    size_t dl; unsigned char*sub=np_download(md,L,&dl);
    if(!sub){ printf("Brak napisów dla: %s\n",movie); return 1; }
    double sfps=np_file_info_fps(md); double fps=resolve_fps(movie,sfps,flag_fps);
    SB o; sb_init(&o); SanReport rep; convert_bytes(sub,dl,fps,"srt",&opt,&o,&rep); free(sub);
    char*outp=default_out(movie,out); write_file(outp,o.b,o.len);
    print_saved(outp,o.len,&rep);
    free(outp); free(o.b); return 0;
}
static int cmd_n24_getid(const char*id,const char*out,const char*movie,double flag_fps,SanOpts opt){
    size_t dl; unsigned char*sub=n24_download_id(id,&dl);
    if(!sub){ fprintf(stderr,"Nie znaleziono: Serwer nie zwrócił ZIP dla napisId=%s\n",id); return 1; }
    double fps=resolve_fps(movie,0,flag_fps);
    SB o; sb_init(&o); SanReport rep; convert_bytes(sub,dl,fps,"srt",&opt,&o,&rep); free(sub);
    char*outp=out?strdup(out):strdup("napisy.srt"); write_file(outp,o.b,o.len); print_saved(outp,o.len,&rep);
    free(outp); free(o.b); return 0;
}
static int cmd_n24_download(const char*movie,const char*lang,const char*out,double flag_fps,SanOpts opt){
    char L[8]; snprintf(L,sizeof L,"%s",lang?lang:"pl"); for(char*p=L;*p;p++)*p=toupper((unsigned char)*p);
    size_t dl; unsigned char*sub=n24_checksub_agent(movie,L,&dl);
    if(!sub){ printf("Brak napisów dla: %s\n",movie); return 1; }
    double fps=resolve_fps(movie,0,flag_fps);
    SB o; sb_init(&o); SanReport rep; convert_bytes(sub,dl,fps,"srt",&opt,&o,&rep); free(sub);
    char*outp=default_out(movie,out); write_file(outp,o.b,o.len); print_saved(outp,o.len,&rep);
    free(outp); free(o.b); return 0;
}
static int cmd_np_fileinfo(const char*movie){ char md[33]; if(md5_10mb(movie,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); return 1; }
    double f=np_file_info_fps(md); if(f>0) printf("FPS (serwer): %g\n",f); else printf("Brak danych FPS\n"); return 0; }
static int cmd_get(const char*movie,const char*lang,const char*out,double flag_fps,SanOpts opt){
    char L[8]; snprintf(L,sizeof L,"%s",lang?lang:"pl"); for(char*p=L;*p;p++)*p=toupper((unsigned char)*p);
    double fps=resolve_fps(movie,0,flag_fps); char*outp=default_out(movie,out);
    /* napiprojekt */ char md[33]; if(md5_10mb(movie,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); free(outp); return 1; }
    size_t dl; unsigned char*sub=np_download(md,L,&dl);
    if(sub){ SB o; sb_init(&o); SanReport rep; convert_bytes(sub,dl,fps,"srt",&opt,&o,&rep); free(sub); write_file(outp,o.b,o.len); print_saved(outp,o.len,&rep); printf("Źródło: napiprojekt\n"); free(outp); free(o.b); return 0; }
    /* napisy24 */ unsigned char*s2=n24_checksub_agent(movie,L,&dl);
    if(s2){ SB o; sb_init(&o); SanReport rep; convert_bytes(s2,dl,fps,"srt",&opt,&o,&rep); free(s2); write_file(outp,o.b,o.len); print_saved(outp,o.len,&rep); printf("Źródło: napisy24\n"); free(outp); free(o.b); return 0; }
    printf("Nie znaleziono napisów. Szczegóły:\n  - napiprojekt: napiprojekt nie ma napisów dla tego pliku\n  - napisy24: brak trafień\n  - opensubtitles: wymaga wersji Python (TLS)\n");
    free(outp); return 1;
}
static int cmd_fpsconv(const char*in,const char*out,double from_fps,double to_fps,const char*movie,const char*fmt_flag){
    if(to_fps<=0 && movie){ double f=trusted_fps(fps_from_file(movie)); if(f) to_fps=f; }
    if(from_fps<=0||to_fps<=0) die("Podaj --from ORAZ --to (albo --to przez --movie).");
    Cues cues; load_cues_c(in,from_fps,&cues);
    double scale=from_fps/to_fps; Cues conv; apply_sync_c(&cues,scale,0,&conv);
    char defname[512]; char*outp; if(out){ outp=xmalloc(strlen(out)+1); strcpy(outp,out); }
    else { char*st=input_stem(in); snprintf(defname,sizeof defname,"%s.%gfps.srt",st,to_fps); free(st); outp=xmalloc(strlen(defname)+1); strcpy(outp,defname); }
    const char*fmt=fmt_from_ext(outp,fmt_flag); SB o; sb_init(&o); emit_subtitle(&conv,fmt,to_fps,&o); write_file(outp,o.b,o.len);
    printf("Przeliczono FPS %g -> %g (scale=%.5f): %s (%d linii)\n",from_fps,to_fps,scale,outp,conv.n);
    free(outp); free(o.b); return 0;
}
static int cmd_merge(char**files,int nfiles,const char*out,double flag_fps,const char*fmt_flag,double*offs,int noff){
    if(nfiles<2) die("Podaj co najmniej 2 pliki do połączenia.");
    double fps=flag_fps>0?flag_fps:DEFAULT_FPS;
    Cues merged; load_cues_c(files[0],fps,&merged);
    long running_end=0; for(int i=0;i<merged.n;i++) if(merged.a[i].end>running_end) running_end=merged.a[i].end;
    for(int i=1;i<nfiles;i++){ Cues c; load_cues_c(files[i],fps,&c); long shift=(i-1<noff)?(long)py_round(offs[i-1]*1000):running_end;
        Cues sh; apply_sync_c(&c,1.0,shift,&sh); for(int k=0;k<sh.n;k++){ Cue*q=cues_push(&merged); *q=sh.a[k]; if(q->end>running_end) running_end=q->end; } }
    char defname[512]; char*outp; if(out){ outp=xmalloc(strlen(out)+1); strcpy(outp,out); }
    else { char*st=input_stem(files[0]); snprintf(defname,sizeof defname,"%s.merged.srt",st); free(st); outp=xmalloc(strlen(defname)+1); strcpy(outp,defname); }
    const char*fmt=fmt_from_ext(outp,fmt_flag); SB o; sb_init(&o); emit_subtitle(&merged,fmt,fps,&o); write_file(outp,o.b,o.len);
    printf("Połączono %d plików → %s (%d linii, format %s)\n",nfiles,outp,merged.n,fmt);
    free(outp); free(o.b); return 0;
}
static int cmd_split(const char*in,const char*out,char**at,int nat,int rebase,double flag_fps,const char*fmt_flag){
    if(nat<1) die("Podaj co najmniej jeden --at.");
    double fps=flag_fps>0?flag_fps:DEFAULT_FPS; Cues cues; load_cues_c(in,fps,&cues);
    long pts[64]; int npts=0; for(int i=0;i<nat&&i<64;i++){ long v=parse_user_time(at[i]); if(v<0){ char m[256]; snprintf(m,sizeof m,"Zły format --at '%s' (użyj hh:mm:ss,mmm lub sekund)",at[i]); die(m);} pts[npts++]=v; }
    for(int i=0;i<npts;i++) for(int j=i+1;j<npts;j++) if(pts[j]<pts[i]){ long t=pts[i];pts[i]=pts[j];pts[j]=t; }
    const char*fmt=fmt_flag?fmt_flag:"srt"; const char*ext=ext_for(fmt);
    char base[512]; if(out) snprintf(base,sizeof base,"%s",out); else { char*st=input_stem(in); snprintf(base,sizeof base,"%s",st); free(st); }
    int nparts=npts+1; SB names; sb_init(&names); int wrote=0;
    for(int p=0;p<nparts;p++){ Cues part; cues_init(&part);
        for(int i=0;i<cues.n;i++){ int idx=0; for(int k=0;k<npts;k++) if(cues.a[i].start>=pts[k]) idx=k+1; if(idx==p){ Cue*q=cues_push(&part); q->start=cues.a[i].start; q->end=cues.a[i].end; for(int k=0;k<cues.a[i].nlines;k++) cue_addline(q,cues.a[i].lines[k],strlen(cues.a[i].lines[k])); } }
        if(part.n==0) continue; long origin=(p>0)?pts[p-1]:0; Cues seg;
        if(rebase&&p>0) apply_sync_c(&part,1.0,-origin,&seg); else seg=part;
        char pth[600]; snprintf(pth,sizeof pth,"%s.part%d.%s",base,p+1,ext); SB o; sb_init(&o); emit_subtitle(&seg,fmt,fps,&o); write_file(pth,o.b,o.len); free(o.b);
        if(wrote) sb_puts(&names,", "); sb_puts(&names,pth); wrote++; }
    if(!wrote){ printf("Brak linii do zapisania.\n"); return 1; }
    printf("Podzielono na: %s%s\n",names.b, rebase?"  (czasy części wyzerowane)":""); free(names.b); return 0;
}

/* --- interaktywny sync (termios + ANSI, w obu buildach) --- */
static int aq_readkey(void){ unsigned char c; if(read(0,&c,1)!=1) return -1; if(c!=0x1b) return c;
    unsigned char b; if(read(0,&b,1)!=1) return 27; if(b!='['&&b!='O') return 27;
    unsigned char d; if(read(0,&d,1)!=1) return 27;
    switch(d){ case 'A':return 1000; case 'B':return 1001; case 'C':return 1002; case 'D':return 1003; case 'H':return 1006; case 'F':return 1007;
        case '5':{unsigned char e; if(read(0,&e,1)!=1){} return 1004;} case '6':{unsigned char e; if(read(0,&e,1)!=1){} return 1005;} default:return 27; } }
static void tui_line(SB*o,int row,int x,int colw,int rev,const char*s){ char h[700]; snprintf(h,sizeof h,"\033[%d;%dH%s%-*.*s\033[0m",row,x,rev?"\033[7m":"",colw,colw,s); sb_puts(o,h); }
static int sync_tui(Cues*ref,Cues*tgt,Cues*wout,double*scale,double*offset){
    if(!isatty(0)||!isatty(1)) return -2;
    struct termios old,raw; tcgetattr(0,&old); raw=old; raw.c_lflag&=~(ICANON|ECHO); raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0; tcsetattr(0,TCSANOW,&raw);
    Cues work; cues_init(&work); for(int i=0;i<tgt->n;i++){ Cue*q=cues_push(&work); q->start=tgt->a[i].start; q->end=tgt->a[i].end; for(int k=0;k<tgt->a[i].nlines;k++) cue_addline(q,tgt->a[i].lines[k],strlen(tgt->a[i].lines[k])); }
    Cues*col[2]; col[0]=ref; col[1]=&work; const char*ttl[2]={"WZOR (referencja)","DO SYNCHRONIZACJI"};
    int active=0,cursor[2]={0,0},top[2]={0,0},sel[2]={-1,-1}, li[512],ri[512],np=0, done=0,apply=0; char msg[120]="";
    while(!done){
        struct winsize ws; memset(&ws,0,sizeof ws); ioctl(1,TIOCGWINSZ,&ws);
        int rows=ws.ws_row>0?ws.ws_row:24, cols=ws.ws_col>0?ws.ws_col:80, colw=(cols-3)/2; if(colw<12)colw=12; int body=rows-6; if(body<1)body=1;
        for(int c=0;c<2;c++){ if(col[c]->n==0)cursor[c]=0; else { if(cursor[c]<0)cursor[c]=0; if(cursor[c]>=col[c]->n)cursor[c]=col[c]->n-1; } if(cursor[c]<top[c])top[c]=cursor[c]; if(cursor[c]>=top[c]+body)top[c]=cursor[c]-body+1; }
        SB o; sb_init(&o); sb_puts(&o,"\033[2J\033[H");
        tui_line(&o,1,1,colw,active==0,ttl[0]); tui_line(&o,1,colw+4,colw,active==1,ttl[1]);
        for(int c=0;c<2;c++){ int x=(c==0)?1:colw+4; for(int row=0;row<body;row++){ int idx=top[c]+row; if(idx>=col[c]->n) break; Cue*cu=&col[c]->a[idx];
            int pn=0; for(int p=0;p<np;p++){ if(c==0&&li[p]==idx)pn=p+1; if(c==1&&ri[p]==idx)pn=p+1; }
            char mk=pn?('0'+pn%10):(sel[c]==idx?'>':' '); char tim[16]; ms_to_srt(cu->start,tim); tim[8]=0;
            char txt[420]=""; for(int k=0;k<cu->nlines;k++){ if(k)strncat(txt," ",sizeof txt-strlen(txt)-1); strncat(txt,cu->lines[k],sizeof txt-strlen(txt)-1); }
            char line[480]; snprintf(line,sizeof line,"%c%4d %s %s",mk,idx+1,tim,txt);
            tui_line(&o,2+row,x,colw,(active==c&&cursor[c]==idx),line); } }
        /* podgląd + status + pomoc */
        for(int c=0;c<2;c++){ int idx=sel[c]>=0?sel[c]:cursor[c]; char pv[700]="-"; if(idx>=0&&idx<col[c]->n){ Cue*cu=&col[c]->a[idx]; char t1[16],t2[16]; ms_to_srt(cu->start,t1); ms_to_srt(cu->end,t2);
            char txt[420]=""; for(int k=0;k<cu->nlines;k++){ if(k)strncat(txt," | ",sizeof txt-strlen(txt)-1); strncat(txt,cu->lines[k],sizeof txt-strlen(txt)-1); }
            snprintf(pv,sizeof pv,"%s [%d] %s->%s  %s",c==0?"WZOR:":"CEL :",idx+1,t1,t2,txt); }
            char h[760]; snprintf(h,sizeof h,"\033[%d;1H\033[2K\033[2m%.*s\033[0m",rows-3+c,cols-1,pv); sb_puts(&o,h); }
        { long T[512],R[512]; int m=0; for(int p=0;p<np;p++){ T[m]=work.a[ri[p]].start; R[m]=ref->a[li[p]].start; m++; } double sc,of; sync_transform(T,R,m,&sc,&of);
          char st[300]; snprintf(st,sizeof st,"\033[%d;1H\033[2K\033[7m par: %d   scale=%.4f  offset=%+.3fs   %s\033[0m",rows-1,np,sc,of/1000.0,msg); sb_puts(&o,st); }
        { char hp[400]; snprintf(hp,sizeof hp,"\033[%d;1H\033[2KTAB kol | up/down ruch | ENTER zaznacz->laczy | u cofnij | CEL: ,/. +-0.1s  </> +-1s  e czas | a zapis | q wyjscie",rows); sb_puts(&o,hp); }
        if(write(1,o.b,o.len)<0){} free(o.b); msg[0]=0;
        int k=aq_readkey();
        if(k==-1||k=='q'||k==27) done=1;
        else if(k==9) active^=1;
        else if(k==1000||k=='k') cursor[active]--;
        else if(k==1001||k=='j') cursor[active]++;
        else if(k==1004) cursor[active]-=body;
        else if(k==1005) cursor[active]+=body;
        else if(k==1006) cursor[active]=0;
        else if(k==1007) cursor[active]=col[active]->n-1;
        else if(k==13||k==10||k==' '){ sel[active]=cursor[active]; if(sel[0]>=0&&sel[1]>=0&&np<512){ li[np]=sel[0]; ri[np]=sel[1]; np++; sel[0]=sel[1]=-1; } }
        else if(k=='u'){ if(np>0)np--; }
        else if(k=='a'){ apply=1; done=1; }
        else if(active==1 && (k==','||k=='.'||k=='<'||k=='>'||k=='e') && work.n>0){ Cue*cu=&work.a[cursor[1]]; long d=0;
            if(k==',')d=-100; else if(k=='.')d=100; else if(k=='<')d=-1000; else if(k=='>')d=1000;
            else { tcsetattr(0,TCSANOW,&old); struct winsize w2; ioctl(1,TIOCGWINSZ,&w2); printf("\033[%d;1H\033[2KNowy czas startu (hh:mm:ss,mmm | sek): ",w2.ws_row>0?w2.ws_row:24); fflush(stdout);
                char buf[64]; if(fgets(buf,sizeof buf,stdin)){ long v=parse_user_time(buf); if(v>=0) d=v-cu->start; } tcsetattr(0,TCSANOW,&raw); }
            cu->start+=d; if(cu->start<0)cu->start=0; cu->end+=d; if(cu->end<0)cu->end=0; } }
    tcsetattr(0,TCSANOW,&old); if(write(1,"\033[2J\033[H",7)<0){}
    if(!apply) return -1;
    long T[512],R[512]; int m=0; for(int p=0;p<np;p++){ T[m]=work.a[ri[p]].start; R[m]=ref->a[li[p]].start; m++; } sync_transform(T,R,m,scale,offset); *wout=work; return 0;
}

static int cmd_sync(const char*ref,const char*tgt,const char*out,double off_sec,int has_off,char**anch,int nanch,double flag_fps){
    if(!ref||!tgt) die("sync wymaga dwóch plików: WZÓR CEL");
    double fps=flag_fps>0?flag_fps:DEFAULT_FPS;
    Cues rc,tc; load_cues_c(ref,fps,&rc); load_cues_c(tgt,fps,&tc);
    double scale,offset;
    if(has_off){ scale=1; offset=off_sec*1000.0; }
    else if(nanch>0){ long T[64],R[64]; int np=0;
        for(int i=0;i<nanch&&np<64;i++){ long ri,ti; char sep; if(sscanf(anch[i],"%ld%c%ld",&ri,&sep,&ti)<3||(sep!=','&&sep!=':')){ char m[128]; snprintf(m,sizeof m,"Zły format --anchor '%s' (użyj R,T)",anch[i]); die(m);}
            if(ri<1||ri>rc.n||ti<1||ti>tc.n){ char m[128]; snprintf(m,sizeof m,"--anchor %s: numer linii poza zakresem",anch[i]); die(m);}
            T[np]=tc.a[ti-1].start; R[np]=rc.a[ri-1].start; np++; }
        sync_transform(T,R,np,&scale,&offset);
    } else { Cues work; double sc,of; int r=sync_tui(&rc,&tc,&work,&sc,&of);
        if(r==-2) die("Interaktywny sync wymaga terminala — użyj --offset SEK lub --anchor R,T.");
        if(r!=0){ printf("Anulowano — nic nie zapisano.\n"); return 1; }
        tc=work; scale=sc; offset=of; }
    Cues syn; apply_sync_c(&tc,scale,offset,&syn);
    char defname[512]; char*outp; if(out){ outp=xmalloc(strlen(out)+1); strcpy(outp,out); }
    else { char*st=input_stem(tgt); snprintf(defname,sizeof defname,"%s.synced.srt",st); free(st); outp=xmalloc(strlen(defname)+1); strcpy(outp,defname); }
    SB o; sb_init(&o); emit_srt(&syn,&o); write_file(outp,o.b,o.len);
    printf("Zsynchronizowano: %s (%d linii)\n",outp,syn.n);
    printf("  transformacja: nowy = %.5f * stary + (%+.3f s)\n",scale,offset/1000.0);
    free(outp); free(o.b); return 0;
}

/* ---------------------------------------------------------------- config */
static const char* config_path(const char*ov){ if(ov) return ov; static char buf[512]; const char*h=getenv("HOME"); snprintf(buf,sizeof buf,"%s/.config/aqnapi/config.ini",h?h:"."); return buf; }
static int key_is_secret(const char*k){ return !strcmp(k,"pass")||!strcmp(k,"password"); }
#include <sys/stat.h>
#include <termios.h>
static char* prompt_line(const char*label,int secret){
    fputs(label,stdout); fflush(stdout);
    struct termios old,neu; int istty=isatty(0);
    if(secret&&istty){ tcgetattr(0,&old); neu=old; neu.c_lflag&=~ECHO; tcsetattr(0,TCSANOW,&neu); }
    char buf[256]; if(!fgets(buf,sizeof buf,stdin)) buf[0]=0;
    if(secret&&istty){ tcsetattr(0,TCSANOW,&old); fputc('\n',stdout); }
    size_t n=strlen(buf); while(n&&(buf[n-1]=='\n'||buf[n-1]=='\r')) buf[--n]=0;
    char*d=xmalloc(n+1); memcpy(d,buf,n+1); return d;
}
/* prosta reprezentacja ini z zachowaniem kolejności */
typedef struct { char sec[3][16]; char key[3][8][24]; char val[3][8][256]; int nkeys[3]; } Ini;
static void ini_load(const char*path,Ini*ini){ memset(ini,0,sizeof *ini);
    strcpy(ini->sec[0],"napisy24"); strcpy(ini->sec[1],"napiprojekt"); strcpy(ini->sec[2],"opensubtitles");
    FILE*f=fopen(path,"r"); if(!f) return; char line[512]; int cur=-1;
    while(fgets(line,sizeof line,f)){ char*s=line; while(*s==' ')s++; size_t n=strlen(s); while(n&&(s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '))s[--n]=0;
        if(s[0]=='['){ char sec[32]; snprintf(sec,sizeof sec,"%.*s",(int)(strlen(s)-2),s+1); cur=-1; for(int i=0;i<3;i++) if(!strcmp(sec,ini->sec[i])) cur=i; continue; }
        if(cur<0||!s[0]) continue; char*eq=strchr(s,'='); if(!eq) continue; *eq=0; char*k=s,*v=eq+1; while(*v==' ')v++;
        { size_t kn=strlen(k); while(kn&&k[kn-1]==' ')k[--kn]=0; } for(char*p=k;*p;p++)*p=tolower((unsigned char)*p);
        int idx=ini->nkeys[cur]; if(idx<8){ snprintf(ini->key[cur][idx],24,"%s",k); snprintf(ini->val[cur][idx],256,"%s",v); ini->nkeys[cur]++; } }
    fclose(f);
}
static void ini_set(Ini*ini,int sec,const char*k,const char*v){ for(int i=0;i<ini->nkeys[sec];i++) if(!strcmp(ini->key[sec][i],k)){ snprintf(ini->val[sec][i],256,"%s",v); return; }
    int idx=ini->nkeys[sec]; if(idx<8){ snprintf(ini->key[sec][idx],24,"%s",k); snprintf(ini->val[sec][idx],256,"%s",v); ini->nkeys[sec]++; } }
static const char* ini_get(Ini*ini,int sec,const char*k){ for(int i=0;i<ini->nkeys[sec];i++) if(!strcmp(ini->key[sec][i],k)) return ini->val[sec][i]; return ""; }
static int cmd_config(const char*sub,const char*ov){
    const char*path=config_path(ov);
    if(!strcmp(sub,"path")){ printf("%s\n",path); return 0; }
    Ini ini; ini_load(path,&ini);
    if(!strcmp(sub,"show")){ int any=0; for(int i=0;i<3;i++) if(ini.nkeys[i])any=1;
        if(!any){ printf("(pusty lub brak pliku: %s)\n",path); return 0; }
        for(int i=0;i<3;i++){ if(!ini.nkeys[i]) continue; printf("[%s]\n",ini.sec[i]);
            for(int k=0;k<ini.nkeys[i];k++){ const char*v=ini.val[i][k]; if(key_is_secret(ini.key[i][k])&&v[0]){ printf("  %s = ",ini.key[i][k]); for(size_t z=0;z<strlen(v);z++)putchar('*'); putchar('\n'); } else printf("  %s = %s\n",ini.key[i][k],v); } }
        return 0; }
    if(!strcmp(sub,"init")){
        printf("Konfiguracja aqnapi — Enter zostawia obecną wartość.\n\n");
        struct { int sec; const char*key; const char*prompt; int secret; } q[]={
            {0,"login","Napisy24 login/e-mail",0},{0,"pass","Napisy24 hasło",1},
            {1,"user","napiprojekt login",0},{1,"pass","napiprojekt hasło",1},
            {2,"api_key","OpenSubtitles API key",0},{2,"username","OpenSubtitles login",0},{2,"password","OpenSubtitles hasło",1}};
        for(int i=0;i<7;i++){ const char*cur=ini_get(&ini,q[i].sec,q[i].key); char lbl[128];
            if(q[i].secret) snprintf(lbl,sizeof lbl,"%s%s: ",q[i].prompt,cur[0]?" [Enter=bez zmian]":"");
            else snprintf(lbl,sizeof lbl,"%s%s%s%s: ",q[i].prompt,cur[0]?" [":"",cur,cur[0]?"]":"");
            char*v=prompt_line(lbl,q[i].secret); if(v[0]) ini_set(&ini,q[i].sec,q[i].key,v); else if(cur[0]) ini_set(&ini,q[i].sec,q[i].key,cur); free(v); }
        /* utwórz katalog */ char dir[512]; snprintf(dir,sizeof dir,"%s",path); char*sl=strrchr(dir,'/'); if(sl){ *sl=0; char cmd[600]; snprintf(cmd,sizeof cmd,"mkdir -p '%s'",dir); if(system(cmd)!=0){} }
        FILE*f=fopen(path,"w"); if(!f) die("nie mogę zapisać konfiguracji");
        for(int i=0;i<3;i++){ fprintf(f,"[%s]\n",ini.sec[i]); for(int k=0;k<ini.nkeys[i];k++) fprintf(f,"%s = %s\n",ini.key[i][k],ini.val[i][k]); fprintf(f,"\n"); }
        fclose(f); chmod(path,0600);
        printf("\nZapisano: %s (uprawnienia 600)\n",path); return 0;
    }
    fprintf(stderr,"config: użyj init | show | path\n"); return 2;
}

/* ---------------------------------------------------------------- wyszukiwanie */
static char* xml_first(const char*hay,const char*tag){ char op[48],cl[48]; snprintf(op,sizeof op,"<%s>",tag); snprintf(cl,sizeof cl,"</%s>",tag);
    const char*s=strstr(hay,op); if(!s){ char*e=xmalloc(1); e[0]=0; return e; } s+=strlen(op); const char*e=strstr(s,cl); if(!e){ char*x=xmalloc(1); x[0]=0; return x; }
    /* zdejmij CDATA (jak ElementTree) */ int cdata=0; const char*cd=strstr(s,"<![CDATA[");
    if(cd && cd<e){ const char*ce=strstr(cd+9,"]]>"); if(ce && ce<e){ s=cd+9; e=ce; cdata=1; } }
    size_t n=(size_t)(e-s); char*o=xmalloc(n+1); memcpy(o,s,n); o[n]=0; if(!cdata) html_unescape(o); strip_inplace(o); return o; }
/* Iteruj elementy XML w kolejności dokumentu. Zwraca 1 i ustawia tag + inner
 * (malloc), przesuwa *pp za </tag>. Pomija <?..?>, <!..>, </..>. */
static int xml_next(const char**pp,char*tag,size_t tagsz,char**inner){
    const char*p=*pp;
    for(;;){ while(*p && *p!='<') p++; if(!*p) return 0;
        if(p[1]=='?'||p[1]=='!'||p[1]=='/'){ const char*gt=strchr(p,'>'); if(!gt) return 0; p=gt+1; continue; } break; }
    const char*ts=p+1,*te=ts; while(*te && *te!='>' && *te!='/' && !is_ascii_ws(*te)) te++;
    size_t tn=(size_t)(te-ts); if(tn>=tagsz) tn=tagsz-1; memcpy(tag,ts,tn); tag[tn]=0;
    const char*ot=strchr(p,'>'); if(!ot) return 0;
    if(ot>p && ot[-1]=='/'){ char*z=xmalloc(1); z[0]=0; *inner=z; *pp=ot+1; return 1; }
    char close[80]; snprintf(close,sizeof close,"</%s>",tag);
    const char*ce=strstr(ot+1,close);
    if(!ce){ char*z=xmalloc(1); z[0]=0; *inner=z; *pp=ot+1; return 1; }
    size_t il=(size_t)(ce-(ot+1)); char*in=xmalloc(il+1); memcpy(in,ot+1,il); in[il]=0; *inner=in;
    *pp=ce+strlen(close); return 1; }
/* Pole „wydanie/release" wg zasad Napisy24: pomiń tytuł/rok/SxxExx z początku
 * oraz tagi trackerów [..]{..}(..). Odpowiednik n24_release() w Pythonie. */
static void n24_release(const char*name,char*out,size_t osz){
    char s[512]; snprintf(s,sizeof s,"%s",name?name:"");
    size_t L=strlen(s);
    static const char*exts[]={".mkv",".mp4",".avi",".mov",".m4v",".ts"};
    for(int i=0;i<6;i++){ size_t el=strlen(exts[i]); if(L>=el && !strcasecmp(s+L-el,exts[i])){ s[L-el]=0; L-=el; break; } }
    /* SxxExx (case-insensitive) — bierz to co po odcinku; wpp. po roku 19xx/20xx */
    long cut=-1;
    for(size_t i=0;i+3<L;i++){ if((s[i]=='S'||s[i]=='s')&&isdigit((unsigned char)s[i+1])){ size_t j=i+1; while(j<L&&isdigit((unsigned char)s[j])&&j-i<=2)j++;
        if(j<L&&(s[j]=='E'||s[j]=='e')&&isdigit((unsigned char)s[j+1])){ size_t k=j+1; while(k<L&&isdigit((unsigned char)s[k])&&k-j<=3)k++; cut=(long)k; break; } } }
    if(cut<0){ for(size_t i=0;i+4<=L;i++){ int sep0=(i==0)||s[i-1]=='.'||s[i-1]==' '||s[i-1]=='_'||s[i-1]=='-';
        if(sep0 && (s[i]=='1'||s[i]=='2') && (s[i+1]=='9'||s[i+1]=='0') && isdigit((unsigned char)s[i+2]) && isdigit((unsigned char)s[i+3])){
            char nx=(i+4<L)?s[i+4]:'.'; if(nx=='.'||nx==' '||nx=='_'||nx=='-'){ cut=(long)(i+4); break; } } } }
    char *p = (cut>=0)? s+cut : s;
    /* usuń tagi w [] {} () i przepisz resztę */
    char tmp[512]; size_t o=0; int depth=0;
    for(char*q=p;*q&&o<sizeof tmp-1;q++){ if(*q=='['||*q=='{'||*q=='('){depth++;continue;} if(*q==']'||*q=='}'||*q==')'){ if(depth)depth--; continue;} if(!depth) tmp[o++]=*q; }
    tmp[o]=0;
    /* trim wiodących/końcowych separatorów; collapse ".." */
    size_t a=0; while(tmp[a]=='.'||tmp[a]==' '||tmp[a]=='_'||tmp[a]=='-')a++;
    size_t b=strlen(tmp); while(b>a&&(tmp[b-1]=='.'||tmp[b-1]==' '||tmp[b-1]=='_'||tmp[b-1]=='-'))b--;
    char res[512]; size_t ro=0; int prevdot=0;
    for(size_t i=a;i<b&&ro<sizeof res-1;i++){ if(tmp[i]=='.'){ if(prevdot)continue; prevdot=1; } else prevdot=0; res[ro++]=tmp[i]; }
    res[ro]=0; snprintf(out,osz,"%s",res);
}
static void norm_imdb(const char*in,char*out,size_t osz){ char dig[32]; int k=0; for(const char*p=in;*p&&k<31;p++) if(isdigit((unsigned char)*p)) dig[k++]=*p; dig[k]=0;
    if(k==0){ snprintf(out,osz,"%s",in); return; } char z[8]=""; int pad=7-k; for(int i=0;i<pad&&i<7;i++) z[i]='0'; z[pad>0?pad:0]=0; snprintf(out,osz,"tt%s%s",z,dig); }
static void extract_tt(const char*s,char*out,size_t osz){ out[0]=0; /* znajdź "tt" po którym są cyfry (jak regex tt\d+) */
    for(const char*p=strstr(s,"tt"); p; p=strstr(p+1,"tt")){ if(isdigit((unsigned char)p[2])){ size_t i=0; out[i++]='t'; out[i++]='t'; const char*q=p+2; while(*q&&isdigit((unsigned char)*q)&&i<osz-1) out[i++]=*q++; out[i]=0; return; } } }
static void print_hits_header(void){ printf("%-13s %-12s ","SERWIS","ID"); fputs("JĘZYK  ",stdout); printf("%6s  TYTUŁ / RELEASE\n","POB."); for(int i=0;i<78;i++)putchar('-'); putchar('\n'); }
static void print_hit(const char*svc,const char*id,const char*lang,int dls,const char*title,const char*year,const char*release){
    SB t; sb_init(&t); sb_puts(&t,title); if(year&&year[0]){ char y[32]; snprintf(y,sizeof y," (%s)",year); sb_puts(&t,y); }
    if(release&&release[0]){ char r[80]; snprintf(r,sizeof r,"  [%.32s]",release); sb_puts(&t,r); }
    if(t.len>60) t.b[60]=0; printf("%-13s %-12s %-6s %6d  %s\n",svc,id,lang,dls,t.b); free(t.b); }
typedef struct { char service[16],id[48],lang[8],title[256],year[16],release[160]; int dls; } Hit;
typedef struct { Hit*a; int n,cap; } Hits;
static Hit* hits_push(Hits*h){ if(h->n==h->cap){ h->cap=h->cap?h->cap*2:16; h->a=xrealloc(h->a,h->cap*sizeof(Hit)); } Hit*x=&h->a[h->n++]; memset(x,0,sizeof *x); return x; }
static void n24_collect(Hits*hits,const char*imdb,const char*title){
    char path[600]; if(imdb&&imdb[0]){ char nb[16]; norm_imdb(imdb,nb,sizeof nb); char*e=url_encode(nb); snprintf(path,sizeof path,"/libs/webapi.php?imdb=%s",e); free(e); }
    else { char*e=url_encode(title?title:""); snprintf(path,sizeof path,"/libs/webapi.php?title=%s",e); free(e); }
    size_t bl; char*body=http_get_url("napisy24.pl",path,&bl); if(!body) return; const char*p=body;
    while((p=strstr(p,"<subtitle>"))){ const char*end=strstr(p,"</subtitle>"); if(!end) break;
        size_t blen=end-p; char*blk=xmalloc(blen+1); memcpy(blk,p,blen); blk[blen]=0;
        char*id=xml_first(blk,"id"),*t=xml_first(blk,"title"),*alt=xml_first(blk,"altTitle"),*yr=xml_first(blk,"year"),*lg=xml_first(blk,"language"),*rel=xml_first(blk,"release");
        Hit*x=hits_push(hits); snprintf(x->service,16,"napisy24"); snprintf(x->id,48,"%s",id); snprintf(x->lang,8,"%s",lg);
        snprintf(x->title,256,"%s",t[0]?t:alt); snprintf(x->year,16,"%s",yr); snprintf(x->release,160,"%s",rel);
        free(id);free(t);free(alt);free(yr);free(lg);free(rel);free(blk); p=end+11; }
    free(body);
}
static void np_collect(Hits*hits,const char*title){
    char*e=url_encode(title?title:""); char path[600]; snprintf(path,sizeof path,"/api/api-movie-search.php?mode=get&client=allplayer&search=%s",e); free(e);
    size_t bl; char*body=http_get_url("napiprojekt.pl",path,&bl); if(!body) return; const char*p=body;
    while((p=strstr(p,"<movie>"))){ const char*end=strstr(p,"</movie>"); if(!end) break;
        size_t blen=end-p; char*blk=xmalloc(blen+1); memcpy(blk,p,blen); blk[blen]=0;
        char*mid=xml_first(blk,"id"),*po=xml_first(blk,"polish"),*orig=xml_first(blk,"original"),*yr=xml_first(blk,"year");
        Hit*x=hits_push(hits); snprintf(x->service,16,"napiprojekt"); snprintf(x->id,48,"%s",mid);
        snprintf(x->title,256,"%s",orig[0]?orig:po); snprintf(x->year,16,"%s",yr);
        free(mid);free(po);free(orig);free(yr);free(blk); p=end+8; }
    free(body);
}
static int lang_ok(const char*hl,const char*want){ if(!want||!want[0]) return 1; if(!hl[0]) return 1; char h[8]; snprintf(h,sizeof h,"%s",hl); for(char*p=h;*p;p++)*p=tolower((unsigned char)*p);
    char w[64]; snprintf(w,sizeof w,"%s",want); for(char*t=strtok(w,",");t;t=strtok(NULL,",")){ char lw[16]; snprintf(lw,sizeof lw,"%s",t); for(char*p=lw;*p;p++)*p=tolower((unsigned char)*p); if(!strcmp(h,lw)) return 1; } return 0; }
static int print_hits_table(Hits*hits,const char*langf){ int any=0;
    for(int i=0;i<hits->n;i++){ if(!lang_ok(hits->a[i].lang,langf)) continue; if(!any){ print_hits_header(); any=1; }
        print_hit(hits->a[i].service,hits->a[i].id,hits->a[i].lang,0,hits->a[i].title,hits->a[i].year,hits->a[i].release); }
    if(!any){ printf("Brak wyników.\n"); return 1; }
    printf("\nPobierz: aqnapi <serwis> download/getid/download-id <ID>\n"); return 0;
}
static int n24_search(const char*imdb,const char*title){ Hits h={0}; n24_collect(&h,imdb,title); return print_hits_table(&h,NULL); }
static int cmd_search(const char*imdb,const char*title,const char*query,const char*lang){
    Hits h={0}; if(imdb||title) n24_collect(&h,imdb?imdb:"",title?title:"");
    if(title||query) np_collect(&h,title?title:(query?query:""));
    /* OpenSubtitles wymaga TLS+klucza (poza wersją C) — jak Python bez klucza */
    fprintf(stderr,"WARNING: opensubtitles search: OpenSubtitles wymaga klucza API (Api-Key)\n");
    return print_hits_table(&h,lang&&lang[0]?lang:"pl");
}
static int np_search(const char*title){
    char*e=url_encode(title?title:""); char path[600]; snprintf(path,sizeof path,"/api/api-movie-search.php?mode=get&client=allplayer&search=%s",e); free(e);
    size_t bl; char*body=http_get_url("napiprojekt.pl",path,&bl); if(!body) die("napiprojekt: błąd połączenia");
    const char*p=body; int any=0;
    while((p=strstr(p,"<movie>"))){ const char*end=strstr(p,"</movie>"); if(!end) break;
        size_t blen=end-p; char*blk=xmalloc(blen+1); memcpy(blk,p,blen); blk[blen]=0;
        char*mid=xml_first(blk,"id"),*po=xml_first(blk,"polish"),*orig=xml_first(blk,"original"),*yr=xml_first(blk,"year"),*imdb=xml_first(blk,"imdb_com");
        char tt[16]; extract_tt(imdb,tt,sizeof tt);
        printf("MovieId: %s\n",mid); printf("  %s (%s)  PL: %s\n",orig,yr,po); printf("  IMDB: %s   %s\n",tt,imdb);
        any=1; free(mid);free(po);free(orig);free(yr);free(imdb);free(blk); p=end+8; }
    free(body); if(!any){ printf("Brak wyników.\n"); return 1; }
    return 0;
}

static void np_creds(const char*cfgpath,char*user,size_t us,char*pass,size_t ps);  /* def. niżej */
/* rdzeń uploadu napiprojekt: hash + POST + parsowanie. Zwraca komunikat (malloc,
 * semantyka Pythona: warning|error|status|"brak statusu"); *ok ustawia. Zakłada
 * obecny plik movie i (przy authenticate) creds. die() przy błędzie połączenia. */
static char* np_upload_core(const char*movie,const char*text,size_t sl,const char*lang,
                            const char*author,int corrected,const char*comment,int testing,
                            int authenticate,const char*user,const char*pass,int*ok){
    char md[33]; if(md5_10mb(movie,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); exit(1); }
    char L[8]; snprintf(L,sizeof L,"%s",lang?lang:"PL"); for(char*p=L;*p;p++)*p=toupper((unsigned char)*p);
    char*xml=np_upload_http(md,(unsigned char*)text,sl,L,author,corrected,comment,testing,authenticate,user,pass);
    if(!xml) die("napiprojekt: błąd połączenia");
    char*st=xml_first(xml,"status"),*wr=xml_first(xml,"warning"),*er=xml_first(xml,"error");
    for(char*p=st;*p;p++)*p=tolower((unsigned char)*p);
    *ok=!strcmp(st,"uploaded")||!strcmp(st,"success")||!strcmp(st,"ok");
    char*msg=strdup(wr[0]?wr:(er[0]?er:(st[0]?st:"brak statusu")));
    free(st);free(wr);free(er);free(xml); return msg;
}
static int cmd_np_upload(const char*cfgpath,const char*movie,const char*srt,const char*lang,const char*author,int corrected,const char*comment,int testing,int authenticate){
    if(!movie||!srt) die("napiprojekt upload wymaga --movie i --srt");
    char user[128]="",pass[128]="";
    if(authenticate){ np_creds(cfgpath,user,sizeof user,pass,sizeof pass);
        if(!user[0]||!pass[0]){ fprintf(stderr,"Błąd uwierzytelnienia: Upload z logowaniem wymaga loginu i hasła.\n"); return 2; } }
    size_t n; char*raw=read_input(srt,&n); if(!raw){ fprintf(stderr,"Brak pliku: %s\n",srt); return 1; }
    char*text=decode_text((unsigned char*)raw,n); free(raw); size_t sl=strlen(text);
    int ok; char*msg=np_upload_core(movie,text,sl,lang,author,corrected,comment,testing,authenticate,user[0]?user:NULL,pass[0]?pass:NULL,&ok);
    free(text); printf("[%s] %s\n", ok?"OK":"BŁĄD", msg); free(msg); return ok?0:1;
}

/* creds napiprojekt: config [napiprojekt] user/pass + env NAPI_USER/NAPI_PASS */
static void np_creds(const char*cfgpath,char*user,size_t us,char*pass,size_t ps){
    Ini ini; ini_load(config_path(cfgpath),&ini);
    snprintf(user,us,"%s",ini_get(&ini,1,"user")); snprintf(pass,ps,"%s",ini_get(&ini,1,"pass"));
    const char*eu=getenv("NAPI_USER"),*ep=getenv("NAPI_PASS");
    if(eu&&*eu)snprintf(user,us,"%s",eu); if(ep&&*ep)snprintf(pass,ps,"%s",ep);
}
/* napiprojekt account: api_user_account.php (GET, hasło jawne) -> parent.child: value */
static int cmd_np_account(const char*cfgpath){
    char user[128],pass[128]; np_creds(cfgpath,user,sizeof user,pass,sizeof pass);
    /* jak Python: bez pre-checku pustych creds — serwer i tak zwróci 404 */
    char*eu=url_encode(user),*ep=url_encode(pass);
    char path[400]; snprintf(path,sizeof path,"/api/api_user_account.php?user=%s&pass=%s",eu,ep); free(eu); free(ep);
    size_t bl; char*body=http_get_url("napiprojekt.pl",path,&bl); if(!body) die("napiprojekt: błąd połączenia");
    const char*p=body; char root[64]; char*rin=NULL;
    /* złe creds -> serwer zwraca 404/HTML (nie-XML) -> jak ParseError w Pythonie */
    int haveroot = xml_next(&p,root,sizeof root,&rin);
    if(!haveroot || (strcmp(root,"user_info")!=0 && !strstr(body,"<user_info"))){
        free(rin); free(body); fprintf(stderr,"Błąd uwierzytelnienia: Błędne dane logowania lub zła odpowiedź\n"); return 2; }
    if(strcmp(root,"user_info")!=0){
        free(rin); free(body); fprintf(stderr,"Błąd uwierzytelnienia: Błędne dane logowania (brak danych konta)\n"); return 2; }
    const char*q=rin; char sec[64]; char*sin=NULL;
    while(xml_next(&q,sec,sizeof sec,&sin)){
        const char*r=sin; char key[64]; char*val=NULL;
        while(xml_next(&r,key,sizeof key,&val)){ html_unescape(val); strip_inplace(val); printf("%s.%s: %s\n",sec,key,val); free(val); }
        free(sin); }
    free(rin); free(body); return 0;
}
/* napiprojekt associate: api-movie-associate2.php (GET) — powiąż hasz pliku z id_filmu */
static int cmd_np_associate(const char*cfgpath,const char*movie,const char*movie_id){
    if(!movie||!movie_id){ fprintf(stderr,"napiprojekt associate wymaga <film> <id_filmu>\n"); return 2; }
    /* kolejność jak w Pythonie: najpierw hash pliku, potem kontrola creds */
    char md[33]; if(md5_10mb(movie,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); return 1; }
    char user[128],pass[128]; np_creds(cfgpath,user,sizeof user,pass,sizeof pass);
    if(!user[0]||!pass[0]){ fprintf(stderr,"Błąd uwierzytelnienia: Powiązanie wymaga loginu i hasła\n"); return 2; }
    char*eu=url_encode(user),*ep=url_encode(pass),*ei=url_encode(movie_id);
    char path[500]; snprintf(path,sizeof path,"/api/api-movie-associate2.php?nick=%s&pass=%s&id_pliku=%s&id_filmu=%s",eu,ep,md,ei);
    free(eu); free(ep); free(ei);
    size_t bl; char*body=http_get_url("napiprojekt.pl",path,&bl); if(!body) die("napiprojekt: błąd połączenia");
    char*st=xml_first(body,"status"); for(char*p=st;*p;p++)*p=tolower((unsigned char)*p);
    int ok=!strcmp(st,"success")||!strcmp(st,"ok");
    printf("[%s] %s\n", ok?"OK":"BŁĄD", st[0]?st:"brak statusu");
    free(st); free(body); return ok?0:1;
}
/* generyczny POST modów do api-napiprojekt3.php (client=pynapi). Zwraca XML (malloc) lub NULL. */
static char* np_http_post(const char*const fields[][2], int nf){
    const char*host="www.napiprojekt.pl",*boundary="----aqnapicafe0003"; SB b; sb_init(&b);
    sb_puts(&b,"--");sb_puts(&b,boundary);sb_puts(&b,"\r\nContent-Disposition: form-data; name=\"client\"\r\n\r\npynapi\r\n");
    sb_puts(&b,"--");sb_puts(&b,boundary);sb_puts(&b,"\r\nContent-Disposition: form-data; name=\"client_ver\"\r\n\r\n" VERSION "\r\n");
    for(int i=0;i<nf;i++){ sb_puts(&b,"--");sb_puts(&b,boundary);sb_puts(&b,"\r\nContent-Disposition: form-data; name=\"");sb_puts(&b,fields[i][0]);sb_puts(&b,"\"\r\n\r\n");sb_puts(&b,fields[i][1]);sb_puts(&b,"\r\n"); }
    sb_puts(&b,"--");sb_puts(&b,boundary);sb_puts(&b,"--\r\n");
    SB req; sb_init(&req); char hdr[512];
    snprintf(hdr,sizeof hdr,"POST /api/api-napiprojekt3.php HTTP/1.0\r\nHost: %s\r\nUser-Agent: aqnapi-c/%s\r\nAccept: */*\r\nContent-Type: multipart/form-data; boundary=%s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",host,VERSION,boundary,b.len);
    sb_puts(&req,hdr); sb_putn(&req,b.b,b.len); free(b.b);
    size_t bl; char*body=http_request(host,req.b,req.len,&bl); free(req.b); return body;
}
/* cover: mode=2 — okładka + ocena filmu skojarzonego z hashem */
static int cmd_np_cover(const char*movie,const char*out){
    if(!movie){ fprintf(stderr,"napiprojekt cover wymaga pliku\n"); return 2; }
    char md[33]; if(md5_10mb(movie,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); return 1; }
    const char*f[][2]={{"mode","2"},{"downloaded_cover_id",md}};
    char*xml=np_http_post(f,2); if(!xml) die("napiprojekt: błąd połączenia");
    char*mv=strstr(xml,"<movie>");
    char*st=mv?xml_first(mv,"status"):strdup(""); int ok=mv && !strcasecmp(st,"success"); free(st);
    if(!ok){ free(xml); fprintf(stderr,"Plik nie jest skojarzony z żadnym filmem (brak okładki).\n"); return 2; }
    char*title=xml_first(mv,"title"),*year=xml_first(mv,"year"),*id=xml_first(mv,"id"),
         *rating=xml_first(mv,"rating"),*votes=xml_first(mv,"votes"),
         *imdb=xml_first(mv,"imdb_com"),*nick=xml_first(mv,"nick"),*cb=xml_first(mv,"cover");
    printf("Film: %s (%s)\n",title,year);
    printf("  MovieId: %s\n",id);
    if(rating[0]) printf("  Ocena: %s (%s głosów)\n",rating,votes);
    if(imdb[0]) printf("  IMDB: %s\n",imdb);
    if(nick[0]) printf("  Skojarzył: %s\n",nick);
    size_t clen=0; unsigned char*cover=cb[0]?b64decode(cb,strlen(cb),&clen):NULL;
    if(out && cover && clen){ write_file(out,(char*)cover,clen); printf("  Okładka zapisana (%zu B): %s\n",clen,out); }
    else if(cover && clen){ printf("  Okładka: %zu B (użyj -o by zapisać)\n",clen); }
    free(cover); free(title);free(year);free(id);free(rating);free(votes);free(imdb);free(nick);free(cb); free(xml); return 0;
}
/* version: mode=16 — najnowsza wersja klienta */
static int cmd_np_version(void){
    const char*f[][2]={{"mode","16"}};
    char*xml=np_http_post(f,1); if(!xml) die("napiprojekt: błąd połączenia");
    char*ver=xml_first(xml,"version_number"),*url=xml_first(xml,"download_url"),*ch=xml_first(xml,"latest_changes");
    printf("Najnowsza wersja: %s\n",ver); printf("Pobierz: %s\n",url);
    if(ch[0]) printf("Zmiany:\n%s\n",ch);
    free(ver);free(url);free(ch);free(xml); return 0;
}
static const char*NP_KINDS[]={
    "Napisy nie są w ogóle wyświetlane",
    "Napisy są do tego filmu, ale wyświetlają się w nieodpowiednim momencie",
    "Napisy są do zupełnie innego filmu",
    "Napisy są przetłumaczone przez komputer - translator",
    "Napisy mają złe kodowanie, krzaki zamiast polskich liter",
    "Jest tylko część napisów",
    "Program pobrał napisy w innym języku, niż to było ustawione",
    "Gdy włączam film napisy pojawiają mi się podwójnie",
    "Inny powód"};
/* report: mode=64 — zgłoś złe napisy (user_nick/user_password) */
static int cmd_np_report(const char*cfgpath,const char*movie,int kind,const char*comment,const char*lang,int list){
    if(list){ for(int i=0;i<9;i++) printf("  %d: %s\n",i,NP_KINDS[i]); return 0; }
    if(kind<0||kind>=9){ fprintf(stderr,"Zły --kind (0-8)\n"); return 4; }
    if(!movie){ fprintf(stderr,"napiprojekt report wymaga pliku\n"); return 2; }
    char md[33]; if(md5_10mb(movie,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); return 1; }
    char user[128],pass[128]; np_creds(cfgpath,user,sizeof user,pass,sizeof pass);
    if(!user[0]||!pass[0]){ fprintf(stderr,"Błąd uwierzytelnienia: Zgłoszenie wymaga loginu i hasła.\n"); return 2; }
    char fn[512]; input_basename(movie,fn,sizeof fn);
    char L[8]; snprintf(L,sizeof L,"%s",lang&&lang[0]?lang:"PL"); for(char*p=L;*p;p++)*p=toupper((unsigned char)*p);
    char ks[8]; snprintf(ks,sizeof ks,"%d",kind); char*enc=np_encode_password(pass);
    const char*f[][2]={{"mode","64"},{"user_nick",user},{"user_password",enc},{"RBS_FileHash",md},{"RBS_VideoFile",fn},
        {"RBS_Lang",L},{"RBS_ProblemKind",ks},{"RBS_ProblemPercent",""},{"RBS_ProblemPlayer",""},{"RBS_ProblemComment",comment?comment:""}};
    char*xml=np_http_post(f,10); free(enc); if(!xml) die("napiprojekt: błąd połączenia");
    char*err=xml_first(xml,"error"); if(err[0]){ fprintf(stderr,"Błąd: %s\n",err); free(err); free(xml); return 1; } free(err);
    char*st=xml_first(xml,"status"); printf("Zgłoszono (%s): status=%s\n",NP_KINDS[kind], st[0]?st:"ok");
    free(st); free(xml); return 0;
}

/* ---------------------------------------------------------------- HTTPS (TLS) + update
 * Aktywne tylko w buildzie monorepo (-DAQNAPI_TLS, linkowanie third_party/mbedtls). */
#ifdef AQNAPI_TLS
#ifdef __COSMOPOLITAN__
#include "third_party/mbedtls/ssl.h"
#include "third_party/mbedtls/net_sockets.h"
#include "third_party/mbedtls/entropy.h"
#include "third_party/mbedtls/ctr_drbg.h"
#include "third_party/mbedtls/x509_crt.h"
#else
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#endif

/* prosty cookie-jar dla sesji WWW (napisy24). Włączany globalnie. */
static char g_cookies[4096]=""; static int g_cookies_on=0;
static void cookie_reset(void){ g_cookies[0]=0; g_cookies_on=1; }
static void cookie_set(const char*nv){ /* nv = "name=value" (bez atrybutów) */
    const char*eq=strchr(nv,'='); if(!eq) return; size_t nl=eq-nv; char name[128]; if(nl>=sizeof name)nl=sizeof name-1; memcpy(name,nv,nl); name[nl]=0;
    /* usuń istniejące o tej nazwie */ char nb[4096]; nb[0]=0; char tmp[4096]; snprintf(tmp,sizeof tmp,"%s",g_cookies);
    for(char*t=strtok(tmp,";");t;t=strtok(NULL,";")){ char*s=t; while(*s==' ')s++; if(strncmp(s,name,nl)==0&&s[nl]=='='){ continue; } if(nb[0]){ strncat(nb,"; ",sizeof nb-strlen(nb)-1);} strncat(nb,s,sizeof nb-strlen(nb)-1); }
    if(nb[0]) strncat(nb,"; ",sizeof nb-strlen(nb)-1); strncat(nb,nv,sizeof nb-strlen(nb)-1); snprintf(g_cookies,sizeof g_cookies,"%s",nb); }
static void cookie_capture(const char*hdrs){ for(const char*p=hdrs;*p;p++){ if((p==hdrs||p[-1]=='\n')&&!strncasecmp(p,"set-cookie:",11)){
    const char*v=p+11; while(*v==' ')v++; const char*e=v; while(*e&&*e!=';'&&*e!='\r'&&*e!='\n')e++; size_t l=e-v; char nv[512]; if(l>=sizeof nv)l=sizeof nv-1; memcpy(nv,v,l); nv[l]=0; if(strchr(nv,'=')) cookie_set(nv); } } }

/* Rozmiar ostatniego zasobu HTTPS (z Content-Range/Content-Length); -1 = nieznany. */
static long g_http_total=-1;
/* GET/POST po HTTPS; obsługa chunked, przekierowań i cookies. Zwraca ciało (malloc). */
static char* https_fetch(const char*method,const char*host,const char*path,
                         const char*extra_hdrs,const char*post,int*status,size_t*outlen,int depth){
    if(depth>6) return NULL;
    mbedtls_net_context net; mbedtls_ssl_context ssl; mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg; mbedtls_entropy_context ent; char*result=NULL;
    mbedtls_net_init(&net); mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&drbg); mbedtls_entropy_init(&ent);
    int r;
    if(mbedtls_ctr_drbg_seed(&drbg,mbedtls_entropy_func,&ent,(const unsigned char*)"aqnapi",6)) goto done;
    if(mbedtls_net_connect(&net,host,"443",MBEDTLS_NET_PROTO_TCP)) goto done;
    if(mbedtls_ssl_config_defaults(&conf,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)) goto done;
    { /* weryfikacja CA: wbudowany bundle /zip/cacert.pem (fallback: systemowy; ostatecznie brak) */
      static mbedtls_x509_crt CA; static int ca_state=0; /* 0=nieładowane 1=ok 2=brak */
      if(ca_state==0){ mbedtls_x509_crt_init(&CA); size_t cl; char*cb=NULL;
          /* kolejność: bundle w APE (cosmo), env, typowe ścieżki (Debian/OpenWrt/BSD/Alpine) */
          static const char*paths[]={"/zip/cacert.pem",NULL,"/etc/ssl/certs/ca-certificates.crt",
              "/etc/ssl/cert.pem","/etc/pki/tls/certs/ca-bundle.crt","/etc/ssl/ca-bundle.pem",
              "/usr/local/share/certs/ca-root-nss.crt",NULL};
          const char*envp=getenv("SSL_CERT_FILE"); paths[1]=envp;
          for(int i=0;i<7 && !cb;i++){ if(paths[i]) cb=read_file(paths[i],&cl); }
          if(cb && mbedtls_x509_crt_parse(&CA,(const unsigned char*)cb,cl+1)>=0) ca_state=1; else ca_state=2; free(cb); }
      if(ca_state==1){ mbedtls_ssl_conf_ca_chain(&conf,&CA,NULL); mbedtls_ssl_conf_authmode(&conf,MBEDTLS_SSL_VERIFY_REQUIRED); }
      else mbedtls_ssl_conf_authmode(&conf,MBEDTLS_SSL_VERIFY_NONE); }
    mbedtls_ssl_conf_rng(&conf,mbedtls_ctr_drbg_random,&drbg);
    if(mbedtls_ssl_setup(&ssl,&conf)) goto done;
    mbedtls_ssl_set_hostname(&ssl,host);
    mbedtls_ssl_set_bio(&ssl,&net,mbedtls_net_send,mbedtls_net_recv,NULL);
    while((r=mbedtls_ssl_handshake(&ssl))!=0){ if(r!=MBEDTLS_ERR_SSL_WANT_READ&&r!=MBEDTLS_ERR_SSL_WANT_WRITE) goto done; }
    { SB req; sb_init(&req); char h[1400];
      snprintf(h,sizeof h,"%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: aqnapi v%s\r\n",method,path,host,VERSION);
      sb_puts(&req,h); if(extra_hdrs) sb_puts(&req,extra_hdrs);
      if(g_cookies_on && g_cookies[0]){ sb_puts(&req,"Cookie: "); sb_puts(&req,g_cookies); sb_puts(&req,"\r\n"); }
      if(post){ char cl[64]; snprintf(cl,sizeof cl,"Content-Length: %zu\r\n",strlen(post)); sb_puts(&req,cl); if(!extra_hdrs||!strcasestr(extra_hdrs,"content-type")) sb_puts(&req,"Content-Type: application/json\r\n"); }
      sb_puts(&req,"Connection: close\r\n\r\n"); if(post) sb_puts(&req,post);
      size_t off=0; int fail=0; while(off<req.len){ r=mbedtls_ssl_write(&ssl,(unsigned char*)req.b+off,req.len-off);
          if(r<=0){ if(r==MBEDTLS_ERR_SSL_WANT_WRITE) continue; fail=1; break; } off+=r; }
      free(req.b); if(fail) goto done; }
    { SB resp; sb_init(&resp); unsigned char buf[4096];
      while((r=mbedtls_ssl_read(&ssl,buf,sizeof buf))>0) sb_putn(&resp,(char*)buf,(size_t)r);
      int st=0; sscanf(resp.b,"HTTP/1.%*d %d",&st);
      char*he=strstr(resp.b,"\r\n\r\n"); if(!he){ free(resp.b); goto done; }
      *he=0; char*hdrs=resp.b; char*bodyp=he+4; size_t blen=resp.len-(bodyp-resp.b);
      if(g_cookies_on) cookie_capture(hdrs);
      /* rozmiar zasobu: Content-Range ".../N" ma pierwszeństwo, wpp. Content-Length */
      g_http_total=-1;
      for(char*p=hdrs;*p;p++){ if((p==hdrs||p[-1]=='\n')&&!strncasecmp(p,"content-range:",14)){ char*sl=strchr(p,'/'); if(sl) g_http_total=strtol(sl+1,NULL,10); break; } }
      if(g_http_total<0) for(char*p=hdrs;*p;p++){ if((p==hdrs||p[-1]=='\n')&&!strncasecmp(p,"content-length:",15)){ g_http_total=strtol(p+15,NULL,10); break; } }
      if(st>=300&&st<400){ /* przekierowanie */
          char*loc=NULL; for(char*p=hdrs;*p;p++) if((p==hdrs||p[-1]=='\n')&&!strncasecmp(p,"location:",9)){ loc=p+9; break; }
          if(loc){ while(*loc==' ')loc++; char url[1024]; size_t i=0; while(loc[i]&&loc[i]!='\r'&&loc[i]!='\n'&&i<sizeof url-1){ url[i]=loc[i]; i++; } url[i]=0;
              char nh[256],np2[1024];
              if(url[0]=='/'){ /* względny -> ten sam host */ snprintf(nh,sizeof nh,"%s",host); snprintf(np2,sizeof np2,"%s",url); }
              else { const char*u=url; if(!strncmp(u,"https://",8))u+=8; else if(!strncmp(u,"http://",7))u+=7;
                  const char*sl=strchr(u,'/'); if(sl){ size_t hl=sl-u; if(hl>=sizeof nh)hl=sizeof nh-1; memcpy(nh,u,hl); nh[hl]=0; snprintf(np2,sizeof np2,"%s",sl); }
                  else { snprintf(nh,sizeof nh,"%s",u); strcpy(np2,"/"); } }
              free(resp.b);
              /* zwolnij TLS przed rekurencją */ mbedtls_ssl_close_notify(&ssl); mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf); mbedtls_net_free(&net); mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent);
              return https_fetch("GET",nh,np2,extra_hdrs,NULL,status,outlen,depth+1); }
      }
      int chunked=0; for(char*p=hdrs;*p;p++) if((p==hdrs||p[-1]=='\n')&&!strncasecmp(p,"transfer-encoding:",18)&&strcasestr(p,"chunked")) chunked=1;
      SB out; sb_init(&out);
      if(chunked){ char*p=bodyp; size_t left=blen; while(left>0){ char*eol=memchr(p,'\n',left); if(!eol) break; long csz=strtol(p,NULL,16); size_t adv=(eol-p)+1; p+=adv; left-=adv; if(csz<=0) break; if((size_t)csz>left) csz=left; sb_putn(&out,p,csz); p+=csz; left-=csz; if(left>=2){ p+=2; left-=2; } } }
      else sb_putn(&out,bodyp,blen);
      *status=st; *outlen=out.len; result=out.b; free(resp.b); }
done:
    mbedtls_ssl_close_notify(&ssl); mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf);
    mbedtls_net_free(&net); mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent);
    return result;
}
static int json_str(const char*j,const char*key,char*out,size_t osz){ char pat[64]; snprintf(pat,sizeof pat,"\"%s\"",key);
    const char*p=strstr(j,pat); if(!p){ out[0]=0; return 0; } p+=strlen(pat); while(*p&&*p!=':')p++; if(*p)p++; while(*p==' '||*p=='"')p++;
    size_t i=0; while(*p&&*p!='"'&&i<osz-1){ if(*p=='\\'&&p[1]){p++;} out[i++]=*p++; } out[i]=0; return i>0; }
static long json_num(const char*j,const char*key){ char pat[64]; snprintf(pat,sizeof pat,"\"%s\"",key);
    const char*p=strstr(j,pat); if(!p) return -1; p+=strlen(pat); while(*p&&*p!=':')p++; if(*p)p++; while(*p==' ')p++; return strtol(p,NULL,10); }
/* wartość skalarów jako tekst; "None" gdy brak lub null (jak dict.get w Pythonie) */
static void json_optnum(const char*j,const char*key,char*out,size_t osz){ char pat[64]; snprintf(pat,sizeof pat,"\"%s\"",key);
    const char*p=strstr(j,pat); if(!p){ snprintf(out,osz,"None"); return; } p+=strlen(pat); while(*p&&*p!=':')p++; if(*p)p++; while(*p==' ')p++;
    if(!strncmp(p,"null",4)){ snprintf(out,osz,"None"); return; }
    if(!strncmp(p,"true",4)){ snprintf(out,osz,"True"); return; }
    if(!strncmp(p,"false",5)){ snprintf(out,osz,"False"); return; }
    if(*p=='"'){ p++; size_t i=0; while(*p&&*p!='"'&&i<osz-1)out[i++]=*p++; out[i]=0; return; }
    char*e; long v=strtol(p,&e,10); if(e==p){ snprintf(out,osz,"None"); return; } snprintf(out,osz,"%ld",v); }

/* --- OpenSubtitles (REST v1 po TLS) --- */
static const char* OS_HOST="api.opensubtitles.com";
static void os_creds(const char*cfgpath,char*key,char*user,char*pass){
    Ini ini; ini_load(config_path(cfgpath),&ini);
    const char*k=ini_get(&ini,2,"api_key"); const char*u=ini_get(&ini,2,"username"); const char*p=ini_get(&ini,2,"password");
    const char*ek=getenv("OS_API_KEY"),*eu=getenv("OS_USERNAME"),*ep=getenv("OS_PASSWORD");
    snprintf(key,128,"%s",ek&&*ek?ek:k); snprintf(user,128,"%s",eu&&*eu?eu:u); snprintf(pass,128,"%s",ep&&*ep?ep:p);
}
static char* os_hdrs(const char*key,const char*bearer){ SB h; sb_init(&h); char t[400];
    /* User-Agent dostarcza https_fetch (bazowo "aqnapi v<wersja>") — bez duplikatu */
    snprintf(t,sizeof t,"Api-Key: %s\r\nAccept: application/json\r\n",key); sb_puts(&h,t);
    if(bearer&&bearer[0]){ snprintf(t,sizeof t,"Authorization: Bearer %s\r\n",bearer); sb_puts(&h,t); } return h.b; }
static const char* os_cache_path(void);  /* def. niżej */
/* login -> token (i host bazowy). Zwraca 1 przy sukcesie. */
static int os_login_ex(const char*cfgpath,char token[2048],char host[256],char*ad_out,char*vip_out){
    char key[128],user[128],pass[128]; os_creds(cfgpath,key,user,pass);
    if(!key[0]){ fprintf(stderr,"Błąd uwierzytelnienia: OpenSubtitles wymaga klucza API (Api-Key)\n"); return 0; }
    if(!user[0]||!pass[0]){ fprintf(stderr,"Błąd uwierzytelnienia: brak username/password OpenSubtitles\n"); return 0; }
    char body[512]; snprintf(body,sizeof body,"{\"username\":\"%s\",\"password\":\"%s\"}",user,pass);
    char*hd=os_hdrs(key,NULL); int st; size_t n;
    char*j=https_fetch("POST",OS_HOST,"/api/v1/login",hd,body,&st,&n,0); free(hd);
    if(!j||st!=200){ fprintf(stderr,"Błąd: OpenSubtitles /login HTTP %d: %.120s\n",j?st:0,j?j:""); free(j); return 0; }
    token[0]=0; json_str(j,"token",token,2048); char bu[256]; if(json_str(j,"base_url",bu,sizeof bu)&&bu[0]) snprintf(host,256,"%s",bu); else snprintf(host,256,"%s",OS_HOST);
    if(ad_out) json_optnum(j,"allowed_downloads",ad_out,32); if(vip_out) json_optnum(j,"vip",vip_out,32);
    long ad=json_num(j,"allowed_downloads"); free(j);
    if(token[0]){ /* zapisz cache tokenu (kompatybilnie z Pythonem, do logout/reuse) */
        const char*cp=os_cache_path(); char dir[512]; snprintf(dir,sizeof dir,"%s",cp); char*sl=strrchr(dir,'/');
        if(sl){ *sl=0; char cmd[600]; snprintf(cmd,sizeof cmd,"mkdir -p '%s'",dir); if(system(cmd)!=0){} }
        FILE*cf=fopen(cp,"w"); if(cf){ fprintf(cf,"{\"token\": \"%s\", \"base\": \"%s\", \"api_key\": \"%s\", \"username\": \"%s\", \"allowed_downloads\": %ld}",token,host,key,user,ad); fclose(cf); } }
    return token[0]!=0;
}
static int os_login(const char*cfgpath,char token[2048],char host[256]){ return os_login_ex(cfgpath,token,host,NULL,NULL); }
static int cmd_os_login(const char*cfgpath){ char tok[2048],host[256],ad[32],vip[32]; if(!os_login_ex(cfgpath,tok,host,ad,vip)) return 2;
    printf("Zalogowano. base_url=%s limit pobrań=%s VIP=%s\n",host,ad,vip); return 0; }
static int cmd_os_search(const char*cfgpath,const char*imdb,const char*title,const char*query,const char*lang,const char*season,const char*episode){
    char key[128],u[128],p[128]; os_creds(cfgpath,key,u,p);
    if(!key[0]){ fprintf(stderr,"Błąd uwierzytelnienia: OpenSubtitles wymaga klucza API (Api-Key)\n"); return 2; }
    SB q; sb_init(&q); int first=1;
    #define QP(k,v) do{ if(v&&*(v)){ sb_puts(&q,first?"?":"&"); sb_puts(&q,k); sb_putc(&q,'='); char*e=url_encode(v); sb_puts(&q,e); free(e); first=0; } }while(0)
    const char*qv=query&&query[0]?query:title;
    QP("query",qv);
    if(imdb&&imdb[0]){ const char*d=imdb; while(*d&&!isdigit((unsigned char)*d))d++; QP("imdb_id",d); }
    QP("languages",lang); QP("season_number",season); QP("episode_number",episode);
    #undef QP
    char path[700]; snprintf(path,sizeof path,"/api/v1/subtitles%s",q.b); free(q.b);
    char*hd=os_hdrs(key,NULL); int st; size_t n; char*j=https_fetch("GET",OS_HOST,path,hd,NULL,&st,&n,0); free(hd);
    if(!j||st!=200){ fprintf(stderr,"Błąd: OpenSubtitles /subtitles HTTP %d\n",j?st:0); free(j); return 1; }
    Hits h={0}; const char*pp=j;
    while((pp=strstr(pp,"\"subtitle_id\""))){ const char*nx=strstr(pp+1,"\"subtitle_id\""); size_t wl=nx?(size_t)(nx-pp):strlen(pp);
        char*blk=xmalloc(wl+1); memcpy(blk,pp,wl); blk[wl]=0;
        Hit*x=hits_push(&h); snprintf(x->service,16,"opensubtitles");
        long f=json_num(blk,"file_id"); if(f>0) snprintf(x->id,48,"%ld",f);
        json_str(blk,"language",x->lang,sizeof x->lang);
        char mv[256]; if(json_str(blk,"movie_name",mv,sizeof mv)&&mv[0]) snprintf(x->title,256,"%s",mv); else json_str(blk,"title",x->title,sizeof x->title);
        long yr=json_num(blk,"year"); if(yr>0) snprintf(x->year,16,"%ld",yr);
        json_str(blk,"release",x->release,sizeof x->release);
        long dc=json_num(blk,"download_count"); x->dls=(dc>0)?(int)dc:0;
        free(blk); pp=nx?nx:pp+wl; }
    free(j);
    if(h.n==0){ printf("Brak wyników.\n"); return 1; }
    print_hits_header(); for(int i=0;i<h.n;i++) print_hit(h.a[i].service,h.a[i].id,h.a[i].lang,h.a[i].dls,h.a[i].title,h.a[i].year,h.a[i].release);
    printf("\nPobierz: aqnapi <serwis> download/getid/download-id <ID>\n"); return 0;
}
static int cmd_os_download(const char*cfgpath,const char*file_id,const char*out,const char*movie,double flag_fps,SanOpts opt){
    char key[128],u[128],p[128]; os_creds(cfgpath,key,u,p);
    char tok[2048],host[256]; if(!os_login(cfgpath,tok,host)) return 2;
    char body[128]; snprintf(body,sizeof body,"{\"file_id\":%s}",file_id);
    char*hd=os_hdrs(key,tok); int st; size_t n; char*j=https_fetch("POST",host,"/api/v1/download",hd,body,&st,&n,0); free(hd);
    if(!j||st!=200){ fprintf(stderr,"Błąd: OpenSubtitles /download HTTP %d: %.160s\n",j?st:0,j?j:""); free(j); return 1; }
    char link[1024]; if(!json_str(j,"link",link,sizeof link)||!link[0]){ fprintf(stderr,"Nie znaleziono: brak linku do pobrania\n"); free(j); return 1; }
    long remaining=json_num(j,"remaining"); char reset[64]; json_str(j,"reset_time",reset,sizeof reset);
    /* pobierz link (GET, https_fetch obsłuży redirecty) */
    const char*lu=link; if(!strncmp(lu,"https://",8))lu+=8; else if(!strncmp(lu,"http://",7))lu+=7;
    char lh[256],lp[900]; const char*sl=strchr(lu,'/'); size_t hl=sl?(size_t)(sl-lu):strlen(lu); if(hl>=sizeof lh)hl=sizeof lh-1; memcpy(lh,lu,hl); lh[hl]=0; snprintf(lp,sizeof lp,"%s",sl?sl:"/");
    int st2; size_t dl; char*data=https_fetch("GET",lh,lp,NULL,NULL,&st2,&dl,0); free(j);
    if(!data||st2!=200){ fprintf(stderr,"Błąd: pobranie pliku HTTP %d\n",st2); free(data); return 1; }
    double fps=resolve_fps(movie,0,flag_fps);
    SB o; sb_init(&o); SanReport rep; convert_bytes((unsigned char*)data,dl,fps,"srt",&opt,&o,&rep); free(data);
    char*outp=out?strdup(out):(movie?default_out(movie,NULL):strdup("napisy.srt")); write_file(outp,o.b,o.len); print_saved(outp,o.len,&rep);
    printf("Pozostały limit pobrań: %ld (reset: %s)\n",remaining,reset); free(outp); free(o.b); return 0;
}
/* ---- pretty-print JSON jak json.dumps(indent=2, ensure_ascii=False) ---- */
static void jpp_ws(const char**p){ while(**p==' '||**p=='\t'||**p=='\r'||**p=='\n') (*p)++; }
static void jpp_decode(const char**p,SB*out){ const char*s=*p; if(*s!='"'){*p=s;return;} s++;
    while(*s && *s!='"'){ if(*s=='\\'){ s++; char c=*s;
        switch(c){ case 'n':sb_putc(out,'\n');break; case 't':sb_putc(out,'\t');break; case 'r':sb_putc(out,'\r');break;
            case 'b':sb_putc(out,'\b');break; case 'f':sb_putc(out,'\f');break;
            case '/':sb_putc(out,'/');break; case '\\':sb_putc(out,'\\');break; case '"':sb_putc(out,'"');break;
            case 'u':{ char hx[5]={s[1],s[2],s[3],s[4],0}; unsigned cp=(unsigned)strtol(hx,NULL,16); s+=4;
                if(cp>=0xD800&&cp<=0xDBFF && s[1]=='\\'&&s[2]=='u'){ char h2[5]={s[3],s[4],s[5],s[6],0}; unsigned lo=(unsigned)strtol(h2,NULL,16); s+=6; cp=0x10000+((cp-0xD800)<<10)+(lo-0xDC00); }
                if(cp<0x80) sb_putc(out,(char)cp);
                else if(cp<0x800){ sb_putc(out,(char)(0xC0|(cp>>6))); sb_putc(out,(char)(0x80|(cp&0x3F))); }
                else if(cp<0x10000){ sb_putc(out,(char)(0xE0|(cp>>12))); sb_putc(out,(char)(0x80|((cp>>6)&0x3F))); sb_putc(out,(char)(0x80|(cp&0x3F))); }
                else { sb_putc(out,(char)(0xF0|(cp>>18))); sb_putc(out,(char)(0x80|((cp>>12)&0x3F))); sb_putc(out,(char)(0x80|((cp>>6)&0x3F))); sb_putc(out,(char)(0x80|(cp&0x3F))); }
            } break;
            default: sb_putc(out,c); break; }
        s++; } else { sb_putc(out,*s); s++; } }
    if(*s=='"') s++; *p=s; }
static void jpp_emit(SB*out,const char*s,size_t n){ sb_putc(out,'"');
    for(size_t i=0;i<n;i++){ unsigned char c=(unsigned char)s[i];
        if(c=='"') sb_puts(out,"\\\""); else if(c=='\\') sb_puts(out,"\\\\");
        else if(c=='\n') sb_puts(out,"\\n"); else if(c=='\t') sb_puts(out,"\\t"); else if(c=='\r') sb_puts(out,"\\r");
        else if(c=='\b') sb_puts(out,"\\b"); else if(c=='\f') sb_puts(out,"\\f");
        else if(c<0x20){ char u[8]; snprintf(u,sizeof u,"\\u%04x",c); sb_puts(out,u); }
        else sb_putc(out,(char)c); }
    sb_putc(out,'"'); }
static void jpp_indent(SB*out,int d){ for(int i=0;i<d*2;i++) sb_putc(out,' '); }
static void jpp(const char**p,int depth,SB*out){ jpp_ws(p); char c=**p;
    if(c=='{'){ (*p)++; jpp_ws(p); if(**p=='}'){ (*p)++; sb_puts(out,"{}"); return; }
        sb_puts(out,"{\n");
        while(**p && **p!='}'){ jpp_ws(p);
            SB kb; sb_init(&kb); jpp_decode(p,&kb); jpp_ws(p); if(**p==':') (*p)++;
            jpp_indent(out,depth+1); jpp_emit(out,kb.b,kb.len); sb_puts(out,": "); free(kb.b);
            jpp(p,depth+1,out); jpp_ws(p);
            if(**p==','){ (*p)++; sb_puts(out,",\n"); } else sb_puts(out,"\n"); }
        if(**p=='}') (*p)++; jpp_indent(out,depth); sb_putc(out,'}');
    } else if(c=='['){ (*p)++; jpp_ws(p); if(**p==']'){ (*p)++; sb_puts(out,"[]"); return; }
        sb_puts(out,"[\n");
        while(**p && **p!=']'){ jpp_indent(out,depth+1); jpp(p,depth+1,out); jpp_ws(p);
            if(**p==','){ (*p)++; sb_puts(out,",\n"); } else sb_puts(out,"\n"); }
        if(**p==']') (*p)++; jpp_indent(out,depth); sb_putc(out,']');
    } else if(c=='"'){ SB s2; sb_init(&s2); jpp_decode(p,&s2); jpp_emit(out,s2.b,s2.len); free(s2.b);
    } else { const char*s=*p; while(**p && !strchr(",}]\r\n\t ",**p)) (*p)++; sb_putn(out,s,(size_t)(*p-s)); } }

static const char* os_cache_path(void){ static char buf[512]; const char*h=getenv("HOME"); snprintf(buf,sizeof buf,"%s/.cache/aqnapi/os_token.json",h?h:"."); return buf; }
/* OpenSubtitles logout: DELETE /api/v1/logout tokenem z cache; jak Python. */
static int cmd_os_logout(const char*cfgpath){
    char key[128],u[128],p[128]; os_creds(cfgpath,key,u,p);
    const char*cp=os_cache_path(); size_t cl; char*cache=read_file(cp,&cl);
    char token[2048]="",base[256]=""; if(cache){ json_str(cache,"token",token,sizeof token); json_str(cache,"base",base,sizeof base); free(cache); }
    if(!token[0]){ fprintf(stderr,"Błąd uwierzytelnienia: Ta operacja wymaga zalogowania (POST /login)\n"); return 2; }
    if(!base[0]) snprintf(base,sizeof base,"%s",OS_HOST);
    /* base może zawierać schemat (https://host) — użyj samego hosta */
    const char*bh=base; if(!strncmp(bh,"https://",8)) bh+=8; else if(!strncmp(bh,"http://",7)) bh+=7;
    char host[256]; snprintf(host,sizeof host,"%s",bh); char*sl=strchr(host,'/'); if(sl)*sl=0;
    char*hd=os_hdrs(key,token); int st=0; size_t n=0; char*r=https_fetch("DELETE",host,"/api/v1/logout",hd,NULL,&st,&n,0); free(hd); free(r);
    remove(cp);
    printf("%s\n", st==200?"Wylogowano":"Nie udało się wylogować"); return 0;
}
static int cmd_os_formats(const char*cfgpath){
    char key[128],u[128],p[128]; os_creds(cfgpath,key,u,p);
    if(!key[0]){ fprintf(stderr,"Błąd uwierzytelnienia: OpenSubtitles wymaga klucza API (Api-Key)\n"); return 2; }
    char*hd=os_hdrs(key,NULL); int st=0; size_t n=0; char*j=https_fetch("GET",OS_HOST,"/api/v1/infos/formats",hd,NULL,&st,&n,0); free(hd);
    if(!j||st!=200){ fprintf(stderr,"Błąd: OpenSubtitles /infos/formats HTTP %d\n",j?st:0); free(j); return 1; }
    const char*a=strstr(j,"\"output_formats\""); SB o; sb_init(&o); int first=1;
    if(a){ a=strchr(a,'['); if(a){ a++; while(*a && *a!=']'){ jpp_ws(&a); if(*a=='"'){ SB s2; sb_init(&s2); jpp_decode(&a,&s2);
        if(!first) sb_puts(&o,", "); sb_putn(&o,s2.b,s2.len); first=0; free(s2.b); jpp_ws(&a); if(*a==',') a++; } else if(*a==']') break; else a++; } } }
    printf("%s\n", o.b?o.b:""); free(o.b); free(j); return 0;
}
static int cmd_os_languages(const char*cfgpath){
    char key[128],u[128],p[128]; os_creds(cfgpath,key,u,p);
    if(!key[0]){ fprintf(stderr,"Błąd uwierzytelnienia: OpenSubtitles wymaga klucza API (Api-Key)\n"); return 2; }
    char*hd=os_hdrs(key,NULL); int st=0; size_t n=0; char*j=https_fetch("GET",OS_HOST,"/api/v1/infos/languages",hd,NULL,&st,&n,0); free(hd);
    if(!j||st!=200){ fprintf(stderr,"Błąd: OpenSubtitles /infos/languages HTTP %d\n",j?st:0); free(j); return 1; }
    const char*pp=j;
    while((pp=strstr(pp,"\"language_code\""))){ const char*nx=strstr(pp+1,"\"language_code\""); size_t wl=nx?(size_t)(nx-pp):strlen(pp);
        char*blk=xmalloc(wl+1); memcpy(blk,pp,wl); blk[wl]=0;
        char code[32],name[128]; json_str(blk,"language_code",code,sizeof code); json_str(blk,"language_name",name,sizeof name);
        printf("%s: %s\n",code,name); free(blk); pp=nx?nx:pp+wl; }
    free(j); return 0;
}
static int cmd_os_guessit(const char*cfgpath,const char*filename){
    char key[128],u[128],p[128]; os_creds(cfgpath,key,u,p);
    if(!key[0]){ fprintf(stderr,"Błąd uwierzytelnienia: OpenSubtitles wymaga klucza API (Api-Key)\n"); return 2; }
    char*ef=url_encode(filename?filename:""); char path[900]; snprintf(path,sizeof path,"/api/v1/utilities/guessit?filename=%s",ef); free(ef);
    char*hd=os_hdrs(key,NULL); int st=0; size_t n=0; char*j=https_fetch("GET",OS_HOST,path,hd,NULL,&st,&n,0); free(hd);
    if(!j||st!=200){ fprintf(stderr,"Błąd: OpenSubtitles /utilities/guessit HTTP %d\n",j?st:0); free(j); return 1; }
    const char*q=j; SB o; sb_init(&o); jpp(&q,0,&o); printf("%s\n",o.b?o.b:""); free(o.b); free(j); return 0;
}
/* --- napisy24 WWW (Joomla + Community Builder + RSForm) po HTTPS --- */
static int n24_scrape_token(const char*html,char*out,size_t osz){
    for(const char*p=strstr(html,"name=\"");p;p=strstr(p+1,"name=\"")){ const char*h=p+6; int ok=1;
        for(int i=0;i<32;i++){ if(!isxdigit((unsigned char)h[i])){ ok=0; break; } }
        if(ok && !strncmp(h+32,"\" value=\"1\"",10)){ size_t l=32; if(l>=osz)l=osz-1; memcpy(out,h,l); out[l]=0; return 1; } }
    out[0]=0; return 0;
}
static int n24_web_login(const char*cfgpath){
    Ini ini; ini_load(config_path(cfgpath),&ini); const char*u=ini_get(&ini,0,"login"),*pw=ini_get(&ini,0,"pass");
    const char*eu=getenv("NAPI24_LOGIN"),*ep=getenv("NAPI24_PASS"); if(eu&&*eu)u=eu; if(ep&&*ep)pw=ep;
    if(!u||!u[0]||!pw||!pw[0]){ fprintf(stderr,"Błąd uwierzytelnienia: brak login/pass napisy24\n"); return 0; }
    cookie_reset();
    int st; size_t n; char*home=https_fetch("GET","napisy24.pl","/",NULL,NULL,&st,&n,0);
    char token[40]=""; if(home){ n24_scrape_token(home,token,sizeof token); free(home); }
    char*eu2=url_encode(u),*ep2=url_encode(pw); SB b; sb_init(&b); char t[700];
    snprintf(t,sizeof t,"username=%s&passwd=%s&remember=yes&option=com_comprofiler&view=login&op2=login&return=&loginfrom=loginmodule",eu2,ep2);
    sb_puts(&b,t); free(eu2); free(ep2); if(token[0]){ sb_puts(&b,"&"); sb_puts(&b,token); sb_puts(&b,"=1"); }
    char*resp=https_fetch("POST","napisy24.pl","/cb-login","Content-Type: application/x-www-form-urlencoded\r\n",b.b,&st,&n,0); free(b.b);
    int ok=resp && strstr(resp,"Wyloguj")!=NULL; free(resp);
    char*dj=https_fetch("GET","napisy24.pl","/dodaj-napisy",NULL,NULL,&st,&n,0); free(dj);
    return ok;
}
static int cmd_n24_weblogin(const char*cfgpath){ if(n24_web_login(cfgpath)){ printf("Zalogowano do napisy24 (WWW).\n"); return 0; } fprintf(stderr,"napisy24: logowanie WWW nieudane\n"); return 1; }

/* walidacja lokalna jak check_srt_for_napisy24: >2 linie/blok + nachodzące czasy.
 * Zwraca liczbę problemów; `out` = wszystkie problemy złączone "\n  " (jak Python join). */
static int n24_check_srt(const char*text,char*out,size_t osz){
    Cues c; cues_init(&c); parse_any(text,DEFAULT_FPS,&c); int probs=0; out[0]=0; size_t used=0;
    #define N24ADD(...) do{ char _l[128]; snprintf(_l,sizeof _l,__VA_ARGS__); \
        if(probs && used<osz) used+=snprintf(out+used,osz-used,"\n  "); \
        if(used<osz) used+=snprintf(out+used,osz-used,"%s",_l); probs++; }while(0)
    for(int i=0;i<c.n;i++){ int real=0; for(int k=0;k<c.a[i].nlines;k++){ int ne=0; for(char*q=c.a[i].lines[k];*q;q++) if(!is_ascii_ws(*q)){ ne=1; break; } if(ne) real++; }
        if(real>2) N24ADD("Blok %d: %d linii tekstu (max 2)",i+1,real); }
    for(int i=0;i<c.n-1;i++){ if(c.a[i].end > c.a[i+1].start) N24ADD("Bloki %d/%d: nachodzące czasy",i+1,i+2); }
    #undef N24ADD
    return probs;
}
/* normalizacja do CRLF (jak normalize_for_napisy24) — zwraca bajty (malloc) + len */
static unsigned char* n24_normalize_crlf(const char*text,size_t*outlen){
    Cues c; cues_init(&c); parse_any(text,DEFAULT_FPS,&c); SB e; sb_init(&e);
    for(int i=0;i<c.n;i++){ char t1[16],t2[16]; ms_to_srt(c.a[i].start,t1); ms_to_srt(c.a[i].end,t2); char num[16]; snprintf(num,sizeof num,"%d",i+1);
        sb_puts(&e,num); sb_putc(&e,'\n'); sb_puts(&e,t1); sb_puts(&e," --> "); sb_puts(&e,t2); sb_putc(&e,'\n');
        if(c.a[i].nlines==0) sb_putc(&e,'\n'); for(int k=0;k<c.a[i].nlines;k++){ sb_puts(&e,c.a[i].lines[k]); sb_putc(&e,'\n'); } sb_putc(&e,'\n'); }
    size_t b=0,en=e.len; while(b<en&&is_ascii_ws(e.b[b]))b++; while(en>b&&is_ascii_ws(e.b[en-1]))en--;
    SB s; sb_init(&s); for(size_t i=b;i<en;i++){ if(e.b[i]=='\n') sb_puts(&s,"\r\n"); else sb_putc(&s,e.b[i]); } sb_puts(&s,"\r\n");
    free(e.b); *outlen=s.len; return (unsigned char*)s.b;
}
typedef struct { const char*imdb,*title,*title_pl,*year,*release,*translator,*sync,*proof,*resolution,*duration,*size,*fps,*season,*episode,*episode_title; } N24Meta;
/* rdzeń web-uploadu napisy24 (/dodaj-napisy): login + multipart + POST.
 * Zwraca komunikat (malloc): "OK" / "Serwer nie potwierdził dodania" /
 * "napisy24: logowanie nieudane". *ok ustawia. Tekst NIE jest zwalniany. */
static char* n24_web_upload_run(const char*cfgpath,const char*srt,const char*text,const N24Meta*m,int*ok){
    *ok=0;
    if(!n24_web_login(cfgpath)) return strdup("napisy24: logowanie nieudane");
    size_t fl; unsigned char*fb=n24_normalize_crlf(text,&fl);
    int serial = m->season && m->season[0];
    const char*bnd="----aqnapin24form"; SB b; sb_init(&b);
    #define FT(nm,val) do{ sb_puts(&b,"--");sb_puts(&b,bnd);sb_puts(&b,"\r\nContent-Disposition: form-data; name=\"");sb_puts(&b,nm);sb_puts(&b,"\"\r\n\r\n");sb_puts(&b,val?val:"");sb_puts(&b,"\r\n"); }while(0)
    FT("form[form_typ]",serial?"Serial":"Film"); FT("form[form_dodajIMDB]",m->imdb);
    FT("form[form_dodaj_tytul]",m->title); FT("form[form_dodaj_polskiTytul]",m->title_pl);
    FT("form[form_dodaj_rok]",m->year); FT("form[form_dodaj_wydanie]",m->release); FT("form[form_dodaj_hash]","");
    FT("form[form_dodaj_tlumaczenie]",m->translator); FT("form[form_dodaj_dopasowanie]",m->sync); FT("form[form_dodaj_korekta]",m->proof);
    FT("form[form_dodaj_rozdzielczosc]",m->resolution); FT("form[form_dodaj_fps][]",m->fps?m->fps:"23.976");
    FT("form[form_dodaj_jezyk][]","Polski"); FT("form[form_dodajIloscPlyt]","1");
    FT("form[form_czas_cd1]",m->duration); FT("form[form_wielkosc_cd1]",m->size);
    /* plik napisów + 3 puste sloty */
    { char fn[512]; input_basename(srt,fn,sizeof fn); sb_puts(&b,"--");sb_puts(&b,bnd);sb_puts(&b,"\r\nContent-Disposition: form-data; name=\"form[form_dodajNapis1_plik]\"; filename=\"");sb_puts(&b,fn);sb_puts(&b,"\"\r\nContent-Type: application/octet-stream\r\n\r\n");sb_putn(&b,(char*)fb,fl);sb_puts(&b,"\r\n"); }
    for(int i=2;i<=4;i++){ char nm[48]; snprintf(nm,sizeof nm,"form[form_dodajNapis%d_plik]",i); sb_puts(&b,"--");sb_puts(&b,bnd);sb_puts(&b,"\r\nContent-Disposition: form-data; name=\"");sb_puts(&b,nm);sb_puts(&b,"\"; filename=\"\"\r\nContent-Type: application/octet-stream\r\n\r\n\r\n"); }
    FT("form[dodajTlumaczenie]","Dodaj"); FT("form[formId]","7"); FT("form[remId]",""); FT("form[form_dodajTlumaczenieId]","0");
    if(serial){ FT("form[realtxt]",m->title); FT("form[serial][]",m->imdb); FT("form[form_dodaj_nrSezonu]",m->season); FT("form[form_dodaj_nrOdcinka]",m->episode); FT("form[form_dodaj_tytulOdcinka]",m->episode_title); FT("form[form_dodaj_cover]","Serialu"); }
    #undef FT
    sb_puts(&b,"--");sb_puts(&b,bnd);sb_puts(&b,"--\r\n"); free(fb);
    char ct[128]; snprintf(ct,sizeof ct,"Content-Type: multipart/form-data; boundary=%s\r\n",bnd);
    int st; size_t n; char*resp=https_fetch("POST","napisy24.pl","/dodaj-napisy",ct,b.b,&st,&n,0); free(b.b);
    *ok = resp && (strstr(resp,"Napisy Dodane/Zmienione")||strstr(resp,"dziękujemy")); free(resp);
    return strdup(*ok?"OK":"Serwer nie potwierdził dodania");
}
static int cmd_n24_upload(const char*cfgpath,const char*srt,const N24Meta*m,int dry){
    if(!srt){ fprintf(stderr,"napisy24 upload wymaga --srt\n"); return 2; }
    size_t rn; char*raw=read_input(srt,&rn); if(!raw){ fprintf(stderr,"Brak pliku: %s\n",srt); return 1; }
    char*text=decode_text((unsigned char*)raw,rn); free(raw);
    char prob[512]; int probs=n24_check_srt(text,prob,sizeof prob);
    if(probs){ fprintf(stderr,"Błąd: Plik nie przejdzie walidacji Napisy24:\n  %s\n",prob); free(text); return 1; }
    if(dry){ printf("[OK] dry-run: plik poprawny (nie wysłano)\n"); free(text); return 0; }
    int wok; char*wmsg=n24_web_upload_run(cfgpath,srt,text,m,&wok); free(text);
    printf("[%s] %s\n", wok?"OK":"BŁĄD", wmsg); free(wmsg); return wok?0:1;
}
static int cmd_n24_delete(const char*cfgpath,const char*id,const char*reason){
    if(!id){ fprintf(stderr,"napisy24 delete wymaga ID\n"); return 2; }
    if(!n24_web_login(cfgpath)) return 2;
    char path[128]; snprintf(path,sizeof path,"/dodaj-napisy?usun=%s",id);
    int st; size_t n; char*g=https_fetch("GET","napisy24.pl",path,NULL,NULL,&st,&n,0); free(g);
    char*er=url_encode(reason?reason:"usuniecie napisu"); SB b; sb_init(&b); char t[256];
    snprintf(t,sizeof t,"form[form_usunPowod]=%s&form[btnSend]=Usu%%C5%%84+napisy&form[usunId]=%s&form[formId]=8",er,id); sb_puts(&b,t); free(er);
    char*resp=https_fetch("POST","napisy24.pl",path,"Content-Type: application/x-www-form-urlencoded\r\n",b.b,&st,&n,0); free(b.b);
    int ok = resp && strstr(resp,"Napisy usunięte"); free(resp);
    printf("[%s] %s\n", ok?"OK":"BŁĄD", ok?"usunięto":"nie potwierdzono usunięcia"); return ok?0:1;
}
/* --- edycja wpisu WWW (/dodaj-napisy?edytuj=<id>) --- */
typedef struct { char name[96]; char value[2048]; } FField;
static void n24_form_short(const char*name,char*out,size_t osz){
    size_t len=strlen(name);
    if(len>=6 && !strncmp(name,"form[",5) && name[len-1]==']'){
        char tmp[160]; size_t k=0; for(size_t i=5;i<len-1 && k<sizeof tmp-1;i++) tmp[k++]=name[i]; tmp[k]=0;
        char t2[160]; size_t o=0; for(size_t i=0;tmp[i];){ if(tmp[i]==']'&&tmp[i+1]=='['){ i+=2; continue; } t2[o++]=tmp[i++]; } t2[o]=0;
        while(o>0 && (t2[o-1]=='['||t2[o-1]==']')) t2[--o]=0;
        snprintf(out,osz,"%s",t2); return; }
    snprintf(out,osz,"%s",name);
}
static void attr_val(const char*attrs,const char*key,char*out,size_t osz){ out[0]=0;
    char pat[24]; snprintf(pat,sizeof pat,"%s=\"",key); const char*p=strstr(attrs,pat); if(!p){return;} p+=strlen(pat);
    size_t k=0; while(*p&&*p!='"'&&k<osz-1) out[k++]=*p++; out[k]=0; }
/* zescrapuj #userForm jako listę (name,value) — jak web_scrape_form. -1 gdy brak formularza. */
static int n24_scrape_form(const char*page,FField*ff,int max,int*count){ *count=0;
    const char*uf=strstr(page,"id=\"userForm\""); if(!uf) return -1;
    const char*fs=strchr(uf,'>'); if(!fs) return -1; fs++;
    const char*fe=strstr(fs,"</form>"); if(!fe) fe=page+strlen(page);
    char radios[48][96]; int nrad=0; const char*p=fs;
    while(p<fe && *count<max){
        const char*ti=strstr(p,"<input"),*ts=strstr(p,"<select"),*tt=strstr(p,"<textarea");
        const char*best=NULL; int kind=0;
        if(ti&&ti<fe&&(!best||ti<best)){best=ti;kind=1;} if(ts&&ts<fe&&(!best||ts<best)){best=ts;kind=2;} if(tt&&tt<fe&&(!best||tt<best)){best=tt;kind=3;}
        if(!best) break; const char*gt=strchr(best,'>'); if(!gt||gt>fe) break;
        char attrs[2048]; size_t al=(size_t)(gt-best); if(al>=sizeof attrs)al=sizeof attrs-1; memcpy(attrs,best,al); attrs[al]=0;
        char name[96]; attr_val(attrs,"name",name,sizeof name);
        if(!name[0]){ p=gt+1; continue; }
        char typ[24]=""; if(kind==1) attr_val(attrs,"type",typ,sizeof typ); else if(kind==2) strcpy(typ,"select"); else strcpy(typ,"textarea");
        if(kind==2){ const char*be=strstr(gt,"</select>"); if(!be)be=fe; char val[2048]="";
            for(const char*o2=strstr(gt,"<option"); o2&&o2<be; o2=strstr(o2+1,"<option")){ const char*oe=strchr(o2,'>'); if(!oe)break;
                char oa[600]; size_t ol=(size_t)(oe-o2); if(ol>=sizeof oa)ol=sizeof oa-1; memcpy(oa,o2,ol); oa[ol]=0;
                if(strstr(oa,"selected")){ attr_val(oa,"value",val,sizeof val); break; } }
            snprintf(ff[*count].name,96,"%s",name); snprintf(ff[*count].value,2048,"%s",val); (*count)++; p=(be<fe?be:gt)+1; continue; }
        if(!strcmp(typ,"file")){ p=gt+1; continue; }
        if(!strcmp(typ,"radio")){ if(strstr(attrs,"checked")){ int seen=0; for(int r=0;r<nrad;r++) if(!strcmp(radios[r],name))seen=1;
            if(!seen){ if(nrad<48) snprintf(radios[nrad++],96,"%s",name); char val[2048]; attr_val(attrs,"value",val,sizeof val);
                snprintf(ff[*count].name,96,"%s",name); snprintf(ff[*count].value,2048,"%s",val); (*count)++; } } p=gt+1; continue; }
        if(!strcmp(typ,"checkbox")){ if(strstr(attrs,"checked")){ char val[2048]; attr_val(attrs,"value",val,sizeof val);
            snprintf(ff[*count].name,96,"%s",name); snprintf(ff[*count].value,2048,"%s",val); (*count)++; } p=gt+1; continue; }
        { char val[2048]; attr_val(attrs,"value",val,sizeof val); html_unescape(val);
          snprintf(ff[*count].name,96,"%s",name); snprintf(ff[*count].value,2048,"%s",val); (*count)++; }
        p=gt+1;
    }
    return 0;
}
static int cmd_n24_edit(const char*cfgpath,const char*napis_id,int show,const char**sets,int nsets,const char*srt){
    if(!napis_id){ fprintf(stderr,"napisy24 edit wymaga id napisu\n"); return 2; }
    if(!n24_web_login(cfgpath)){ fprintf(stderr,"Błąd uwierzytelnienia: napisy24: logowanie nieudane\n"); return 2; }
    char path[128]; snprintf(path,sizeof path,"/dodaj-napisy?edytuj=%s",napis_id);
    int st; size_t n; char*page=https_fetch("GET","napisy24.pl",path,NULL,NULL,&st,&n,0);
    if(!page){ fprintf(stderr,"napisy24: błąd połączenia\n"); return 1; }
    FField*ff=xmalloc(sizeof(FField)*128); int cnt=0;
    if(n24_scrape_form(page,ff,128,&cnt)!=0){ fprintf(stderr,"Błąd: nie znaleziono formularza edycji (nie Twój napis?)\n"); free(ff); free(page); return 1; }
    free(page);
    if(show){ for(int i=0;i<cnt;i++){ char sh[128]; n24_form_short(ff[i].name,sh,sizeof sh); printf("  %-34s = %s\n",sh,ff[i].value); } free(ff); return 0; }
    if(nsets==0 && !srt){ fprintf(stderr,"Nic do zmiany — podaj --set pole=wartość i/lub --srt (użyj --show, by wypisać pola)\n"); free(ff); return 1; }
    /* nadpisania */
    for(int s=0;s<nsets;s++){ const char*eq=strchr(sets[s],'='); if(!eq){ fprintf(stderr,"Błąd: --set oczekuje pole=wartość (dostałem '%s')\n",sets[s]); free(ff); return 1; }
        char key[96]; size_t kl=(size_t)(eq-sets[s]); if(kl>=sizeof key)kl=sizeof key-1; memcpy(key,sets[s],kl); key[kl]=0;
        char*ks=key; while(*ks==' ')ks++; { size_t e=strlen(ks); while(e>0&&ks[e-1]==' ')ks[--e]=0; }
        char sh[128]; n24_form_short(ks,sh,sizeof sh); const char*val=eq+1; int found=0;
        for(int i=0;i<cnt;i++){ char sh2[128]; n24_form_short(ff[i].name,sh2,sizeof sh2); if(!strcmp(sh2,sh)){ snprintf(ff[i].value,2048,"%s",val); found=1; } }
        if(!found && cnt<128){ snprintf(ff[cnt].name,96,"form[%s]",sh); snprintf(ff[cnt].value,2048,"%s",val); cnt++; } }
    /* multipart */
    const char*bnd="----aqnapin24edit"; SB b; sb_init(&b);
    for(int i=0;i<cnt;i++){ sb_puts(&b,"--");sb_puts(&b,bnd);sb_puts(&b,"\r\nContent-Disposition: form-data; name=\"");sb_puts(&b,ff[i].name);sb_puts(&b,"\"\r\n\r\n");sb_puts(&b,ff[i].value);sb_puts(&b,"\r\n"); }
    if(srt){ size_t rn; char*raw=read_input(srt,&rn); if(raw){ char*text=decode_text((unsigned char*)raw,rn); free(raw); size_t fl; unsigned char*fb=n24_normalize_crlf(text,&fl); free(text);
        char fn[512]; input_basename(srt,fn,sizeof fn);
        sb_puts(&b,"--");sb_puts(&b,bnd);sb_puts(&b,"\r\nContent-Disposition: form-data; name=\"form[form_dodajNapis1_plik]\"; filename=\"");sb_puts(&b,fn);sb_puts(&b,"\"\r\nContent-Type: text/plain\r\n\r\n");sb_putn(&b,(char*)fb,fl);sb_puts(&b,"\r\n"); free(fb); } }
    sb_puts(&b,"--");sb_puts(&b,bnd);sb_puts(&b,"--\r\n"); free(ff);
    char extra[256]; snprintf(extra,sizeof extra,"Content-Type: multipart/form-data; boundary=%s\r\nReferer: https://napisy24.pl%s\r\nOrigin: https://napisy24.pl\r\n",bnd,path);
    char*resp=https_fetch("POST","napisy24.pl",path,extra,b.b,&st,&n,0); free(b.b);
    int ok = resp && strstr(resp,"Dodane") && (strstr(resp,"dziękujemy")||strstr(resp,"dziekujemy"));
    if(ok){ printf("Zaktualizowano napis %s. (https://napisy24.pl%s)\n",napis_id,path); free(resp); return 0; }
    fprintf(stderr,"Edycja nieudana: %s\n", resp?"serwer nie potwierdził":"brak odpowiedzi"); free(resp); return 2;
}
#endif /* AQNAPI_TLS */

/* Agregujący upload (multi-serwis) — jak Python cmd_upload. Domyślny serwis: np.
 * Wypisuje "[OK/BŁĄD] serwis: komunikat" w stałej kolejności np -> n24 -> os. */
static int cmd_agg_upload(const char*cfgpath,const char*service,const char*srt,const char*movie,
    const char*lang,const char*translator,int corrected,const char*comment,int dry,int do_login,
    const char*imdb,const char*title,const char*title_pl,const char*year,const char*release,
    const char*a_sync,const char*proof,const char*resolution,const char*duration,const char*a_size,
    const char*fpsstr,const char*season,const char*episode,const char*episode_title){
    if(!srt){ fprintf(stderr,"upload wymaga --srt\n"); return 2; }
    size_t rn; char*raw=read_input(srt,&rn); if(!raw){ fprintf(stderr,"Brak pliku: %s\n",srt); return 1; }
    char*text=decode_text((unsigned char*)raw,rn); free(raw); size_t sl=strlen(text);
    /* parsuj --service (tokenowo, aliasy jak Python); pusty -> np */
    int wantnp=0,wantn24=0,wantos=0,anyvalid=0;
    char svc[128]; snprintf(svc,sizeof svc,"%s",service&&service[0]?service:"np");
    for(char*p=svc;*p;p++)*p=tolower((unsigned char)*p);
    for(char*tok=strtok(svc," ,\t");tok;tok=strtok(NULL," ,\t")){
        if(!strcmp(tok,"napiprojekt")||!strcmp(tok,"np")){ wantnp=1; anyvalid=1; }
        else if(!strcmp(tok,"napisy24")||!strcmp(tok,"n24")){ wantn24=1; anyvalid=1; }
        else if(!strcmp(tok,"opensubtitles")||!strcmp(tok,"os")){ wantos=1; anyvalid=1; } }
    if(!anyvalid){ wantnp=wantn24=wantos=1; }
    int rc=0;
    if(wantnp){
        if(!movie){ printf("[BŁĄD] napiprojekt: napiprojekt upload wymaga --movie (hash pliku filmowego)\n"); rc=1; }
        else {
            char user[128]="",pass[128]=""; int credok=1;
            if(do_login){ np_creds(cfgpath,user,sizeof user,pass,sizeof pass);
                if(!user[0]||!pass[0]){ printf("[BŁĄD] napiprojekt: Upload z logowaniem wymaga loginu i hasła.\n"); rc=1; credok=0; } }
            if(credok){ int ok; char*msg=np_upload_core(movie,text,sl,lang,translator,corrected,comment,dry,do_login,user[0]?user:NULL,pass[0]?pass:NULL,&ok);
                printf("[%s] napiprojekt: %s\n", ok?"OK":"BŁĄD", msg); free(msg); if(!ok) rc=1; } } }
    if(wantn24){
#ifdef AQNAPI_TLS
        char prob[512]; int probs=n24_check_srt(text,prob,sizeof prob);
        if(probs){ printf("[BŁĄD] napisy24: Plik nie przejdzie walidacji Napisy24:\n  %s\n",prob); rc=1; }
        else if(dry){ printf("[OK] napisy24: dry-run: plik poprawny (nie wysłano)\n"); }
        else {
            char login[128],pw[128]; Ini ini; ini_load(config_path(cfgpath),&ini);
            snprintf(login,sizeof login,"%s",ini_get(&ini,0,"login")); snprintf(pw,sizeof pw,"%s",ini_get(&ini,0,"pass"));
            const char*eu=getenv("NAPI24_LOGIN"),*ep=getenv("NAPI24_PASS"); if(eu&&*eu)snprintf(login,sizeof login,"%s",eu); if(ep&&*ep)snprintf(pw,sizeof pw,"%s",ep);
            if(!login[0]||!pw[0]){ printf("[BŁĄD] napisy24: napisy24 upload wymaga loginu i hasła\n"); rc=1; }
            else {
                char szbuf[32]=""; if(a_size&&a_size[0]) snprintf(szbuf,sizeof szbuf,"%s",a_size); else if(movie) snprintf(szbuf,sizeof szbuf,"%ld",input_size(movie));
                char imn[16]=""; if(imdb&&imdb[0]) norm_imdb(imdb,imn,sizeof imn);
                double mfps=0,mdur=0; if(movie) media_from_file(movie,&mfps,&mdur);
                char durb[16]=""; if(duration&&duration[0]) snprintf(durb,sizeof durb,"%s",duration); else hhmmss(mdur,durb,sizeof durb);
                char relb[256]; { char nb[512]; if(movie) input_basename(movie,nb,sizeof nb); else nb[0]=0;
                    n24_release((release&&release[0])?release:nb,relb,sizeof relb); }
                char fpsb[16]="23.976"; if(fpsstr&&fpsstr[0]) snprintf(fpsb,sizeof fpsb,"%s",fpsstr); else if(mfps>0) snprintf(fpsb,sizeof fpsb,"%g",mfps);
                N24Meta m={imn,title,title_pl,year,relb,translator,a_sync,proof,resolution,durb[0]?durb:NULL,szbuf,fpsb,season,episode,episode_title};
                int ok; char*msg=n24_web_upload_run(cfgpath,srt,text,&m,&ok);
                printf("[%s] napisy24: %s\n", ok?"OK":"BŁĄD", msg); free(msg); if(!ok) rc=1; } }
#else
        printf("[BŁĄD] napisy24: upload przez formularz WWW wymaga wariantu TLS (aqnapi-c-tls.com)\n"); rc=1;
#endif
    }
    if(wantos){ printf("[BŁĄD] opensubtitles: Upload do OpenSubtitles nie jest dostępny w REST API (patrz docs/opensubtitles.md)\n"); rc=1; }
    free(text); return rc;
}

/* ----------------------------------------------------------- URL jako wejście
 * Pobieranie zakresowe (Range) — tylko potrzebne fragmenty, minimalna pamięć.
 * https wymaga wariantu TLS; http działa w obu buildach. */
/* Wyszukiwanie podłańcucha bez rozróżniania wielkości liter (bez zależności GNU). */
static const char* ci_strstr(const char*hay,const char*need){ size_t nl=strlen(need);
    for(const char*p=hay;*p;p++){ size_t i=0; while(i<nl && p[i] && tolower((unsigned char)p[i])==tolower((unsigned char)need[i])) i++; if(i==nl) return p; } return NULL; }
static void url_pct_decode(const char*s,char*out,size_t osz){ size_t o=0;
    for(size_t i=0;s[i]&&o+1<osz;i++){ if(s[i]=='%'&&isxdigit((unsigned char)s[i+1])&&isxdigit((unsigned char)s[i+2])){
            char h[3]={s[i+1],s[i+2],0}; out[o++]=(char)strtol(h,NULL,16); i+=2; } else out[o++]=s[i]; }
    out[o]=0; }
/* Rozbij URL na części; zbuduj nagłówek Basic auth z user:pass@. Zwraca 1 OK. */
static int url_parse(const char*url,int*is_https,char*authhdr,size_t ahsz,
                     char*host,size_t hsz,char*port,size_t psz,char*path,size_t pathsz){
    const char*p; authhdr[0]=0;
    if(!strncmp(url,"https://",8)){ *is_https=1; p=url+8; snprintf(port,psz,"443"); }
    else if(!strncmp(url,"http://",7)){ *is_https=0; p=url+7; snprintf(port,psz,"80"); }
    else return 0;
    const char*slash=p; while(*slash && *slash!='/') slash++;
    char auth[512]; size_t al=(size_t)(slash-p); if(al>=sizeof auth) al=sizeof auth-1; memcpy(auth,p,al); auth[al]=0;
    char*at=strrchr(auth,'@'); char*hp=auth;
    if(at){ *at=0; char ui[512]; char dec[512]; url_pct_decode(auth,dec,sizeof dec);
        if(strchr(dec,':')) snprintf(ui,sizeof ui,"%s",dec); else snprintf(ui,sizeof ui,"%s:",dec);
        char*tok=b64encode((const unsigned char*)ui,strlen(ui)); snprintf(authhdr,ahsz,"Authorization: Basic %s\r\n",tok); free(tok);
        hp=at+1; }
    char*colon=strrchr(hp,':'); if(colon){ *colon=0; snprintf(port,psz,"%s",colon+1); }
    snprintf(host,hsz,"%s",hp);
    if(*slash) snprintf(path,pathsz,"%s",slash); else snprintf(path,pathsz,"/");
    return 1;
}
/* Zwykły HTTP/1.1 GET/HEAD z parsowaniem statusu, rozmiaru i chunked. */
static char* url_http_req(const char*method,const char*host,const char*port,const char*path,
                          const char*extra,size_t*outlen,int*status,long*total){
    struct addrinfo hints,*res=NULL; memset(&hints,0,sizeof hints); hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(host,port,&hints,&res)!=0) return NULL;
    int fd=-1; for(struct addrinfo*a=res;a;a=a->ai_next){ fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol); if(fd<0)continue; if(connect(fd,a->ai_addr,a->ai_addrlen)==0)break; close(fd); fd=-1; }
    freeaddrinfo(res); if(fd<0) return NULL;
    SB req; sb_init(&req); char h[1200];
    snprintf(h,sizeof h,"%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: aqnapi v%s\r\nAccept: */*\r\n",method,path,host,VERSION);
    sb_puts(&req,h); if(extra) sb_puts(&req,extra); sb_puts(&req,"Connection: close\r\n\r\n");
    size_t off=0; while(off<req.len){ ssize_t w=write(fd,req.b+off,req.len-off); if(w<=0){ close(fd); free(req.b); return NULL; } off+=w; }
    free(req.b);
    SB resp; sb_init(&resp); char buf[8192]; ssize_t r; while((r=read(fd,buf,sizeof buf))>0) sb_putn(&resp,buf,(size_t)r); close(fd);
    if(!resp.b){ return NULL; }
    int st=0; sscanf(resp.b,"HTTP/1.%*d %d",&st);
    char*he=strstr(resp.b,"\r\n\r\n"); if(!he){ free(resp.b); return NULL; }
    *he=0; char*hdrs=resp.b; char*bodyp=he+4; size_t blen=resp.len-(size_t)(bodyp-resp.b);
    long tot=-1; for(char*q=hdrs;*q;q++){ if((q==hdrs||q[-1]=='\n')&&!strncasecmp(q,"content-range:",14)){ char*sl=strchr(q,'/'); if(sl) tot=strtol(sl+1,NULL,10); break; } }
    if(tot<0) for(char*q=hdrs;*q;q++){ if((q==hdrs||q[-1]=='\n')&&!strncasecmp(q,"content-length:",15)){ tot=strtol(q+15,NULL,10); break; } }
    int chunked=0; for(char*q=hdrs;*q;q++) if((q==hdrs||q[-1]=='\n')&&!strncasecmp(q,"transfer-encoding:",18)&&ci_strstr(q,"chunked")) chunked=1;
    SB out; sb_init(&out);
    if(chunked){ char*q=bodyp; size_t left=blen; while(left>0){ char*eol=memchr(q,'\n',left); if(!eol)break; long csz=strtol(q,NULL,16); size_t adv=(size_t)(eol-q)+1; q+=adv; left-=adv; if(csz<=0)break; if((size_t)csz>left)csz=left; sb_putn(&out,q,(size_t)csz); q+=csz; left-=(size_t)csz; if(left>=2){q+=2;left-=2;} } }
    else sb_putn(&out,bodyp,blen);
    sb_putc(&out,0); size_t bl=out.len-1; /* NUL-terminacja bez liczenia */
    if(status)*status=st; if(total)*total=tot; if(outlen)*outlen=bl;
    char*b=out.b; free(resp.b); return b;
}
/* Uniwersalny fetch (GET/HEAD) po http lub https, z nagłówkiem Range. */
static char* url_fetch(const char*method,const char*url,const char*rangehdr,size_t*outlen,int*status,long*total){
    int https; char authhdr[600],host[256],port[8],path[2048];
    if(!url_parse(url,&https,authhdr,sizeof authhdr,host,sizeof host,port,sizeof port,path,sizeof path)){ fprintf(stderr,"Zły URL: %s\n",url); return NULL; }
    char extra[900]; snprintf(extra,sizeof extra,"%s%s",authhdr,rangehdr?rangehdr:"");
    if(https){
#ifdef AQNAPI_TLS
        int st=0; size_t n=0; char*b=https_fetch(method,host,path,extra[0]?extra:NULL,NULL,&st,&n,0);
        if(status)*status=st; if(outlen)*outlen=n; if(total)*total=g_http_total; return b;
#else
        fprintf(stderr,"URL https wymaga wariantu TLS (aqnapi-c-tls.com): %s\n",url); return NULL;
#endif
    }
    return url_http_req(method,host,port,path,extra[0]?extra:NULL,outlen,status,total);
}
static long url_size(const char*url){
    int st=0; size_t n=0; long tot=-1; char*b=url_fetch("HEAD",url,NULL,&n,&st,&tot); free(b);
    if(tot<0){ /* fallback: GET Range 0-0 */ b=url_fetch("GET",url,"Range: bytes=0-0\r\n",&n,&st,&tot); free(b); }
    if(tot<0) fprintf(stderr,"Serwer nie podał rozmiaru zasobu: %s\n",url);
    return tot;
}
static unsigned char* url_read_range(const char*url,long start,long length,size_t*outlen){
    char rh[64]; snprintf(rh,sizeof rh,"Range: bytes=%ld-%ld\r\n",start,start+length-1);
    int st=0; size_t n=0; char*b=url_fetch("GET",url,rh,&n,&st,NULL); if(!b) return NULL;
    if(start>0 && st!=206){ fprintf(stderr,"Serwer nie wspiera żądań zakresowych (Range) — HTTP %d: %s\n",st,url); free(b); return NULL; }
    if(outlen)*outlen = n<(size_t)length ? n : (size_t)length;
    return (unsigned char*)b;
}
static unsigned char* url_read_full(const char*url,size_t*outlen){
    int st=0; size_t n=0; char*b=url_fetch("GET",url,NULL,&n,&st,NULL); if(!b) return NULL;
    if(st>=400){ fprintf(stderr,"Pobranie URL nieudane (HTTP %d): %s\n",st,url); free(b); return NULL; }
    if(outlen)*outlen=n; return (unsigned char*)b;
}

/* ---------------------------------------------------------- napisy24 attach
 * AddSubPrg.php — działająca ścieżka API klienta ("Dodaj napisy (tylko do
 * programu)"). Dwufazowo (Check → Send) przez plain HTTP; powiązanie po haszu
 * filmu, bez publicznego wpisu. Pola login/pass/hm/md/hs/fs/tm/dm/fp/im są
 * zaciemniane (n24_obf), fn jawne (utf-8), sf to surowy plik. postVer=v1.99.1. */
#define N24_POSTVER "v1.99.1"
static void prg_field(SB*b,const char*bnd,const char*name,const char*value,const char*charset){
    sb_puts(b,"--"); sb_puts(b,bnd); sb_puts(b,"\r\nContent-Disposition: form-data; name=\""); sb_puts(b,name); sb_puts(b,"\"\r\n");
    if(charset){ sb_puts(b,"Content-Type: text/plain; charset="); sb_puts(b,charset); sb_puts(b,"\r\n"); }
    sb_puts(b,"Content-Transfer-Encoding: 8bit\r\n\r\n"); sb_puts(b,value); sb_puts(b,"\r\n"); }
#define PRG_OBF(b,bnd,name,val) do{ char*_o=n24_obf(val); prg_field(b,bnd,name,_o,NULL); free(_o); }while(0)
static char* n24_prg_post_ep(const char*endpoint,const char*body,size_t blen,const char*boundary){
    SB req; sb_init(&req); char hdr[400];
    snprintf(hdr,sizeof hdr,"POST /run/%s HTTP/1.0\r\nHost: napisy24.pl\r\nUser-Agent: Mozilla/4.0\r\nAccept: */*\r\nContent-Type: multipart/form-data; boundary=%s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",endpoint,boundary,blen);
    sb_puts(&req,hdr); sb_putn(&req,body,blen);
    size_t bl; char*resp=http_request("napisy24.pl",req.b,req.len,&bl); free(req.b);
    if(!resp) return NULL; char*dec=utf8_replace(resp); free(resp); return dec; }
static char* n24_prg_post(const char*body,size_t blen,const char*boundary){ return n24_prg_post_ep("AddSubPrg.php",body,blen,boundary); }
static const char* skip_ws(const char*s){ while(*s==' '||*s=='\r'||*s=='\n'||*s=='\t')s++; return s; }
static int cmd_n24_attach(const char*cfgpath,const char*movie,const char*srt,const char*imdb,
                          const char*duration,const char*resolution,const char*fpsflag,int check_only){
    if(!movie||!srt){ fprintf(stderr,"napisy24 attach wymaga --movie i --srt\n"); return 2; }
    Ini ini; ini_load(config_path(cfgpath),&ini);
    char login[128],pass[128];
    snprintf(login,sizeof login,"%s",ini_get(&ini,0,"login")); snprintf(pass,sizeof pass,"%s",ini_get(&ini,0,"pass"));
    const char*eu=getenv("NAPI24_LOGIN"),*ep=getenv("NAPI24_PASS"); if(eu&&*eu)snprintf(login,sizeof login,"%s",eu); if(ep&&*ep)snprintf(pass,sizeof pass,"%s",ep);
    if(!login[0]||!pass[0]){ fprintf(stderr,"Błąd uwierzytelnienia: napisy24 attach wymaga loginu i hasła\n"); return 2; }
    char osh[17]; int r=oshash(movie,osh);
    if(r==-1){ fprintf(stderr,"Błąd: plik za mały na hash OSH: %s\n",movie); return 1; }
    if(r==-2){ fprintf(stderr,"Brak pliku: %s\n",movie); return 1; }
    for(char*p=osh;*p;p++)*p=toupper((unsigned char)*p);
    char md[33]; if(md5_10mb(movie,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); return 1; }
    char fs[32]; snprintf(fs,sizeof fs,"%ld",input_size(movie));
    char mvname[512]; input_basename(movie,mvname,sizeof mvname);
    size_t sn=0; char*srtdata=read_input(srt,&sn); if(!srtdata){ fprintf(stderr,"Brak pliku: %s\n",srt); return 1; }
    char hs[17]; subtitle_hash((unsigned char*)srtdata,sn,hs);
    char srtname[512]; input_basename(srt,srtname,sizeof srtname);
    char imn[16]=""; int have_im=0; if(imdb&&imdb[0]){ norm_imdb(imdb,imn,sizeof imn); if(strcmp(imn,"0")!=0) have_im=1; }
    const char*bnd="----aqnapiprg000001";
    /* Faza 1: Check (read-only) */
    SB b; sb_init(&b);
    prg_field(&b,bnd,"postAction","Check",NULL); prg_field(&b,bnd,"postVer",N24_POSTVER,NULL);
    PRG_OBF(&b,bnd,"login",login); PRG_OBF(&b,bnd,"pass",pass);
    PRG_OBF(&b,bnd,"hm",osh); PRG_OBF(&b,bnd,"md",md); PRG_OBF(&b,bnd,"hs",hs); PRG_OBF(&b,bnd,"fs",fs);
    if(have_im) PRG_OBF(&b,bnd,"im",imn);
    sb_puts(&b,"--"); sb_puts(&b,bnd); sb_puts(&b,"--\r\n");
    char*resp=n24_prg_post(b.b,b.len,bnd); free(b.b);
    if(!resp){ free(srtdata); fprintf(stderr,"napisy24: błąd połączenia\n"); return 1; }
    const char*v=skip_ws(resp); char vsave[8]; snprintf(vsave,sizeof vsave,"%.4s",v);
    int ok2=!strncmp(v,"OK-2",4), ok0=!strncmp(v,"OK-0",4), ok1=!strncmp(v,"OK-1",4);
    if(ok2){ printf("[BŁĄD] napisy już w bazie (duplikat): %.40s\n",v); free(resp); free(srtdata); return 1; }
    if(!ok0&&!ok1){ printf("[BŁĄD] serwer odrzucił fazę Check: %.60s\n",v); free(resp); free(srtdata); return 1; }
    if(check_only){ printf("[OK] check-only (%s = %s): nic nie wysłano\n",vsave, ok0?"nowe":"film znany");
        printf("(faza Check, read-only: OK-0 = nowe, OK-1 = film znany, OK-2 = duplikat)\n"); free(resp); free(srtdata); return 0; }
    free(resp);
    char fpsbuf[32]=""; if(fpsflag&&fpsflag[0]) snprintf(fpsbuf,sizeof fpsbuf,"%s",fpsflag);
    else { double f=fps_from_file(movie); if(f>0) snprintf(fpsbuf,sizeof fpsbuf,"%.3f",f); }
    /* Faza 2: Send */
    SB s; sb_init(&s);
    prg_field(&s,bnd,"postAction","Send",NULL); prg_field(&s,bnd,"postVer",N24_POSTVER,NULL);
    PRG_OBF(&s,bnd,"login",login); PRG_OBF(&s,bnd,"pass",pass);
    PRG_OBF(&s,bnd,"hm",osh); PRG_OBF(&s,bnd,"md",md); PRG_OBF(&s,bnd,"hs",hs); PRG_OBF(&s,bnd,"fs",fs);
    PRG_OBF(&s,bnd,"tm",duration?duration:""); PRG_OBF(&s,bnd,"dm",resolution?resolution:""); PRG_OBF(&s,bnd,"fp",fpsbuf);
    if(have_im) PRG_OBF(&s,bnd,"im",imn);
    prg_field(&s,bnd,"fn",mvname,"utf-8");
    if(ok0){ sb_puts(&s,"--"); sb_puts(&s,bnd); sb_puts(&s,"\r\nContent-Disposition: form-data; name=\"sf\"; filename=\"");
        sb_puts(&s,srtname); sb_puts(&s,"\"\r\nContent-Type: text/plain\r\nContent-Transfer-Encoding: 8bit\r\n\r\n"); sb_putn(&s,srtdata,sn); sb_puts(&s,"\r\n"); }
    sb_puts(&s,"--"); sb_puts(&s,bnd); sb_puts(&s,"--\r\n");
    char*resp2=n24_prg_post(s.b,s.len,bnd); free(s.b); free(srtdata);
    if(!resp2){ fprintf(stderr,"napisy24: błąd połączenia\n"); return 1; }
    const char*v2=skip_ws(resp2); int ok=!strncmp(v2,"OK",2);
    printf("[%s] %s | %.80s\n", ok?"OK":"BŁĄD", vsave, v2);
    free(resp2); return ok?0:1;
}

/* napisy24 multipart klienta (_multipart_run): każde pole Content-Type text/plain
 * + Content-Transfer-Encoding: 8bit; boundary jak w kliencie; POST /run/<endpoint>. */
#define N24_MRUN_BND "--------071926211419984"
static void mrun_field(SB*b,const char*name,const char*val){
    sb_puts(b,"--" N24_MRUN_BND "\r\nContent-Disposition: form-data; name=\"");
    sb_puts(b,name); sb_puts(b,"\"\r\nContent-Type: text/plain\r\nContent-Transfer-Encoding: 8bit\r\n\r\n");
    sb_puts(b,val); sb_puts(b,"\r\n"); }
static char* n24_mrun_post(const char*endpoint,SB*body){
    sb_puts(body,"--" N24_MRUN_BND "--");
    char hdr[400]; snprintf(hdr,sizeof hdr,"POST /run/%s HTTP/1.0\r\nHost: napisy24.pl\r\nUser-Agent: Mozilla/4.0\r\nAccept: */*\r\nContent-Type: multipart/form-data; boundary=" N24_MRUN_BND "\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",endpoint,body->len);
    SB req; sb_init(&req); sb_puts(&req,hdr); sb_putn(&req,body->b,body->len);
    size_t bl; char*resp=http_request("napisy24.pl",req.b,req.len,&bl); free(req.b);
    if(!resp) return NULL; char*dec=utf8_replace(resp); free(resp); return dec; }
/* napisy24 login klienta: CheckLogin.php (pola login/pass zaciemniane) */
static int cmd_n24_login(const char*cfgpath){
    Ini ini; ini_load(config_path(cfgpath),&ini);
    char login[128],pass[128]; snprintf(login,sizeof login,"%s",ini_get(&ini,0,"login")); snprintf(pass,sizeof pass,"%s",ini_get(&ini,0,"pass"));
    const char*eu=getenv("NAPI24_LOGIN"),*ep=getenv("NAPI24_PASS"); if(eu&&*eu)snprintf(login,sizeof login,"%s",eu); if(ep&&*ep)snprintf(pass,sizeof pass,"%s",ep);
    SB b; sb_init(&b);
    mrun_field(&b,"postAction","Logowanie"); mrun_field(&b,"postVer","v1.99.1");
    char*ol=n24_obf(login); mrun_field(&b,"login",ol); free(ol);
    char*op=n24_obf(pass); mrun_field(&b,"pass",op); free(op);
    char*resp=n24_mrun_post("CheckLogin.php",&b); free(b.b);
    if(!resp) die("napisy24: błąd połączenia");
    strip_inplace(resp); printf("%s\n",resp);
    int ok=!strncmp(resp,"login=ok",8); free(resp); return ok?0:1;
}
/* napisy24 imdb: CheckIMDB.php (imdbId jawne, znormalizowane) -> surowy tekst */
static int cmd_n24_imdb(const char*imdb){
    char nb[16]; norm_imdb(imdb?imdb:"",nb,sizeof nb);
    SB b; sb_init(&b); mrun_field(&b,"imdbId",nb);
    char*resp=n24_mrun_post("CheckIMDB.php",&b); free(b.b);
    if(!resp) die("napisy24: błąd połączenia");
    strip_inplace(resp); printf("%s\n",resp); free(resp); return 0;
}
/* creds napisy24: config [napisy24] login/pass + env NAPI24_LOGIN/NAPI24_PASS */
static void n24_creds(const char*cfgpath,char*login,size_t ls,char*pw,size_t ps){
    Ini ini; ini_load(config_path(cfgpath),&ini);
    snprintf(login,ls,"%s",ini_get(&ini,0,"login")); snprintf(pw,ps,"%s",ini_get(&ini,0,"pass"));
    const char*eu=getenv("NAPI24_LOGIN"),*ep=getenv("NAPI24_PASS"); if(eu&&*eu)snprintf(login,ls,"%s",eu); if(ep&&*ep)snprintf(pw,ps,"%s",ep);
}
/* CheckSub2.php -> id rekordu `lp` z nagłówka odpowiedzi (dla mediainfo). Zwraca 1 gdy znaleziono. */
static int n24_checksub_lp(const char*movie,const char*lang,char*out,size_t osz){ out[0]=0;
    char osh[17]; if(oshash(movie,osh)!=0) return 0; for(char*p=osh;*p;p++)*p=toupper((unsigned char)*p);
    char md[33]; if(md5_10mb(movie,md)!=0) return 0;
    char fs[32]; snprintf(fs,sizeof fs,"%ld",input_size(movie));
    char fn[512]; input_basename(movie,fn,sizeof fn);
    SB b; sb_init(&b); char*o;
    mrun_field(&b,"postAction","CheckSub"); mrun_field(&b,"postVer","v1.99.1");
    o=n24_obf(osh); mrun_field(&b,"fh",o); free(o); o=n24_obf(md); mrun_field(&b,"md",o); free(o);
    o=n24_obf(fs); mrun_field(&b,"fs",o); free(o); o=n24_obf(fn); mrun_field(&b,"fn",o); free(o);
    o=n24_obf(lang); mrun_field(&b,"nl",o); free(o); mrun_field(&b,"n24pref","1"); mrun_field(&b,"licz","1");
    char*resp=n24_mrun_post("CheckSub2.php",&b); free(b.b); if(!resp) return 0;
    char*sep=strstr(resp,"||"); if(sep)*sep=0; /* tylko nagłówek */
    for(char*tok=strtok(resp,"|");tok;tok=strtok(NULL,"|")){ if(!strncmp(tok,"lp:",3)){ snprintf(out,osz,"%s",tok+3); break; } }
    free(resp); return out[0]?1:0;
}
/* mediainfo: ChangeData.php — powiąż hash+media info z rekordem lp */
static int cmd_n24_mediainfo(const char*cfgpath,const char*movie,const char*id,const char*duration,const char*resolution,const char*fpsflag,const char*lang){
    if(!movie){ fprintf(stderr,"napisy24 mediainfo wymaga --movie\n"); return 2; }
    char login[128],pw[128]; n24_creds(cfgpath,login,sizeof login,pw,sizeof pw);
    if(!login[0]||!pw[0]){ fprintf(stderr,"Błąd uwierzytelnienia: napisy24 mediainfo wymaga loginu i hasła\n"); return 2; }
    const char*L=lang&&lang[0]?lang:"pl";
    char recid[64]="";
    if(id&&id[0]) snprintf(recid,sizeof recid,"%s",id);
    else { char lp[64]; if(!n24_checksub_lp(movie,L,lp,sizeof lp)){ fprintf(stderr,"Brak id `lp` dla tego filmu (CheckSub2 nic nie znalazł) — podaj --id\n"); return 2; }
        snprintf(recid,sizeof recid,"%s",lp); printf("Rozpoznano id rekordu (lp): %s\n",recid); }
    char osh[17]; if(oshash(movie,osh)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); return 1; } for(char*p=osh;*p;p++)*p=toupper((unsigned char)*p);
    char md[33]; if(md5_10mb(movie,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); return 1; }
    char fs[32]; snprintf(fs,sizeof fs,"%ld",input_size(movie)); char fn[512]; input_basename(movie,fn,sizeof fn);
    char fpsbuf[32]=""; if(fpsflag&&fpsflag[0]) snprintf(fpsbuf,sizeof fpsbuf,"%s",fpsflag); else { double f=fps_from_file(movie); if(f>0) snprintf(fpsbuf,sizeof fpsbuf,"%.3f",f); }
    char data[256]; snprintf(data,sizeof data,"%s|%s|%s|%s",recid,duration?duration:"",resolution?resolution:"",fpsbuf);
    const char*bnd="----aqnapiprg000001"; SB b; sb_init(&b);
    PRG_OBF(&b,bnd,"type","mediainfo"); prg_field(&b,bnd,"postVer","v1.99.1",NULL);
    PRG_OBF(&b,bnd,"login",login); PRG_OBF(&b,bnd,"pass",pw); PRG_OBF(&b,bnd,"data",data);
    PRG_OBF(&b,bnd,"fh",osh); PRG_OBF(&b,bnd,"md",md); PRG_OBF(&b,bnd,"fs",fs); PRG_OBF(&b,bnd,"fn",fn);
    prg_field(&b,bnd,"n24pref","1",NULL); PRG_OBF(&b,bnd,"nl",L);
    sb_puts(&b,"--"); sb_puts(&b,bnd); sb_puts(&b,"--\r\n");
    char*resp=n24_prg_post_ep("ChangeData.php",b.b,b.len,bnd); free(b.b);
    if(!resp) die("napisy24: błąd połączenia"); strip_inplace(resp);
    printf("Serwer: %s\n",resp); int ok=!strncmp(resp,"data=ok",7); free(resp); return ok?0:2;
}
/* notify: Notifiemail.php / NotifiSMS.php — powiadomienie o napisach */
static int cmd_n24_notify(const char*cfgpath,const char*movie,int off,const char*sms_code,const char*lang){
    if(!movie){ fprintf(stderr,"napisy24 notify wymaga --movie\n"); return 2; }
    char login[128],pw[128]; n24_creds(cfgpath,login,sizeof login,pw,sizeof pw);
    if(!login[0]||!pw[0]){ fprintf(stderr,"Błąd uwierzytelnienia: napisy24 notify wymaga loginu i hasła\n"); return 2; }
    const char*L=lang&&lang[0]?lang:"pl";
    char osh[17]; if(oshash(movie,osh)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); return 1; } for(char*p=osh;*p;p++)*p=toupper((unsigned char)*p);
    char md[33]; if(md5_10mb(movie,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); return 1; }
    char fs[32]; snprintf(fs,sizeof fs,"%ld",input_size(movie));
    const char*bnd="----aqnapiprg000001"; SB b; sb_init(&b);
    prg_field(&b,bnd,"postAction","SetNotifi",NULL); prg_field(&b,bnd,"postVer","v1.99.1",NULL);
    PRG_OBF(&b,bnd,"login",login); PRG_OBF(&b,bnd,"pass",pw); PRG_OBF(&b,bnd,"data",off?"0":"1");
    if(sms_code){ PRG_OBF(&b,bnd,"code",sms_code); }
    PRG_OBF(&b,bnd,"fh",osh); PRG_OBF(&b,bnd,"md",md); PRG_OBF(&b,bnd,"fs",fs); PRG_OBF(&b,bnd,"nl",L);
    sb_puts(&b,"--"); sb_puts(&b,bnd); sb_puts(&b,"--\r\n");
    char*resp=n24_prg_post_ep(sms_code?"NotifiSMS.php":"Notifiemail.php",b.b,b.len,bnd); free(b.b);
    if(!resp) die("napisy24: błąd połączenia"); strip_inplace(resp);
    printf("Serwer: %s\n",resp); int ok=!strncmp(resp,"data=ok",7); free(resp); return ok?0:2;
}
/* trans: GetTrans.php (lista) / SetTrans.php (--set) */
static int cmd_n24_trans(const char*cfgpath,const char*user_id,const char*set_id,const char*info,const char*progress){
    const char*bnd="----aqnapiprg000001";
    if(set_id){
        char login[128],pw[128]; n24_creds(cfgpath,login,sizeof login,pw,sizeof pw);
        if(!login[0]||!pw[0]){ fprintf(stderr,"Błąd uwierzytelnienia: napisy24 trans --set wymaga loginu i hasła\n"); return 2; }
        SB b; sb_init(&b);
        PRG_OBF(&b,bnd,"type","mediainfo"); prg_field(&b,bnd,"postVer","v1.99.1",NULL);
        PRG_OBF(&b,bnd,"login",login); PRG_OBF(&b,bnd,"pass",pw);
        prg_field(&b,bnd,"userId",user_id?user_id:"",NULL); PRG_OBF(&b,bnd,"transid",set_id);
        prg_field(&b,bnd,"info",info?info:"","utf-8"); PRG_OBF(&b,bnd,"progress",progress?progress:"");
        sb_puts(&b,"--"); sb_puts(&b,bnd); sb_puts(&b,"--\r\n");
        char*resp=n24_prg_post_ep("SetTrans.php",b.b,b.len,bnd); free(b.b);
        if(!resp) die("napisy24: błąd połączenia"); strip_inplace(resp);
        printf("Serwer: %s\n",resp); int ok=!strncmp(resp,"data=ok",7); free(resp); return ok?0:2;
    }
    SB b; sb_init(&b);
    prg_field(&b,bnd,"PostAction","GET",NULL); prg_field(&b,bnd,"userId",user_id?user_id:"",NULL);
    sb_puts(&b,"--"); sb_puts(&b,bnd); sb_puts(&b,"--\r\n");
    char*resp=n24_prg_post_ep("GetTrans.php",b.b,b.len,bnd); free(b.b);
    if(!resp) die("napisy24: błąd połączenia"); strip_inplace(resp);
    /* rekordy '~', pola '^' (strtok_r — dwa poziomy) */
    int cnt=0; { char*tmp=strdup(resp),*sv=NULL; for(char*r=strtok_r(tmp,"~",&sv);r;r=strtok_r(NULL,"~",&sv)){ char*s=r; while(*s==' '||*s=='\r'||*s=='\n'||*s=='\t')s++; if(*s)cnt++; } free(tmp); }
    if(cnt==0){ printf("Brak projektów tłumaczeń dla userId=%s.\n",user_id?user_id:""); free(resp); return 0; }
    printf("Projekty tłumaczeń (%d):\n",cnt);
    char*sv1=NULL;
    for(char*r=strtok_r(resp,"~",&sv1);r;r=strtok_r(NULL,"~",&sv1)){ char*s=r; while(*s==' '||*s=='\r'||*s=='\n'||*s=='\t')s++; if(!*s) continue;
        SB line; sb_init(&line); int first=1; char*sv2=NULL; for(char*f=strtok_r(s,"^",&sv2);f;f=strtok_r(NULL,"^",&sv2)){ if(!first) sb_puts(&line," | "); sb_puts(&line,f); first=0; }
        printf("  %s\n",line.b?line.b:""); free(line.b); }
    free(resp); return 0;
}
/* premieres: GetIMDB.php (PostAction=GET) */
static int cmd_n24_premieres(void){
    const char*bnd="----aqnapiprg000001"; SB b; sb_init(&b);
    prg_field(&b,bnd,"PostAction","GET",NULL);
    sb_puts(&b,"--"); sb_puts(&b,bnd); sb_puts(&b,"--\r\n");
    char*resp=n24_prg_post_ep("GetIMDB.php",b.b,b.len,bnd); free(b.b);
    if(!resp) die("napisy24: błąd połączenia"); strip_inplace(resp);
    printf("%s\n", resp[0]?resp:"(pusta odpowiedź)"); free(resp); return 0;
}

static int cmd_update(int check){
#ifdef AQNAPI_TLS
    int st; size_t n;
    char*j=https_fetch("GET","api.github.com","/repos/areqq/aqnapi/releases/latest",
                       "Accept: application/vnd.github+json\r\n",NULL,&st,&n,0);
    if(!j||st!=200){ fprintf(stderr,"Błąd: GitHub API HTTP %d\n",j?st:0); free(j); return 1; }
    char tag[32]; json_str(j,"tag_name",tag,sizeof tag); const char*latest=tag[0]=='v'?tag+1:tag;
    /* porównanie wersji (major.minor.patch) */
    int lv[3]={0,0,0},cv[3]={0,0,0}; sscanf(latest,"%d.%d.%d",&lv[0],&lv[1],&lv[2]); sscanf(VERSION,"%d.%d.%d",&cv[0],&cv[1],&cv[2]);
    int newer=(lv[0]>cv[0])||(lv[0]==cv[0]&&(lv[1]>cv[1]||(lv[1]==cv[1]&&lv[2]>cv[2])));
    if(!newer){ printf("Masz najnowszą wersję (v%s).\n",VERSION); free(j); return 0; }
    printf("Dostępna nowsza wersja: v%s (masz v%s)\n",latest,VERSION);
    if(check){ free(j); return 0; }
    /* znajdź asset aqnapi-c.com i pobierz */
    char*ap=strstr(j,"\"aqnapi-c.com\""); char durl[1024]={0};
    if(ap){ char*bp=strstr(ap,"browser_download_url"); if(bp) json_str(bp,"browser_download_url",durl,sizeof durl); }
    if(!durl[0]){ fprintf(stderr,"Nie znaleziono artefaktu aqnapi-c.com w wydaniu.\n"); free(j); return 1; }
    free(j);
    const char*u=durl; if(!strncmp(u,"https://",8))u+=8; char host[256],path[1024]; const char*sl=strchr(u,'/');
    size_t hl=sl?(size_t)(sl-u):strlen(u); if(hl>=sizeof host)hl=sizeof host-1; memcpy(host,u,hl); host[hl]=0; snprintf(path,sizeof path,"%s",sl?sl:"/");
    printf("Pobieram aqnapi-c.com …\n"); size_t bl; int st2;
    char*bin=https_fetch("GET",host,path,NULL,NULL,&st2,&bl,0);
    if(!bin||st2!=200||bl<1000){ fprintf(stderr,"Pobieranie nie powiodło się (HTTP %d).\n",st2); free(bin); return 1; }
    /* podmień samego siebie */
    char self[1024]; ssize_t sl2=readlink("/proc/self/exe",self,sizeof self-1); if(sl2>0) self[sl2]=0; else snprintf(self,sizeof self,"%s","aqnapi-c.com");
    char tmp[1100]; snprintf(tmp,sizeof tmp,"%s.new",self); FILE*f=fopen(tmp,"wb"); if(!f){ fprintf(stderr,"Nie mogę zapisać.\n"); free(bin); return 1; }
    fwrite(bin,1,bl,f); fclose(f); free(bin); chmod(tmp,0755); if(rename(tmp,self)!=0){ fprintf(stderr,"Nie mogę podmienić %s\n",self); return 1; }
    printf("Zaktualizowano do v%s: %s\n",latest,self); return 0;
#else
    (void)check; fprintf(stderr,"update: ta binarka nie ma TLS (build cosmocc). Użyj binarki z monorepo (mbedtls) albo aqnapi.py.\n"); return 2;
#endif
}

static void usage(void){
    printf("aqnapi %s (wersja C)\n"
        "Użycie:\n"
        "  aqnapi hash PLIK\n"
        "  aqnapi fps PLIK\n"
        "  aqnapi convert WEJŚCIE [-o WYJ] [--format srt|vtt|ass|microdvd] [--movie FILM] [--fps F]\n"
        "                 [--strip-sdh] [--keep-tags] [--no-sanitize] [--max-display S] [--min-display S]\n"
        "  aqnapi fpsconv WEJŚCIE --from F [--to F | --movie FILM] [-o WYJ] [--format ...]\n"
        "  aqnapi merge PLIK PLIK [...] [-o WYJ] [--offset S ...] [--format ...]\n"
        "  aqnapi split WEJŚCIE --at CZAS [--at CZAS ...] [-o BAZA] [--no-rebase] [--format ...]\n"
        "  aqnapi download FILM [-l PL] [-o WYJ] [--fps F]      (napiprojekt, HTTP)\n"
        "  aqnapi napiprojekt upload --movie FILM --srt PLIK [-l PL] [--translator A]\n"
        "                            [--corrected] [--comment K] [--login] [--dry-run]\n"
        "  aqnapi napisy24 attach --movie FILM --srt PLIK [--imdb tt..] [--check-only]\n"
        "                         [--duration HH:MM:SS] [--resolution WxH] [--fps F]\n"
        "  aqnapi --version | --help\n"
        "napiprojekt upload buduje 7z-AES natywnie w C; --login przypisuje do konta.\n"
        "napisy24 attach (AddSubPrg.php) powiązuje napisy po haszu filmu (bez wpisu publicznego).\n"
        "TLS (OpenSubtitles, napisy24 WWW) i interaktywny sync — w wariancie aqnapi-c-tls.com / Python.\n", VERSION);
}

int main(int argc,char**argv){
    const char*cmd=NULL,*out=NULL,*movie=NULL,*lang=NULL,*fmt=NULL,*cfgpath=NULL,*title=NULL,*imdb=NULL,*query=NULL,*season=NULL,*episode=NULL,
        *release=NULL,*resolution=NULL,*duration=NULL,*a_size=NULL,*year=NULL,
        *title_pl=NULL,*episode_title=NULL,*a_sync=NULL,*proof=NULL,*reason=NULL,*service=NULL,
        *rec_id=NULL,*sms_code=NULL,*info=NULL,*progress=NULL,*set_val=NULL;
    int notify_off=0, show_fields=0, rep_kind=0, rep_list=0;
    const char*set_list[32]; int nset=0;
    double fps=0,from_fps=0,to_fps=0,maxd=0,mind=0;
    int keep_tags=0,strip_sdh=0,no_san=0,rebase=1,corrected=0,testing=0,check=0,do_login=0,check_only=0;
    const char*srt=NULL,*translator=NULL,*comment=NULL;
    char*files[64]; int nfiles=0; double offs[32]; int noff=0; char*ats[64]; int nat=0; char*anch[64]; int nanch=0;
    /* parsowanie niezależne od pozycji (flagi globalne mogą być przed poleceniem) */
    for(int i=1;i<argc;i++){ const char*a=argv[i];
        if(!strcmp(a,"--version")){ printf("aqnapi %s\n",VERSION); return 0; }
        else if(!strcmp(a,"--help")||!strcmp(a,"-h")){ usage(); return 0; }
        else if(!strcmp(a,"-o")||!strcmp(a,"--output")){ if(++i<argc) out=argv[i]; }
        else if(!strcmp(a,"--movie")){ if(++i<argc) movie=argv[i]; }
        else if(!strcmp(a,"-l")||!strcmp(a,"--lang")){ if(++i<argc) lang=argv[i]; }
        else if(!strcmp(a,"--format")){ if(++i<argc) fmt=argv[i]; }
        else if(!strcmp(a,"--config")){ if(++i<argc) cfgpath=argv[i]; }
        else if(!strcmp(a,"--title")){ if(++i<argc) title=argv[i]; }
        else if(!strcmp(a,"--imdb")){ if(++i<argc) imdb=argv[i]; }
        else if(!strcmp(a,"--query")){ if(++i<argc) query=argv[i]; }
        else if(!strcmp(a,"--season")){ if(++i<argc) season=argv[i]; }
        else if(!strcmp(a,"--episode")){ if(++i<argc) episode=argv[i]; }
        else if(!strcmp(a,"--release")){ if(++i<argc) release=argv[i]; }
        else if(!strcmp(a,"--resolution")){ if(++i<argc) resolution=argv[i]; }
        else if(!strcmp(a,"--duration")){ if(++i<argc) duration=argv[i]; }
        else if(!strcmp(a,"--size")){ if(++i<argc) a_size=argv[i]; }
        else if(!strcmp(a,"--year")){ if(++i<argc) year=argv[i]; }
        else if(!strcmp(a,"--title-pl")){ if(++i<argc) title_pl=argv[i]; }
        else if(!strcmp(a,"--episode-title")){ if(++i<argc) episode_title=argv[i]; }
        else if(!strcmp(a,"--sync")){ if(++i<argc) a_sync=argv[i]; }
        else if(!strcmp(a,"--proof")){ if(++i<argc) proof=argv[i]; }
        else if(!strcmp(a,"--reason")){ if(++i<argc) reason=argv[i]; }
        else if(!strcmp(a,"--srt")){ if(++i<argc) srt=argv[i]; }
        else if(!strcmp(a,"--translator")){ if(++i<argc) translator=argv[i]; }
        else if(!strcmp(a,"--service")){ if(++i<argc) service=argv[i]; }
        else if(!strcmp(a,"--id")){ if(++i<argc) rec_id=argv[i]; }
        else if(!strcmp(a,"--sms-code")){ if(++i<argc) sms_code=argv[i]; }
        else if(!strcmp(a,"--info")){ if(++i<argc) info=argv[i]; }
        else if(!strcmp(a,"--progress")){ if(++i<argc) progress=argv[i]; }
        else if(!strcmp(a,"--set")){ if(++i<argc){ set_val=argv[i]; if(nset<32) set_list[nset++]=argv[i]; } }
        else if(!strcmp(a,"--off")) notify_off=1;
        else if(!strcmp(a,"--show")) show_fields=1;
        else if(!strcmp(a,"--kind")){ if(++i<argc) rep_kind=atoi(argv[i]); }
        else if(!strcmp(a,"--list")) rep_list=1;
        else if(!strcmp(a,"--comment")){ if(++i<argc) comment=argv[i]; }
        else if(!strcmp(a,"--corrected")) corrected=1;
        else if(!strcmp(a,"--test")||!strcmp(a,"--dry-run")) testing=1;
        else if(!strcmp(a,"--check")) check=1;
        else if(!strcmp(a,"--check-only")) check_only=1;
        else if(!strcmp(a,"--login")) do_login=1;
        else if(!strcmp(a,"--fps")){ if(++i<argc) fps=atof(argv[i]); }
        else if(!strcmp(a,"--from")){ if(++i<argc) from_fps=atof(argv[i]); }
        else if(!strcmp(a,"--to")){ if(++i<argc) to_fps=atof(argv[i]); }
        else if(!strcmp(a,"--offset")){ if(++i<argc && noff<32) offs[noff++]=atof(argv[i]); }
        else if(!strcmp(a,"--at")){ if(++i<argc && nat<64) ats[nat++]=argv[i]; }
        else if(!strcmp(a,"--anchor")){ if(++i<argc && nanch<64) anch[nanch++]=argv[i]; }
        else if(!strcmp(a,"--max-display")){ if(++i<argc) maxd=atof(argv[i]); }
        else if(!strcmp(a,"--min-display")){ if(++i<argc) mind=atof(argv[i]); }
        else if(!strcmp(a,"--keep-tags")) keep_tags=1;
        else if(!strcmp(a,"--strip-sdh")) strip_sdh=1;
        else if(!strcmp(a,"--no-sanitize")) no_san=1;
        else if(!strcmp(a,"--no-rebase")) rebase=0;
        else if(a[0]!='-'){ if(!cmd) cmd=a; else if(nfiles<64) files[nfiles++]=argv[i]; }
    }
    if(!cmd){ usage(); return 2; }
    const char*pos = nfiles>0? files[0] : NULL;
    SanOpts opt=SAN_DEFAULT; opt.enabled=!no_san; opt.keep_tags=keep_tags; opt.strip_sdh=strip_sdh;
    opt.max_display_ms=(long)((maxd>0?maxd:10.0)*1000); opt.min_display_ms=(long)((mind>0?mind:0)*1000);

    if(!strcmp(cmd,"hash")){ if(!pos){usage();return 2;} return cmd_hash(pos); }
    if(!strcmp(cmd,"fps")){ if(!pos){usage();return 2;} return cmd_fps(pos); }
    if(!strcmp(cmd,"convert")){ if(!pos){usage();return 2;} return cmd_convert(pos,out,movie,fps,fmt,opt); }
    if(!strcmp(cmd,"download")){ if(!pos){usage();return 2;} return cmd_download(pos,lang,out,fps,opt); }
    if(!strcmp(cmd,"fpsconv")){ if(!pos){usage();return 2;} return cmd_fpsconv(pos,out,from_fps,to_fps,movie,fmt); }
    if(!strcmp(cmd,"merge")){ return cmd_merge(files,nfiles,out,fps,fmt,offs,noff); }
    if(!strcmp(cmd,"split")){ if(!pos){usage();return 2;} return cmd_split(pos,out,ats,nat,rebase,fps,fmt); }
    if(!strcmp(cmd,"config")){ return cmd_config(pos?pos:"show",cfgpath); }
    if(!strcmp(cmd,"sync")){ return cmd_sync(nfiles>0?files[0]:NULL, nfiles>1?files[1]:NULL, out, noff>0?offs[0]:0, noff>0, anch, nanch, fps); }
    if(!strcmp(cmd,"update")){ return cmd_update(check); }
    if(!strcmp(cmd,"get")){ if(!pos){usage();return 2;} return cmd_get(pos,lang,out,fps,opt); }
    if(!strcmp(cmd,"search")){ return cmd_search(imdb,title,query,lang); }
    if(!strcmp(cmd,"upload")){ char fpsb[16]=""; if(fps>0) snprintf(fpsb,sizeof fpsb,"%g",fps);
        return cmd_agg_upload(cfgpath,service,srt,movie,lang,translator,corrected,comment,testing,do_login,
            imdb,title,title_pl,year,release,a_sync,proof,resolution,duration,a_size,fpsb[0]?fpsb:NULL,season,episode,episode_title); }
    if(!strcmp(cmd,"napiprojekt")||!strcmp(cmd,"np")){ const char*sub=nfiles>0?files[0]:NULL,*a1=nfiles>1?files[1]:NULL; if(!sub){usage();return 2;}
        if(!strcmp(sub,"download")){ if(!a1){usage();return 2;} return cmd_download(a1,lang,out,fps,opt); }
        if(!strcmp(sub,"fileinfo")){ if(!a1){usage();return 2;} return cmd_np_fileinfo(a1); }
        if(!strcmp(sub,"search")){ const char*t=a1?a1:(title?title:query); if(!t){usage();return 2;} return np_search(t); }
        if(!strcmp(sub,"upload")){ return cmd_np_upload(cfgpath,movie,srt,lang,translator,corrected,comment,testing,do_login); }
        if(!strcmp(sub,"account")){ return cmd_np_account(cfgpath); }
        if(!strcmp(sub,"associate")){ const char*mv=a1?a1:movie; return cmd_np_associate(cfgpath,mv,nfiles>2?files[2]:NULL); }
        if(!strcmp(sub,"cover")){ const char*mv=a1?a1:movie; return cmd_np_cover(mv,out); }
        if(!strcmp(sub,"version")){ return cmd_np_version(); }
        if(!strcmp(sub,"report")){ const char*mv=a1?a1:movie; return cmd_np_report(cfgpath,mv,rep_kind,comment,lang,rep_list); }
        fprintf(stderr,"napiprojekt: '%s' nieobsługiwane w wersji C (użyj aqnapi.py)\n",sub); return 2; }
    if(!strcmp(cmd,"napisy24")||!strcmp(cmd,"n24")){ const char*sub=nfiles>0?files[0]:NULL,*a1=nfiles>1?files[1]:NULL; if(!sub){usage();return 2;}
        if(!strcmp(sub,"hash")){ if(!a1){usage();return 2;} return cmd_hash(a1); }
        if(!strcmp(sub,"login")){ return cmd_n24_login(cfgpath); }
        if(!strcmp(sub,"imdb")){ const char*id=a1?a1:imdb; if(!id){usage();return 2;} return cmd_n24_imdb(id); }
        if(!strcmp(sub,"weblogin")){
#ifdef AQNAPI_TLS
            return cmd_n24_weblogin(cfgpath);
#else
            fprintf(stderr,"weblogin wymaga wariantu TLS (aqnapi-c-tls.com)\n"); return 2;
#endif
        }
        if(!strcmp(sub,"getid")){ if(!a1){usage();return 2;} return cmd_n24_getid(a1,out,movie,fps,opt); }
        if(!strcmp(sub,"download")){ if(!a1){usage();return 2;} return cmd_n24_download(a1,lang,out,fps,opt); }
        if(!strcmp(sub,"search")){ return n24_search(imdb,title); }
        if(!strcmp(sub,"attach")){ char fpsb[16]=""; if(fps>0) snprintf(fpsb,sizeof fpsb,"%g",fps);
            return cmd_n24_attach(cfgpath,movie,srt,imdb,duration,resolution,fpsb[0]?fpsb:NULL,check_only); }
        if(!strcmp(sub,"upload")){
#ifdef AQNAPI_TLS
            /* fps + czas z kontenera filmu; release wg zasad N24; rozmiar z filmu */
            double mfps=0,mdur=0; if(movie) media_from_file(movie,&mfps,&mdur);
            char fpsb[16]=""; if(fps>0) snprintf(fpsb,sizeof fpsb,"%g",fps); else if(mfps>0) snprintf(fpsb,sizeof fpsb,"%g",mfps);
            char durb[16]=""; if(duration&&duration[0]) snprintf(durb,sizeof durb,"%s",duration); else hhmmss(mdur,durb,sizeof durb);
            char relb[256]; { char nb[512]; if(movie) input_basename(movie,nb,sizeof nb); else nb[0]=0;
                n24_release((release&&release[0])?release:nb,relb,sizeof relb); }
            char szb[32]=""; if(a_size&&a_size[0]) snprintf(szb,sizeof szb,"%s",a_size); else if(movie) snprintf(szb,sizeof szb,"%ld",input_size(movie));
            N24Meta m={imdb,title,title_pl,year,relb,translator,a_sync,proof,resolution,durb[0]?durb:NULL,szb[0]?szb:NULL,fpsb[0]?fpsb:NULL,season,episode,episode_title};
            return cmd_n24_upload(cfgpath,srt,&m,testing);
#else
            fprintf(stderr,"napisy24 upload wymaga wariantu TLS (aqnapi-c-tls.com)\n"); return 2;
#endif
        }
        if(!strcmp(sub,"delete")||!strcmp(sub,"rm")){
#ifdef AQNAPI_TLS
            return cmd_n24_delete(cfgpath,a1,reason);
#else
            fprintf(stderr,"napisy24 delete wymaga wariantu TLS (aqnapi-c-tls.com)\n"); return 2;
#endif
        }
        if(!strcmp(sub,"mediainfo")||!strcmp(sub,"changedata")){ char fpsb[16]=""; if(fps>0) snprintf(fpsb,sizeof fpsb,"%g",fps);
            return cmd_n24_mediainfo(cfgpath,movie,rec_id,duration,resolution,fpsb[0]?fpsb:NULL,lang?lang:"pl"); }
        if(!strcmp(sub,"notify")){ return cmd_n24_notify(cfgpath,movie,notify_off,sms_code,lang?lang:"pl"); }
        if(!strcmp(sub,"trans")){ return cmd_n24_trans(cfgpath,a1,set_val,info,progress); }
        if(!strcmp(sub,"premieres")||!strcmp(sub,"getimdb")){ return cmd_n24_premieres(); }
        if(!strcmp(sub,"edit")){
#ifdef AQNAPI_TLS
            return cmd_n24_edit(cfgpath,a1,show_fields,set_list,nset,srt);
#else
            fprintf(stderr,"napisy24 edit wymaga wariantu TLS (aqnapi-c-tls.com)\n"); return 2;
#endif
        }
        fprintf(stderr,"napisy24: '%s' nieobsługiwane w wersji C (użyj aqnapi.py)\n",sub); return 2; }
    if(!strcmp(cmd,"opensubtitles")||!strcmp(cmd,"os")){
#ifdef AQNAPI_TLS
        const char*sub=nfiles>0?files[0]:NULL,*a1=nfiles>1?files[1]:NULL; if(!sub){usage();return 2;}
        if(!strcmp(sub,"login")) return cmd_os_login(cfgpath);
        if(!strcmp(sub,"logout")) return cmd_os_logout(cfgpath);
        if(!strcmp(sub,"search")) return cmd_os_search(cfgpath,imdb,title,query,lang,season,episode);
        if(!strcmp(sub,"download")){ if(!a1){usage();return 2;} return cmd_os_download(cfgpath,a1,out,movie,fps,opt); }
        if(!strcmp(sub,"formats")) return cmd_os_formats(cfgpath);
        if(!strcmp(sub,"languages")) return cmd_os_languages(cfgpath);
        if(!strcmp(sub,"guessit")){ const char*fn=a1?a1:query; if(!fn){usage();return 2;} return cmd_os_guessit(cfgpath,fn); }
        fprintf(stderr,"opensubtitles: '%s' nieobsługiwane w wersji C\n",sub); return 2;
#else
        fprintf(stderr,"opensubtitles: wymaga wariantu TLS (aqnapi-c-tls.com) lub aqnapi.py\n"); return 2;
#endif
    }
    if(!strcmp(cmd,"_selftest")){
        unsigned char key[32],pt[16],ct[16]; for(int i=0;i<32;i++)key[i]=i; for(int i=0;i<16;i++)pt[i]=(i<<4)|i;
        AES x; aes_init(&x,key); memcpy(ct,pt,16); aes_encrypt_block(&x,ct); char h[33]; hexlower(ct,16,h);
        printf("AES-256 FIPS: %s %s\n",h,!strcmp(h,"8ea2b7ca516745bfeafc49904b496089")?"OK":"FAIL");
        SHA256 s; sha256_init(&s); sha256_update(&s,(unsigned char*)"abc",3); unsigned char d[32]; sha256_final(&s,d); char h2[65]; hexlower(d,32,h2);
        printf("SHA-256(abc): %.16s… %s\n",h2,!strcmp(h2,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")?"OK":"FAIL");
        if(pos){ const char*msg="Zawartość testowa 7z-AES: ąćęłńóśźż\n"; size_t al; unsigned char*a=write_7z_aes("test.txt",(unsigned char*)msg,strlen(msg),&al);
            write_file(pos,(char*)a,al); free(a); printf("Archiwum 7z zapisane: %s (%zu B)\n",pos,al); }
        return 0; }
    fprintf(stderr,"Nieznane polecenie: %s\n",cmd); usage(); return 2;
}
