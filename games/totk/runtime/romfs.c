/* Read-only game RomFS, backed by the player's fully IVFC-verified image, and the DLC (add-on content) RomFS images. */
#include "runtime.h"
#include "totk_ui_lang.h"
#include "romfs_config.h"
#include "game_version.h"
#include "fs_host.h"
#include "romfs_path.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#define NOT_FOUND 0x202u
#define UNSUPPORTED 0x6402u
#define INVALID 0xe402u
#define NH 65536
/* A mapped runtime-index.bin (scripts/prepare_romfs_backend.py): file rows sorted by path, then the names. */
typedef struct { const uint8_t *bytes; uint64_t count,strings; } View;
static View rom;
static int image_fd=-1;
/* Split image: romfs.bin.000, .001, ... of equal size (the last one shorter), read in place. The phone gets the 17 GB
   image this way: devicectl drops the connection on a single file of that size, and joining the parts on the device
   would need the space twice. image_fd is then the first part; src_pread routes image offsets to their part. */
#define MAX_CHUNKS 64
static int chunk_fds[MAX_CHUNKS];
static unsigned nchunks;
static uint64_t chunk_size;
static ssize_t src_pread(int fd,void *buf,size_t n,off_t off) {
 if(!nchunks||fd!=image_fd)return pread(fd,buf,n,off);
 uint64_t c=(uint64_t)off/chunk_size,o=(uint64_t)off%chunk_size;if(c>=nchunks)return 0;
 if(n>chunk_size-o)n=(size_t)(chunk_size-o);
 return pread(chunk_fds[c],buf,n,(off_t)o);
}
static const uint8_t *image_map;
static char mount_name[32];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t read_calls,read_bytes,read_ns,read_prepare_ns,read_report_ns;
static uint64_t handles[NH]; /* file index + 1; 0 is free */
/* Overlay (scripts/make_romfs_patches.py): files listed in <TOTK_ROMFS_PATCHES, default extracted/v0/romfs-patches>/manifest.tsv
   are served in place of the originals. Each line names a RomFS path, the overlay's size and SHA-256 and the original's
   SHA-256; a missing path, a size or hash mismatch or a changed original stops the start (regenerate the overlay).
   TOTK_ROMFS_PATCHES=0 serves the originals. */
#define NOVL 16
static struct { uint64_t index,size; int fd; } overlays[NOVL];
static unsigned noverlay;
static uint8_t handle_ovl[NH]; /* overlay slot + 1 of an open handle, 0 = original */
static uint8_t handle_vol[NH]; /* add-on content slot + 1 of an open handle, 0 = the game's RomFS */
typedef struct { uint64_t offset,size;uint32_t name,len; } Entry;
_Static_assert(sizeof(Entry)==24,"index entry layout");
static Entry ventry(const View *v,uint64_t i) {Entry e;memcpy(&e,v->bytes+24+i*24,24);return e;}
static const char *vname(const View *v,Entry e) {return (const char *)v->bytes+v->strings+e.name;}
static uint64_t vlower(const View *v,const char *name) {uint64_t lo=0,hi=v->count;while(lo<hi){uint64_t m=lo+(hi-lo)/2;if(strcmp(vname(v,ventry(v,m)),name)<0)lo=m+1;else hi=m;}return lo;}
static Entry entry(uint64_t i) {return ventry(&rom,i);}
static const char *entry_name(Entry e) {return vname(&rom,e);}
static void need(int c,const char *m) {if(!c)runtime_fail(m);}
static void sha_fd(int fd,const char *expected,uint64_t total,const char *stage) {
 CC_SHA256_CTX c;CC_SHA256_Init(&c);uint8_t *buffer=malloc(8<<20);need(buffer!=NULL,"hash buffer");
 for(off_t off=0;;){ssize_t n=src_pread(fd,buffer,8<<20,off);need(n>=0,"read hash input");if(!n)break;CC_SHA256_Update(&c,buffer,(CC_LONG)n);off+=n;
  if(stage&&((uint64_t)off%(256u<<20)<(8u<<20)||!n))runtime_boot_progress(stage,(uint64_t)off,total);}
 uint8_t d[32];char hex[65];CC_SHA256_Final(d,&c);free(buffer);for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);need(!strcmp(hex,expected),"RomFS integrity mismatch");
}
/* A successful full hash may be reused only while the same regular file has
   unchanged filesystem identity and modification metadata. This is a startup
   shortcut, not a fresh integrity proof; TOTK_ROMFS_VERIFY=full forces one. */
