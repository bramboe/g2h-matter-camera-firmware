/* Exact-profile optional boot gate. The wrapper must verify the full
 * native image hash before preloading. No file in the vendor image is edited.
 * Boot with G2H_VIDEO_BOOT_BLOCKED=1 to block capture/record/RTSP tasks. */
#define _GNU_SOURCE
#include "g2h_video_boot_tasks.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stddef.h>
#include <errno.h>
_Static_assert(sizeof(struct stat)==88 && offsetof(struct stat,st_mode)==16 && offsetof(struct stat,st_uid)==24,"native stat ABI");
extern int __lxstat(int,const char*,struct stat*);
extern int __fxstat(int,int,struct stat*);
static struct g2h_boot_tasks gate;
static int start(void *context,const char *name,unsigned argument){return g2h_boot_task_start(&gate,context,name,argument);}
static bool writable_slot(uintptr_t address){
 FILE *file=fopen("/proc/self/maps","r");if(!file)return false;
 char line[512],permissions[5];unsigned long low,high;bool found=false;
 while(fgets(line,sizeof(line),file))if(sscanf(line,"%lx-%lx %4s",&low,&high,permissions)==3&&low<=address&&address+sizeof(void*)<=high&&permissions[0]=='r'&&permissions[1]=='w'){found=true;break;}
 fclose(file);return found;
}
__attribute__((constructor)) static void install(void){
 const char *enabled=getenv("G2H_VIDEO_BOOT_BLOCKED");if(!enabled||strcmp(enabled,"1"))return;
 /* Fail closed on a video-disabled boot. Qualification uses an independent
  * boot guardian; a committed installation retains explicit terminal recovery. */
 if(sizeof(void*)!=4||geteuid()!=0)_exit(125);
 Dl_info info={0};
 if(!dladdr((void*)(uintptr_t)0x457e5,&info)||!info.dli_fname||strcmp(info.dli_fname,"/tmp/out/camera")||(uintptr_t)info.dli_fbase!=0x8000)_exit(125);
 uintptr_t *slot=(uintptr_t*)(uintptr_t)(0x430ab0+0x5c);
 if(!writable_slot((uintptr_t)slot)||*slot!=0x457e5)_exit(125);
 /* A private pause request suppresses our cached/output copies during this
  * video-disabled boot. It is not a declaration of measured sensor state. */
 if(mkdir("/tmp/g2h-capture-control",0700)&&errno!=EEXIST)_exit(125);
 struct stat st;
 if(__lxstat(3,"/tmp/g2h-capture-control",&st)||!S_ISDIR(st.st_mode)||st.st_uid!=0||(st.st_mode&077))_exit(125);
 int fd=open("/tmp/g2h-capture-control/paused",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
 if(fd<0)_exit(125);
 if(__fxstat(3,fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||(st.st_mode&077)||st.st_nlink!=1)_exit(125);
 close(fd);
 gate.blocked=true;gate.original=(g2h_task_start_fn)(uintptr_t)*slot;
 *slot=(uintptr_t)start;
}
