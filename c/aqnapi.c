/* aqnapi (wersja C, POC) — niezależna, maksymalnie zgodna reimplementacja
 * podzbioru aqnapi.py, kompilowana przez cosmocc do uniwersalnej binarki APE.
 *
 * Zakres POC (bajtowo zgodny z aqnapi.py):
 *   hash FILE                 — OSH + MD5(10MiB)
 *   fps  FILE                 — FPS z MKV / AVI / MP4-MOV
 *   convert IN [-o OUT] [--fps F]   — SRT/MicroDVD/VTT -> SRT (UTF-8+BOM, LF)
 *                                     z tą samą sanityzacją co Python
 *   download FILE [-l PL] [-o OUT] [--fps F]
 *                             — pobranie z napiprojekt (mode=1, HTTP) -> SRT
 *
 * Poza zakresem POC (pozostaje w wersji Python): OpenSubtitles (TLS),
 * napisy24 (ZIP/formularz WWW), 7z-AES upload, sync (curses), ASS/MPL2/TMPlayer,
 * transkodowanie cp1250/iso-8859-2 (POC zakłada wejście UTF-8).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#define VERSION "1.0.0"
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

static int oshash(const char*path,char out[17]){
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
    FILE*f=fopen(path,"rb"); if(!f) return -1;
    MD5 m; md5_init(&m);
    unsigned char*buf=xmalloc(1<<20); size_t total=0,r;
    while(total<CHUNK_10MB && (r=fread(buf,1,(size_t)(CHUNK_10MB-total<(1<<20)?CHUNK_10MB-total:(1<<20)),f))>0){ md5_update(&m,buf,r); total+=r; }
    free(buf); fclose(f);
    unsigned char d[16]; md5_final(&m,d); hexlower(d,16,out); return 0;
}
static void md5_bytes(const unsigned char*p,size_t n,char out[33]){ MD5 m; md5_init(&m); md5_update(&m,p,n); unsigned char d[16]; md5_final(&m,d); hexlower(d,16,out); }

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
static double fps_mkv(FILE*f){
    fseek(f,0,SEEK_SET);
    long track=0;
    for(long guard=0;guard<2000000;guard++){
        uint64_t id,len;
        if(ebml_read(f,&id,1)<0) return 0;
        if(ebml_read(f,&len,0)<0) return 0;
        if(id==0x83){ int b=fgetc(f); track=(b<0)?0:b; }
        else if(id==0x23E383 && track==1){
            unsigned char raw[4]; if(fread(raw,1,4,f)!=4) return 0;
            uint64_t ns=rd_be(raw,4); if(ns==0) return 0; return 1000000000.0/(double)ns;
        }
        else if(id!=0x18538067 && id!=0x1654AE6B && id!=0xAE && id!=0x83){
            fseek(f,(long)len,SEEK_CUR);
        }
    }
    return 0;
}
static double fps_avi(FILE*f){ unsigned char b[4]; fseek(f,32,SEEK_SET); if(fread(b,1,4,f)!=4) return 0;
    uint32_t us=(uint32_t)b[0]|(b[1]<<8)|(b[2]<<16)|((uint32_t)b[3]<<24); if(!us) return 0; return 1000000.0/(double)us; }

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
static double fps_mp4(FILE*f,long flen){
    long pos=0; char t[5]; long pl;
    while(pos>=0 && pos<flen){ long nx=box_next(f,pos,flen,t,&pl); if(nx<0) break;
        if(!strcmp(t,"moov")){
            long tp,te,mp,me,hp,he,dhp,dhe,sp,se; long tpos=pl;
            char tt[5]; long tpl;
            while(tpos>=0 && tpos<nx){ long tnx=box_next(f,tpos,nx,tt,&tpl); if(tnx<0) break;
                if(!strcmp(tt,"trak")){ tp=tpl; te=tnx;
                    if(bmff_find(f,tp,te,"mdia",&mp,&me)){
                        if(bmff_find(f,mp,me,"hdlr",&hp,&he)){
                            /* payload: version+flags(4) + pre_defined(4) + handler(4) */
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
                                        if(timescale && tot_d) return (double)tot_s*(double)timescale/(double)tot_d;
                                    }
                                }
                            }
                        }
                    }
                }
                tpos=tnx; }
        }
        pos=nx; }
    return 0;
}
static double fps_from_file(const char*path){
    FILE*f=fopen(path,"rb"); if(!f) return 0;
    unsigned char m[8]; size_t r=fread(m,1,8,f); double v=0;
    if(r>=4 && m[0]==0x1a&&m[1]==0x45&&m[2]==0xdf&&m[3]==0xa3) v=fps_mkv(f);
    else if(r>=4 && !memcmp(m,"RIFF",4)) v=fps_avi(f);
    else if(r>=8 && !memcmp(m+4,"ftyp",4)){ fseek(f,0,SEEK_END); long fl=ftell(f); v=fps_mp4(f,fl); }
    fclose(f); return v;
}
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
                char*seg=xmalloc(len+1); memcpy(seg,p,len); seg[len]=0; strip_format_tags(seg); cue_addline(q,seg,strlen(seg)); free(seg);
                if(!bar) break; p=bar+1; }
        }
        free(t);
    }
    lines_free(&L);
}