typedef struct {
 char magic[16], sha256[65];
 uint64_t dev,ino,size;
 int64_t mtime_sec,mtime_nsec,ctime_sec,ctime_nsec,birth_sec,birth_nsec;
} RomfsProof;
static RomfsProof proof_for(const struct stat *s,const char *sha256) {
 RomfsProof p={0};memcpy(p.magic,"TOTKRFS-PROOF-1",16);snprintf(p.sha256,sizeof p.sha256,"%s",sha256);
 p.dev=(uint64_t)s->st_dev;p.ino=(uint64_t)s->st_ino;p.size=(uint64_t)s->st_size;
 p.mtime_sec=s->st_mtimespec.tv_sec;p.mtime_nsec=s->st_mtimespec.tv_nsec;
 p.ctime_sec=s->st_ctimespec.tv_sec;p.ctime_nsec=s->st_ctimespec.tv_nsec;
 p.birth_sec=s->st_birthtimespec.tv_sec;p.birth_nsec=s->st_birthtimespec.tv_nsec;
 return p;
}
static int proof_equal(const RomfsProof *a,const RomfsProof *b) {
 return !memcmp(a->magic,b->magic,sizeof a->magic)&&!memcmp(a->sha256,b->sha256,sizeof a->sha256)
  &&a->dev==b->dev&&a->ino==b->ino&&a->size==b->size
  &&a->mtime_sec==b->mtime_sec&&a->mtime_nsec==b->mtime_nsec
  &&a->ctime_sec==b->ctime_sec&&a->ctime_nsec==b->ctime_nsec
  &&a->birth_sec==b->birth_sec&&a->birth_nsec==b->birth_nsec;
}
/* Split image: one record per part, all of which must be unchanged for the earlier full hash to be reused. */
static void chunk_proofs(RomfsProof *out) {
 /* ctime is left out: reinstalling the iOS app touches the data container's file metadata (ctime) without changing a
    byte, which made every launch hash 17 GB again. A content change still moves mtime (and size). */
 for(unsigned c=0;c<nchunks;c++){struct stat st;need(fstat(chunk_fds[c],&st)==0&&S_ISREG(st.st_mode),"RomFS part stat");out[c]=proof_for(&st,ROMFS_IMAGE_SHA256);out[c].ctime_sec=out[c].ctime_nsec=0;}
}
static void verify_chunks(const char *dir) {
 RomfsProof want[MAX_CHUNKS],saved[MAX_CHUNKS],after[MAX_CHUNKS];size_t bytes=nchunks*sizeof(RomfsProof);chunk_proofs(want);
 const char *pdir=getenv("TOTK_ROMFS_PROOF_DIR");if(pdir&&*pdir)dir=pdir;   /* iOS: directories devicectl creates belong to root */
 char path[4096],tmp[4096];need(snprintf(path,sizeof(path),"%s/.romfs-verified-parts-v1",dir)<(int)sizeof(path),"RomFS proof path");
 int force=getenv("TOTK_ROMFS_VERIFY")&&!strcmp(getenv("TOTK_ROMFS_VERIFY"),"full");
 int proof_fd=open(path,O_RDONLY|O_NOFOLLOW);
 if(proof_fd>=0){struct stat ps;int ok=fstat(proof_fd,&ps)==0&&S_ISREG(ps.st_mode)&&(size_t)ps.st_size==bytes&&read(proof_fd,saved,bytes)==(ssize_t)bytes;close(proof_fd);
  if(ok&&!force&&!memcmp(saved,want,bytes)){fprintf(stderr,"[totk] RomFS parts unchanged; reused full SHA256 verification record (%u parts)\n",nchunks);runtime_boot_progress(TOTK_UI("游戏资源校验记录有效","Game data verification record is valid"),ROMFS_IMAGE_SIZE,ROMFS_IMAGE_SIZE);return;}}
 fprintf(stderr,"[totk] verifying local RomFS SHA256 over %u parts (%llu bytes)%s\n",nchunks,(unsigned long long)ROMFS_IMAGE_SIZE,force?" [forced]":"");
 runtime_boot_progress(TOTK_UI("校验游戏资源","Verifying game data"),0,ROMFS_IMAGE_SIZE);sha_fd(image_fd,ROMFS_IMAGE_SHA256,ROMFS_IMAGE_SIZE,TOTK_UI("校验游戏资源","Verifying game data"));
 chunk_proofs(after);need(!memcmp(after,want,bytes),"RomFS changed during verification");
 if(snprintf(tmp,sizeof(tmp),"%s/.romfs-verified-parts-v1.%ld.tmp",dir,(long)getpid())>=(int)sizeof(tmp))return;
 int out=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
 if(out>=0){ssize_t n=write(out,want,bytes);int ok=n==(ssize_t)bytes&&fsync(out)==0;close(out);if(ok){if(rename(tmp,path)!=0)unlink(tmp);}else unlink(tmp);}
}
static void open_chunks(const char *dir) {
 uint64_t total=0;
 for(;nchunks<MAX_CHUNKS;nchunks++){char path[4096];need(snprintf(path,sizeof(path),"%s/romfs.bin.%03u",dir,nchunks)<(int)sizeof(path),"RomFS part path");
  int fd=open(path,O_RDONLY);if(fd<0)break;struct stat st;need(fstat(fd,&st)==0&&st.st_size>0,"RomFS part stat");
  if(!nchunks)chunk_size=(uint64_t)st.st_size;else need(chunk_fds[nchunks-1]>=0&&total==(uint64_t)nchunks*chunk_size,"RomFS parts must have equal sizes except the last");
  chunk_fds[nchunks]=fd;total+=(uint64_t)st.st_size;}
 need(nchunks>0,"RomFS image missing (romfs.bin or romfs.bin.000...)");
 need(total==ROMFS_IMAGE_SIZE,"RomFS parts do not add up to the image size");
 image_fd=chunk_fds[0];
 fprintf(stderr,"[totk] RomFS image in %u parts of %llu bytes\n",nchunks,(unsigned long long)chunk_size);
}
/* The full SHA-256 of a single-file image, skipped while the record <proof dir>/<proof> from an earlier full hash still
   matches the file (proof_for). */
