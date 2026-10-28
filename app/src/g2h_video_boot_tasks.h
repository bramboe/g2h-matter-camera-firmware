/* Experimental boot-time task filter, never a physical-state assertion. */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
typedef int (*g2h_task_start_fn)(void *,const char *,unsigned);
struct g2h_boot_tasks {bool blocked;g2h_task_start_fn original;atomic_uint suppressed;};
static unsigned g2h_video_task_bit(const char *name){
 if(!name)return 0;
 const char *names[]={"video_task","record_task","rtsp_task","short_record_task","cloud_record_task"};
 for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i)if(!strcmp(name,names[i]))return 1u<<i;
 return 0;
}
static int g2h_boot_task_start(struct g2h_boot_tasks *gate,void *context,const char *name,unsigned argument){
 const unsigned bit=g2h_video_task_bit(name);
 if(gate->blocked&&bit){atomic_fetch_or(&gate->suppressed,bit);return 0;}
 return gate->original(context,name,argument);
}
