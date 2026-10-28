#include "../app/src/g2h_video_boot_tasks.h"
#include <assert.h>
#include <stdio.h>
static void *seen_context;static const char *seen_name;static unsigned seen_argument,calls;
static int original(void *context,const char *name,unsigned argument){seen_context=context;seen_name=name;seen_argument=argument;++calls;return -17;}
int main(void){
 struct g2h_boot_tasks gate={.blocked=true,.original=original};int context=1;
 const char *blocked[]={"video_task","record_task","rtsp_task","short_record_task","cloud_record_task"};
 for(unsigned i=0;i<5;++i){assert(!g2h_boot_task_start(&gate,&context,blocked[i],42));assert(calls==0);}
 assert(atomic_load(&gate.suppressed)==31);
 const char *allowed[]={"audio_task","property_task","video_task_extra","Video_task","",NULL};
 for(unsigned i=0;i<6;++i){assert(g2h_boot_task_start(&gate,&context,allowed[i],42)==-17);assert(seen_context==&context&&seen_name==allowed[i]&&seen_argument==42);}
 gate.blocked=false;
 for(unsigned i=0;i<5;++i)assert(g2h_boot_task_start(&gate,&context,blocked[i],99)==-17);
 assert(calls==11&&seen_argument==99);puts("boot task filter tests passed");
}