static void verify_file(int fd,const char *dir,const char *sha256,const char *proof,const char *what) {
 const char *pdir=getenv("TOTK_ROMFS_PROOF_DIR");if(pdir&&*pdir)dir=pdir;
 struct stat before,after;need(fstat(fd,&before)==0&&S_ISREG(before.st_mode),"RomFS image stat");
 RomfsProof want=proof_for(&before,sha256),saved;
 char path[4096],tmp[4096];need(snprintf(path,sizeof(path),"%s/%s",dir,proof)<(int)sizeof(path),"RomFS proof path");
 int force=getenv("TOTK_ROMFS_VERIFY")&&!strcmp(getenv("TOTK_ROMFS_VERIFY"),"full");
 int proof_fd=open(path,O_RDONLY|O_NOFOLLOW);
 if(proof_fd>=0){struct stat ps;int ok=fstat(proof_fd,&ps)==0&&S_ISREG(ps.st_mode)&&ps.st_size==sizeof(saved)&&read(proof_fd,&saved,sizeof(saved))==sizeof(saved);close(proof_fd);
  if(ok&&!force&&proof_equal(&saved,&want)){fprintf(stderr,"[totk] %s unchanged; reused full SHA256 verification record (%llu bytes)\n",what,(unsigned long long)want.size);runtime_boot_progress(TOTK_UI("游戏资源校验记录有效","Game data verification record is valid"),want.size,want.size);return;}}
 fprintf(stderr,"[totk] verifying local %s SHA256 (%llu bytes)%s\n",what,(unsigned long long)want.size,force?" [forced]":"");
 runtime_boot_progress(TOTK_UI("校验游戏资源","Verifying game data"),0,want.size);sha_fd(fd,sha256,want.size,TOTK_UI("校验游戏资源","Verifying game data"));
 need(fstat(fd,&after)==0,"RomFS image stat after hash");
 RomfsProof finished=proof_for(&after,sha256);need(proof_equal(&want,&finished),"RomFS changed during verification");
 if(snprintf(tmp,sizeof(tmp),"%s/%s.%ld.tmp",dir,proof,(long)getpid())>=(int)sizeof(tmp))return;
 int out=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
 if(out>=0){ssize_t n=write(out,&want,sizeof(want));int ok=n==sizeof(want)&&fsync(out)==0;close(out);
  if(ok){if(rename(tmp,path)!=0)unlink(tmp);}else unlink(tmp);}
}
static void verify_image(int fd,const char *dir) {
 if(nchunks){verify_chunks(dir);return;}
 verify_file(fd,dir,ROMFS_IMAGE_SHA256,".romfs-verified-v1","RomFS");
}
static void sha_range(int fd,uint64_t offset,uint64_t size,const char *expected,const char *what) {
 CC_SHA256_CTX c;CC_SHA256_Init(&c);uint8_t *buffer=malloc(8<<20);need(buffer!=NULL,"hash buffer");
 for(uint64_t done=0;done<size;){size_t chunk=size-done>(8u<<20)?(8u<<20):(size_t)(size-done);ssize_t n=src_pread(fd,buffer,chunk,(off_t)(offset+done));need(n>0,"read hash input");CC_SHA256_Update(&c,buffer,(CC_LONG)n);done+=(uint64_t)n;}
 uint8_t d[32];char hex[65];CC_SHA256_Final(d,&c);free(buffer);for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);
 if(strcmp(hex,expected)){fprintf(stderr,"[totk] RomFS overlay: %s SHA-256 %s, manifest says %s\n",what,hex,expected);runtime_fail("RomFS overlay integrity mismatch");}
}
static uint64_t lower(const char *name);
static int path_ok(const char *p);
static void overlay_load(const char *dir,int required) {
 char path[4096];need(snprintf(path,sizeof(path),"%s/manifest.tsv",dir)<(int)sizeof(path),"RomFS overlay path");FILE *m=fopen(path,"r");
 if(!m&&required){fprintf(stderr,"[totk] RomFS overlay: %s missing; build it with scripts/make_output_res_overlay.py <w>x<h>\n",path);runtime_fail("true-2K HeapDef overlay missing (make romfs-patches-out OUTPUT_RES=<w>x<h>)");}
 if(!m){fprintf(stderr,"[totk] RomFS overlay: none (%s)\n",path);return;}
 char line[8192];
 while(fgets(line,sizeof(line),m)){
  if(line[0]=='#'||line[0]=='\n')continue;char name[4096],sha[65],orig[65];unsigned long long size=0;
  need(sscanf(line,"%4095[^\t]\t%llu\t%64[0-9a-f]\t%64[0-9a-f]",name,&size,sha,orig)==4&&strlen(sha)==64&&strlen(orig)==64,"RomFS overlay manifest line");
  need(path_ok(name)&&noverlay<NOVL,"RomFS overlay path or count");uint64_t i=lower(name);need(i<rom.count&&!strcmp(entry_name(entry(i)),name),"RomFS overlay names a file the RomFS lacks");
  for(unsigned k=0;k<noverlay;k++)need(overlays[k].index!=i,"RomFS overlay: two overlays replace the same file");
  Entry e=entry(i);sha_range(image_fd,e.offset,e.size,orig,"original");
  char file[8200];need(snprintf(file,sizeof(file),"%s/%s",dir,name)<(int)sizeof(file),"RomFS overlay file path");int fd=open(file,O_RDONLY);struct stat st;
  need(fd>=0&&fstat(fd,&st)==0&&(uint64_t)st.st_size==size,"RomFS overlay file missing or wrong size");sha_range(fd,0,size,sha,name);
  overlays[noverlay].index=i;overlays[noverlay].size=size;overlays[noverlay].fd=fd;noverlay++;
  fprintf(stderr,"[totk] RomFS overlay: %s (%llu bytes) replaces the original\n",name,size);
 }
 fclose(m);
}
/* TOTK_ROMFS_PATCHES (default extracted/v0/romfs-patches; 0 = none), and with a true-2K output bigger than 1920x1080 also
   <TOTK_ROMFS_PATCHES_OUT, default extracted/v0/romfs-patches-out>/<w>x<h> (the HeapDef sizes, scripts/make_output_res_overlay.py). */