/* VTT: minimalne dekodowanie encji + usunięcie tagów <..> */
static void html_unescape(char*s){
    struct{const char*e;const char*r;} tab[]={{"&amp;","&"},{"&lt;","<"},{"&gt;",">"},{"&quot;","\""},{"&#39;","'"},{"&apos;","'"},{"&nbsp;"," "},{NULL,NULL}};
    char*o=s,*p=s; while(*p){ if(*p=='&'){ int done=0; for(int i=0;tab[i].e;i++){ size_t el=strlen(tab[i].e); if(!strncmp(p,tab[i].e,el)){ for(const char*r=tab[i].r;*r;) *o++=*r++; p+=el; done=1; break; } } if(done) continue; } *o++=*p++; } *o=0;
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

/* wykrycie formatu (podzbiór POC: srt/microdvd/vtt) */
static const char* detect_format(const char*text){
    const char*h=text; if((unsigned char)h[0]==0xef&&(unsigned char)h[1]==0xbb&&(unsigned char)h[2]==0xbf) h+=3;
    while(*h&&is_ascii_ws(*h)) h++;
    if(!strncasecmp(h,"WEBVTT",6)) return "vtt";
    if(strstr(text,"-->")) return "srt";
    /* pierwsza niepusta linia */
    const char*p=text; while(*p){ const char*nl=strchr(p,'\n'); size_t len=nl?(size_t)(nl-p):strlen(p);
        char buf[8]={0}; size_t k=0; for(size_t j=0;j<len&&k<7;j++){ if(!is_ascii_ws(p[j])) buf[k++]=p[j]; }
        if(k){ if(buf[0]=='{') return "microdvd"; break; }
        if(!nl) break; p=nl+1; }
    return "srt";
}

static void parse_any(const char*text,double fps,Cues*out){
    const char*fmt=detect_format(text);
    if(!strcmp(fmt,"microdvd")) parse_microdvd(text,fps,out);
    else if(!strcmp(fmt,"vtt")) parse_vtt(text,out);
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
/* sanityzacja — dokładnie jak sanitize_cues (domyślne opcje) */
static void sanitize(Cues*in,Cues*out,SanReport*rep){
    memset(rep,0,sizeof *rep); cues_init(out);
    for(int i=0;i<in->n;i++){ Cue*c=&in->a[i];
        Cue tmp; tmp.lines=NULL; tmp.nlines=0; int changed=0;
        for(int k=0;k<c->nlines;k++){ char*s=xmalloc(strlen(c->lines[k])+1); strcpy(s,c->lines[k]); strip_format_tags(s);
            if(strcmp(s,c->lines[k])!=0) changed=1;
            tmp.lines=xrealloc(tmp.lines,(tmp.nlines+1)*sizeof(char*)); tmp.lines[tmp.nlines++]=s; }
        if(changed) rep->tags++;
        /* trim + odrzuć puste */
        int keep=0; for(int k=0;k<tmp.nlines;k++){ strip_inplace(tmp.lines[k]); if(tmp.lines[k][0]!=0) tmp.lines[keep++]=tmp.lines[k]; else free(tmp.lines[k]); }
        tmp.nlines=keep;
        if(tmp.nlines==0){ free(tmp.lines); rep->empty++; continue; }
        long start=c->start,end=c->end; if(end<=start){ end=start+1000; rep->nonpos++; }
        Cue*q=cues_push(out); q->start=start; q->end=end; q->lines=tmp.lines; q->nlines=tmp.nlines;
    }
    for(int i=0;i<out->n;i++){ Cue*c=&out->a[i]; long nxt = (i+1<out->n)? out->a[i+1].start : -1;
        if(nxt>=0 && c->end>nxt){ long v=c->start+1; c->end=(nxt>v)?nxt:v; rep->overlaps++; }
        if(c->end-c->start>MAX_DISPLAY_MS){ c->end=c->start+MAX_DISPLAY_MS; rep->lng++; }
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

/* pełny pipeline: bajty -> SRT (rzuca błąd gdy 0 linii z niepustego wejścia) */
static void convert_to_srt(const char*data,double fps,SB*out,SanReport*rep){
    const char*p=data; if((unsigned char)p[0]==0xef&&(unsigned char)p[1]==0xbb&&(unsigned char)p[2]==0xbf) p+=3;
    Cues raw; cues_init(&raw); parse_any(p,fps,&raw);
    int nonws=0; for(const char*q=p;*q;q++) if(!is_ascii_ws(*q)){ nonws=1; break; }
    if(raw.n==0 && nonws) die("Napisy wyglądają na uszkodzone lub w nierozpoznanym formacie (0 rozpoznanych linii) — nie zapisuję.");
    Cues clean; sanitize(&raw,&clean,rep);
    emit_srt(&clean,out);
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
    const char*fields[][2]={{"client","aqnapi"},{"client_ver",VERSION},{"mode","1"},
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

/* ---------------------------------------------------------------- I/O plików */
static char* read_file(const char*path,size_t*len){ FILE*f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); char*b=xmalloc(n+1); size_t r=fread(b,1,n,f); b[r]=0; fclose(f); if(len)*len=r; return b; }
static void write_file(const char*path,const char*data,size_t len){ FILE*f=fopen(path,"wb"); if(!f) die("nie mogę zapisać pliku wyjściowego"); fwrite(data,1,len,f); fclose(f); }
static char* default_out(const char*movie,const char*explicit_out){ if(explicit_out){ char*d=xmalloc(strlen(explicit_out)+1); strcpy(d,explicit_out); return d; }
    const char*dot=strrchr(movie,'.'); size_t base= dot? (size_t)(dot-movie):strlen(movie); char*d=xmalloc(base+5); memcpy(d,movie,base); strcpy(d+base,".srt"); return d; }
static const char* basename_of(const char*p){ const char*s=strrchr(p,'/'); return s?s+1:p; }

/* ---------------------------------------------------------------- polecenia */
static int cmd_hash(const char*path){
    char osh[17]; int r=oshash(path,osh);
    if(r==-1){ fprintf(stderr,"Błąd: plik za mały na hash OSH (min. %d B): %s\n",2*OSH_CHUNK,path); return 1; }
    if(r==-2){ fprintf(stderr,"Brak pliku: %s\n",path); return 1; }
    char md[33]; if(md5_10mb(path,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",path); return 1; }
    printf("OSH (fh)        : %s\n",osh);
    printf("MD5-10MiB (md)  : %s\n",md);
    printf("rozmiar         : %ld B\n",file_size(path));
    printf("nazwa           : %s\n",basename_of(path));
    return 0;
}
static int cmd_fps(const char*path){ double f=fps_from_file(path);
    if(f==0){ printf("Nie udało się odczytać FPS z pliku (obsługa: MKV/AVI/MP4/MOV)\n"); return 1; }
    const char*note = trusted_fps(f)? "":"  (poza bramką 22<fps<32 — traktowane jako niepewne)";
    printf("FPS: %.3f%s\n",f,note); return 0;
}
static double resolve_fps(const char*movie,double server_fps,double flag_fps){
    if(movie){ double f=trusted_fps(fps_from_file(movie)); if(f) return f; }
    if(trusted_fps(server_fps)) return server_fps;
    if(flag_fps>0) return flag_fps;
    return DEFAULT_FPS;
}
static int cmd_convert(const char*in,const char*out,const char*movie,double flag_fps){
    size_t n; char*data=read_file(in,&n); if(!data){ fprintf(stderr,"Brak pliku: %s\n",in); return 1; }
    double fps=resolve_fps(movie,0,flag_fps);
    SB srt; sb_init(&srt); SanReport rep; convert_to_srt(data,fps,&srt,&rep); free(data);
    char*outp=default_out(in,out); write_file(outp,srt.b,srt.len);
    print_saved(outp,srt.len,&rep);
    free(outp); free(srt.b); return 0;
}
static int cmd_download(const char*movie,const char*lang,const char*out,double flag_fps){
    char md[33]; if(md5_10mb(movie,md)!=0){ fprintf(stderr,"Brak pliku: %s\n",movie); return 1; }
    char L[8]; snprintf(L,sizeof L,"%s",lang?lang:"PL"); for(char*p=L;*p;p++)*p=toupper((unsigned char)*p);
    size_t dl; unsigned char*sub=np_download(md,L,&dl);
    if(!sub){ printf("Brak napisów dla: %s\n",movie); return 1; }
    double sfps=np_file_info_fps(md); double fps=resolve_fps(movie,sfps,flag_fps);
    char*text=xmalloc(dl+1); memcpy(text,sub,dl); text[dl]=0; free(sub);
    SB srt; sb_init(&srt); SanReport rep; convert_to_srt(text,fps,&srt,&rep); free(text);
    char*outp=default_out(movie,out); write_file(outp,srt.b,srt.len);
    print_saved(outp,srt.len,&rep);
    free(outp); free(srt.b); return 0;
}

static void usage(void){
    printf("aqnapi %s (wersja C, POC)\n"
        "Użycie:\n"
        "  aqnapi hash PLIK\n"
        "  aqnapi fps PLIK\n"
        "  aqnapi convert WEJŚCIE [-o WYJŚCIE] [--movie FILM] [--fps F]\n"
        "  aqnapi download FILM [-l PL] [-o WYJŚCIE] [--fps F]\n"
        "  aqnapi --version | --help\n", VERSION);
}

int main(int argc,char**argv){
    if(argc<2){ usage(); return 2; }
    const char*cmd=argv[1];
    if(!strcmp(cmd,"--version")){ printf("aqnapi %s\n",VERSION); return 0; }
    if(!strcmp(cmd,"--help")||!strcmp(cmd,"-h")){ usage(); return 0; }

    /* proste parsowanie flag */
    const char*pos=NULL,*out=NULL,*movie=NULL,*lang=NULL; double fps=0;
    for(int i=2;i<argc;i++){
        if(!strcmp(argv[i],"-o")||!strcmp(argv[i],"--output")){ if(++i<argc) out=argv[i]; }
        else if(!strcmp(argv[i],"--movie")){ if(++i<argc) movie=argv[i]; }
        else if(!strcmp(argv[i],"-l")||!strcmp(argv[i],"--lang")){ if(++i<argc) lang=argv[i]; }
        else if(!strcmp(argv[i],"--fps")){ if(++i<argc) fps=atof(argv[i]); }
        else if(argv[i][0]!='-' && !pos){ pos=argv[i]; }
    }

    if(!strcmp(cmd,"hash")){ if(!pos){usage();return 2;} return cmd_hash(pos); }
    if(!strcmp(cmd,"fps")){ if(!pos){usage();return 2;} return cmd_fps(pos); }
    if(!strcmp(cmd,"convert")){ if(!pos){usage();return 2;} return cmd_convert(pos,out,movie,fps); }
    if(!strcmp(cmd,"download")){ if(!pos){usage();return 2;} return cmd_download(pos,lang,out,fps); }
    fprintf(stderr,"Nieznane polecenie: %s\n",cmd); usage(); return 2;
}