static void overlay_init(void) {
 const char *dir=getenv("TOTK_ROMFS_PATCHES");if(!dir||strcmp(dir,"0"))overlay_load(dir?dir:"extracted/" TOTK_GAME_DIR "/romfs-patches",0);
 unsigned w,h;if(runtime_output_res(&w,&h)&&runtime_output_res_extra_bytes()){const char *out=getenv("TOTK_ROMFS_PATCHES_OUT");char d[4096];
  need(snprintf(d,sizeof(d),"%s/%ux%u",out?out:"extracted/" TOTK_GAME_DIR "/romfs-patches-out",w,h)<(int)sizeof(d),"RomFS output overlay path");overlay_load(d,1);}
}
/* <dir>/runtime-index.bin, checked against its SHA-256 and against an image of image_size bytes. */
static void map_index(const char *dir,const char *sha256,uint64_t image_size,View *v) {
 char path[4096];need(snprintf(path,sizeof(path),"%s/runtime-index.bin",dir)<(int)sizeof(path),"RomFS path");int fd=open(path,O_RDONLY);need(fd>=0,"RomFS index missing");
 struct stat st;need(fstat(fd,&st)==0&&st.st_size>=24&&st.st_size<128*1024*1024,"RomFS index size");sha_fd(fd,sha256,0,NULL);
 size_t size=(size_t)st.st_size;uint8_t *bytes=mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);need(bytes!=MAP_FAILED,"map RomFS index");
 need(!memcmp(bytes,"TOTKRFS1",8),"RomFS index format");uint64_t count,strings;memcpy(&count,bytes+8,8);memcpy(&strings,bytes+16,8);
 need(count<1000000&&strings==24+count*24&&strings<size,"RomFS index layout");
 v->bytes=bytes;v->count=count;v->strings=strings;
 const char *previous=NULL;
 for(uint64_t i=0;i<count;i++){Entry e=ventry(v,i);need(e.offset<=image_size&&e.size<=image_size-e.offset,"RomFS file bounds");need(e.name<size-strings&&e.len>1&&e.len<=size-strings-e.name,"RomFS name bounds");const char *name=vname(v,e);need(name[e.len-1]==0&&strlen(name)==e.len-1&&(!previous||strcmp(previous,name)<0),"RomFS sorted paths");previous=name;}
}
void runtime_fs_init(void) {
 if(image_fd>=0)return;
 const char *dir=getenv("TOTK_ROMFS");if(!dir)dir="extracted/" TOTK_GAME_DIR "/romfs";char path[4096];
 runtime_boot_progress(TOTK_UI("读取资源索引","Reading the data index"),0,0);map_index(dir,ROMFS_INDEX_SHA256,ROMFS_IMAGE_SIZE,&rom);
 struct stat st;need(snprintf(path,sizeof(path),"%s/romfs.bin",dir)<(int)sizeof(path),"RomFS image path");image_fd=open(path,O_RDONLY);
 if(image_fd<0)open_chunks(dir);
 else need(fstat(image_fd,&st)==0&&(uint64_t)st.st_size==ROMFS_IMAGE_SIZE,"RomFS image size");
 verify_image(image_fd,dir);
 if(!nchunks&&getenv("TOTK_ROMFS_MMAP")&&strcmp(getenv("TOTK_ROMFS_MMAP"),"0")){
  void *mapped=mmap(NULL,(size_t)ROMFS_IMAGE_SIZE,PROT_READ,MAP_PRIVATE,image_fd,0);
  if(mapped!=MAP_FAILED){image_map=mapped;fprintf(stderr,"[totk] RomFS mmap read path enabled\n");}
  else fprintf(stderr,"[totk] RomFS mmap failed (%s); using pread\n",strerror(errno));
 }
 runtime_boot_status(TOTK_UI("准备游戏资源","Preparing game data"),TOTK_UI("检查分辨率补丁并挂载只读游戏资源","Checking the resolution patch and mounting the read-only game data"));
 overlay_init();
 fprintf(stderr,"[totk] native read-only RomFS ready: %llu files\n",(unsigned long long)rom.count);
}
static int path_ok(const char *p) {return romfs_path_valid_relative(p);}
static uint64_t lower(const char *name) {return vlower(&rom,name);}
/* Add-on content (DLC): <TOTK_AOC, default extracted/<version>/aoc>/manifest.tsv (build_app.py step_dlc) lists one
   RomFS per DLC the build verified: nn::aoc index, title ID, image size, image SHA-256, index SHA-256. Each image is
   hashed like the game's RomFS (a record of the full hash is reused while the file is unchanged). nn::aoc lists these
   indices only; nn::fs::MountAddOnContent mounts one under the game's name (BOTW: index 1 as aoc:). TOTK_AOC=0: none. */
#define NAOC 8
static struct { int index; char title[17]; View view; int fd; char mount[32]; } aoc[NAOC];
static unsigned naoc;
static pthread_once_t aoc_once=PTHREAD_ONCE_INIT;
static void aoc_load(void) {
 const char *dir=getenv("TOTK_AOC");if(dir&&!strcmp(dir,"0")){fprintf(stderr,"[totk] add-on content: off (TOTK_AOC=0)\n");return;}
 if(!dir)dir="extracted/" TOTK_GAME_DIR "/aoc";
 char path[4096];need(snprintf(path,sizeof(path),"%s/manifest.tsv",dir)<(int)sizeof(path),"add-on content path");FILE *m=fopen(path,"r");
 if(!m){fprintf(stderr,"[totk] add-on content: none (%s)\n",path);return;}
 char line[1024];
 while(fgets(line,sizeof(line),m)){
  if(line[0]=='#'||line[0]=='\n')continue;int index;char title[17],sha[65],isha[65];unsigned long long size;
  need(sscanf(line,"%d\t%16[0-9A-F]\t%llu\t%64[0-9a-f]\t%64[0-9a-f]",&index,title,&size,sha,isha)==5&&strlen(title)==16&&strlen(sha)==64&&strlen(isha)==64,"add-on content manifest line");
  need(index>0&&naoc<NAOC,"add-on content index or count");for(unsigned k=0;k<naoc;k++)need(aoc[k].index!=index,"add-on content: an index listed twice");
  char folder[4096];need(snprintf(folder,sizeof(folder),"%s/%s",dir,title)<(int)sizeof(folder),"add-on content folder");
  need(snprintf(path,sizeof(path),"%s/romfs.bin",folder)<(int)sizeof(path),"add-on content image path");int fd=open(path,O_RDONLY);struct stat st;
  need(fd>=0&&fstat(fd,&st)==0&&(uint64_t)st.st_size==size,"add-on content image missing or wrong size (rebuild the app)");
  char proof[64],what[64];snprintf(proof,sizeof(proof),".aoc-%s-verified-v1",title);snprintf(what,sizeof(what),"DLC %s RomFS",title);
  verify_file(fd,folder,sha,proof,what);
  aoc[naoc].index=index;memcpy(aoc[naoc].title,title,17);aoc[naoc].fd=fd;map_index(folder,isha,size,&aoc[naoc].view);
  fprintf(stderr,"[totk] add-on content %d (%s): %llu files\n",index,title,(unsigned long long)aoc[naoc].view.count);naoc++;
 }
 fclose(m);
}
static void aoc_init(void) {pthread_once(&aoc_once,aoc_load);}
int runtime_aoc_count(void) {aoc_init();return (int)naoc;}
int runtime_aoc_list(int *out,int offset,int count) {
 aoc_init();if(offset<0||count<0||(!out&&count))runtime_fail("invalid add-on content list range");
 int n=0;for(unsigned k=(unsigned)offset;k<naoc&&n<count;k++)out[n++]=aoc[k].index;return n;
}
static unsigned mount_aoc(const char *name,int index,void *cache,uint64_t size) {
 (void)cache;(void)size;aoc_init();if(!name||!path_ok(name)||strlen(name)>=sizeof(aoc[0].mount)||strchr(name,'/')||strchr(name,':'))return INVALID;
 pthread_mutex_lock(&lock);
 int busy=!strcmp(name,mount_name);for(unsigned k=0;k<naoc;k++)busy|=!strcmp(name,aoc[k].mount);
 for(unsigned k=0;k<naoc;k++)if(aoc[k].index==index){
  if(busy||*aoc[k].mount){pthread_mutex_unlock(&lock);runtime_fail("add-on content mount name or index already mounted");}
  strcpy(aoc[k].mount,name);pthread_mutex_unlock(&lock);
  fprintf(stderr,"[totk] add-on content %d (%s) mounted as %s:\n",index,aoc[k].title,name);return 0;}
 pthread_mutex_unlock(&lock);fprintf(stderr,"[totk] add-on content %d is not packaged\n",index);return NOT_FOUND;
}
static unsigned aoc_cache_size(uint64_t *out,int index) {(void)index;if(!out)return INVALID;*out=0;return 0;}
/* The volume a mounted path names (0: the game's RomFS, k: aoc[k-1]) and the path inside it; -1 if none. */
static int resolve(const char *path,const char **rel) {
 const char *p=romfs_path_relative(mount_name,path);if(p){*rel=p;return 0;}
 for(unsigned k=0;k<naoc;k++)if(*aoc[k].mount&&(p=romfs_path_relative(aoc[k].mount,path))){*rel=p;return (int)k+1;}
 return -1;
}
static const View *volume(int v) {return v?&aoc[v-1].view:&rom;}
static unsigned mount_rom(const char *name,void *cache,uint64_t size) {
 (void)cache;(void)size;runtime_fs_init();if(!name||!path_ok(name)||strlen(name)>=sizeof(mount_name)||strchr(name,'/')||strchr(name,':'))return INVALID;
 if(*mount_name&&strcmp(name,mount_name))runtime_fail("second RomFS mount not implemented");strcpy(mount_name,name);return 0;
}
static unsigned cache_size(uint64_t *out) {if(!out)return INVALID;*out=0;return 0;}
/* TOTK_FS_LOG=1: log every RomFS open with the time since start (shows how far the game gets without a picture). */
static void fs_log(const char *what,const char *path){static int on=-1;static uint64_t t0;if(on<0){on=getenv("TOTK_FS_LOG")!=NULL;t0=runtime_now_ns();}if(on)fprintf(stderr,"[totk-fs] %8.3f %s %s\n",(runtime_now_ns()-t0)/1e9,what,path);}
void runtime_fs_log(const char *what,const char *path){fs_log(what,path);}
unsigned runtime_romfs_open(uint64_t *out,const char *path,int mode) {
 fs_log("rom",path?path:"(null)");
 if(!out||mode!=1)return UNSUPPORTED;const char *p;int v=resolve(path,&p);if(v<0)return NOT_FOUND;const View *ix=volume(v);
 uint64_t i=vlower(ix,p);if(i>=ix->count||strcmp(p,vname(ix,ventry(ix,i))))return NOT_FOUND;
 uint8_t ov=0;if(!v)for(unsigned k=0;k<noverlay;k++)if(overlays[k].index==i)ov=(uint8_t)(k+1);
 pthread_mutex_lock(&lock);for(unsigned j=1;j<NH;j++)if(!handles[j]){handles[j]=i+1;handle_ovl[j]=ov;handle_vol[j]=(uint8_t)v;*out=0x100000000ULL+j;pthread_mutex_unlock(&lock);return 0;}pthread_mutex_unlock(&lock);runtime_fail("file handle table full");return INVALID;
}
static int handle_entry(uint64_t h,Entry *e) {if(h<=0x100000000ULL||h>=0x100000000ULL+NH)return 0;pthread_mutex_lock(&lock);uint64_t i=handles[h-0x100000000ULL];uint8_t v=handle_vol[h-0x100000000ULL];pthread_mutex_unlock(&lock);if(!i)return 0;*e=ventry(volume(v),i-1);return 1;}
/* Where an open handle's bytes come from: the overlay file, or the original's range of its image. */
static void handle_source(uint64_t h,const Entry *e,int *fd,uint64_t *base,uint64_t *size) {
 pthread_mutex_lock(&lock);uint8_t ov=handle_ovl[h-0x100000000ULL],v=handle_vol[h-0x100000000ULL];pthread_mutex_unlock(&lock);
 if(ov){*fd=overlays[ov-1].fd;*base=0;*size=overlays[ov-1].size;}else{*fd=v?aoc[v-1].fd:image_fd;*base=e->offset;*size=e->size;}
}
unsigned runtime_romfs_read(uint64_t *out,uint64_t h,int64_t offset,void *buffer,uint64_t size) {
 Entry e;if(!out||offset<0||!handle_entry(h,&e))return INVALID;int fd;uint64_t base,fsize;handle_source(h,&e,&fd,&base,&fsize);if((uint64_t)offset>fsize||(!buffer&&size))return INVALID;
 uint64_t n=size;if(n>fsize-(uint64_t)offset)n=fsize-(uint64_t)offset;uint64_t done=0;
 static _Atomic int profile_mode=-1;int profile=atomic_load(&profile_mode);if(profile<0){profile=getenv("TOTK_FS_TIMING")!=NULL;atomic_store(&profile_mode,profile);}uint64_t prep_start=profile?runtime_now_ns():0;
 dm_guest_write_prepare(buffer,(size_t)n);
 uint64_t t0=profile?runtime_now_ns():0;
 if(fd==image_fd&&image_map){memcpy(buffer,image_map+base+(uint64_t)offset,(size_t)n);done=n;}
 else while(done<n){size_t chunk=(size_t)(n-done);if(chunk>8<<20)chunk=8<<20;ssize_t r=src_pread(fd,(uint8_t *)buffer+done,chunk,(off_t)(base+(uint64_t)offset+done));if(r<=0)runtime_fail("RomFS read failure");done+=(uint64_t)r;}
 if(profile){uint64_t now=runtime_now_ns();atomic_fetch_add(&read_calls,1);atomic_fetch_add(&read_bytes,done);atomic_fetch_add(&read_ns,now-t0);atomic_fetch_add(&read_prepare_ns,t0-prep_start);
  uint64_t last=atomic_load(&read_report_ns);
  if(now-last>1000000000ULL&&atomic_compare_exchange_strong(&read_report_ns,&last,now))
   fprintf(stderr,"[totk-fs] read window t=%.3f calls=%llu bytes=%llu wall_ms=%.3f prepare_ms=%.3f\n",now/1e9,
    (unsigned long long)atomic_exchange(&read_calls,0),(unsigned long long)atomic_exchange(&read_bytes,0),atomic_exchange(&read_ns,0)/1e6,atomic_exchange(&read_prepare_ns,0)/1e6);}
 *out=done;return 0;
}
unsigned runtime_romfs_size(int64_t *out,uint64_t h) {Entry e;if(!out||!handle_entry(h,&e))return INVALID;int fd;uint64_t base,size;handle_source(h,&e,&fd,&base,&size);*out=(int64_t)size;return 0;}
void runtime_romfs_close(uint64_t h) {Entry e;if(!handle_entry(h,&e))runtime_fail("close invalid file handle");pthread_mutex_lock(&lock);handles[h-0x100000000ULL]=0;handle_ovl[h-0x100000000ULL]=0;handle_vol[h-0x100000000ULL]=0;pthread_mutex_unlock(&lock);}
unsigned runtime_romfs_entry_type(int *out,const char *path) {
 if(!out)return INVALID;const char *p;int v=resolve(path,&p);if(v<0)return NOT_FOUND;const View *ix=volume(v);
 uint64_t i=vlower(ix,p);if(i<ix->count&&!strcmp(vname(ix,ventry(ix,i)),p)){*out=1;return 0;}
 char dir[4096];if(snprintf(dir,sizeof(dir),"%s/",p)>=(int)sizeof(dir))return INVALID;i=vlower(ix,dir);if(i<ix->count&&!strncmp(vname(ix,ventry(ix,i)),dir,strlen(dir))){*out=0;return 0;}return NOT_FOUND;
}
static int owns(const char *mount,const char *path) {size_t n=strlen(mount);return n&&path&&!strncmp(path,mount,n)&&path[n]==':';}
int runtime_romfs_owns(const char *path) {
 if(owns(mount_name,path))return 1;for(unsigned k=0;k<naoc;k++)if(owns(aoc[k].mount,path))return 1;return 0;
}
/* nn::fs::Unmount of a read-only mount (name without the colon): an add-on content mount is released once its files
   are closed; the game's RomFS stays mounted. 0: not one of these mounts. */
int runtime_romfs_unmount(const char *name) {
 if(!name)return 0;if(*mount_name&&!strcmp(name,mount_name))return 1;
 pthread_mutex_lock(&lock);
 for(unsigned k=0;k<naoc;k++)if(*aoc[k].mount&&!strcmp(name,aoc[k].mount)){
  for(unsigned j=1;j<NH;j++)if(handles[j]&&handle_vol[j]==k+1){pthread_mutex_unlock(&lock);runtime_fail("unmount of add-on content with open files");}
  *aoc[k].mount=0;pthread_mutex_unlock(&lock);fprintf(stderr,"[totk] add-on content %d unmounted\n",aoc[k].index);return 1;}
 pthread_mutex_unlock(&lock);return 0;
}
int runtime_romfs_handle(uint64_t h) {return h>0x100000000ULL&&h<0x100000000ULL+NH;}
unsigned runtime_romfs_read_options(uint64_t *out,uint64_t h,int64_t off,void *buf,uint64_t size) {return runtime_romfs_read(out,h,off,buf,size);}
void *runtime_fs_override(const char *name) {
 static const struct{const char *name;void *fn;} map[]={
 {"_ZN2nn2fs8MountRomEPKcPvm",mount_rom},{"_ZN2nn2fs22QueryMountRomCacheSizeEPm",cache_size},
 {"_ZN2nn2fs17MountAddOnContentEPKciPvm",mount_aoc},{"_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi",aoc_cache_size}};
 for(size_t i=0;i<sizeof(map)/sizeof(map[0]);i++)if(!strcmp(name,map[i].name))return map[i].fn;return runtime_fs_host_override(name);
}
void runtime_fs_probe(void) {
 runtime_fs_init();need(mount_rom("rom",NULL,0)==0,"mount RomFS");unsigned count=0;uint8_t buffer[257];uint64_t sum=0;
 /* Audio stream paths include an extra slash after the mount root. Verify
    actual resource lookup and bytes, while still rejecting path traversal. (A TOTK stream; BOTW has no such file.) */
#ifndef GAME_BOTW
 {
  const char *name="Sound/Resource/Stream/Dm_OP_0001.bwav";
  char path[256];uint8_t expected[257],actual[257];uint64_t h,n;
  snprintf(path,sizeof path,"rom:/%s",name);
  need(!runtime_romfs_open(&h,path,1),"open normal audio path");
  need(!runtime_romfs_read(&n,h,0,expected,sizeof expected)&&n==sizeof expected,"read normal audio path");runtime_romfs_close(h);
  for(int slashes=2;slashes<=3;slashes++){
   snprintf(path,sizeof path,slashes==2?"rom://%s":"rom:///%s",name);
   int type=-1;need(!runtime_romfs_entry_type(&type,path)&&type==1,"normalized audio entry type");
   need(!runtime_romfs_open(&h,path,1),"open normalized audio stream path");
   need(!runtime_romfs_read(&n,h,0,actual,sizeof actual)&&n==sizeof actual&&!memcmp(actual,expected,sizeof actual),"normalized audio stream bytes");runtime_romfs_close(h);
  }
  need(runtime_romfs_open(&h,"rom://../Sound/Resource/Stream/Dm_OP_0001.bwav",1)!=0,"reject traversal after repeated root separator");
 }
#endif
 for(uint64_t i=0;i<rom.count;i+=rom.count/64){Entry e=entry(i);char p[4096];need(snprintf(p,sizeof(p),"rom:/%s",entry_name(e))<(int)sizeof(p),"probe path");uint64_t h=0,n=0;int64_t size;need(!runtime_romfs_open(&h,p,1)&&!runtime_romfs_size(&size,h)&&(uint64_t)size==e.size,"probe open/size");need(!runtime_romfs_read(&n,h,0,buffer,sizeof(buffer)),"probe partial read");for(uint64_t j=0;j<n;j++)sum=sum*131+buffer[j];need(!runtime_romfs_read(&n,h,size,buffer,1)&&n==0,"probe EOF");runtime_romfs_close(h);count++;}
 for(unsigned k=0;k<noverlay;k++){Entry e=entry(overlays[k].index);char p[4096];need(snprintf(p,sizeof(p),"rom:/%s",entry_name(e))<(int)sizeof(p),"probe overlay path");uint64_t oh=0,n=0;int64_t size=0;uint8_t a[64],b[64];
  need(!runtime_romfs_open(&oh,p,1)&&!runtime_romfs_size(&size,oh)&&(uint64_t)size==overlays[k].size,"probe overlay size");need(!runtime_romfs_read(&n,oh,size-64,a,64)&&n==64&&pread(overlays[k].fd,b,64,(off_t)(size-64))==64&&!memcmp(a,b,64),"probe overlay bytes");runtime_romfs_close(oh);}
 uint64_t h=0;need(runtime_romfs_open(&h,"rom:/../main",1)==NOT_FOUND,"reject traversal");need(runtime_romfs_open(&h,"rom:/absent",2)==UNSUPPORTED,"reject write open");
 /* Add-on content: listed in index order, each mounted, sampled like the RomFS, then unmounted. */
 int listed[NAOC];unsigned aoc_count=0;need(runtime_aoc_list(listed,0,NAOC)==runtime_aoc_count(),"list add-on content");
 need(mount_aoc("aoc",NAOC*1000,NULL,0)==NOT_FOUND,"absent add-on content index");
 for(unsigned k=0;k<naoc;k++){
  need(listed[k]==aoc[k].index&&(!k||listed[k-1]<listed[k]),"add-on content list order");
  need(mount_aoc("aoc",aoc[k].index,NULL,0)==0&&runtime_romfs_owns("aoc:/x"),"mount add-on content");
  const View *v=&aoc[k].view;int type=-1;
  for(uint64_t i=0;i<v->count;i+=v->count/16+1){Entry e=ventry(v,i);char p[4096];need(snprintf(p,sizeof(p),"aoc:/%s",vname(v,e))<(int)sizeof(p),"probe add-on content path");uint64_t n=0;int64_t size;
   need(!runtime_romfs_open(&h,p,1)&&!runtime_romfs_size(&size,h)&&(uint64_t)size==e.size,"probe add-on content open/size");need(!runtime_romfs_read(&n,h,0,buffer,sizeof(buffer))&&n==(e.size<sizeof(buffer)?e.size:sizeof(buffer)),"probe add-on content read");
   uint8_t direct[257];need(pread(aoc[k].fd,direct,n,(off_t)e.offset)==(ssize_t)n&&!memcmp(direct,buffer,n),"probe add-on content bytes");runtime_romfs_close(h);aoc_count++;}
  char first[4096];need(v->count&&snprintf(first,sizeof(first),"aoc:/%s",vname(v,ventry(v,0)))<(int)sizeof(first),"probe add-on content first file");
  char *slash=strchr(first+5,'/');if(slash){*slash=0;need(!runtime_romfs_entry_type(&type,first)&&type==0,"add-on content directory entry");*slash='/';}
  need(runtime_romfs_unmount("aoc")&&!runtime_romfs_owns("aoc:/x")&&runtime_romfs_open(&h,first,1)==NOT_FOUND,"unmount add-on content");}
 printf("{\"status\":\"pass\",\"files\":%llu,\"sampled_files\":%u,\"overlays\":%u,\"aoc\":%u,\"aoc_sampled_files\":%u,\"read_digest\":\"%016llx\",\"scope\":\"read-only mount/open/size/read/EOF, verified overlays, add-on content mount/list/unmount; no game startup\"}\n",(unsigned long long)rom.count,count,noverlay,naoc,aoc_count,(unsigned long long)sum);
}
