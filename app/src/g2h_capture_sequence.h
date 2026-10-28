/* Own sequencing: request results are never physical-state observations. */
#pragma once
#include <errno.h>
#include <stdbool.h>
struct g2h_capture_ops {
 int (*ready)(void);
 int (*sensor)(int on);
 int (*record_running)(void);
 int (*record_stop)(void);
 int (*record_start)(void);
};
struct g2h_capture_memory {bool restore_recorder;};
static int g2h_capture_transition(const struct g2h_capture_ops *ops,struct g2h_capture_memory *memory,int on){
 if(!ops||!memory||!ops->ready||!ops->sensor||!ops->record_running||!ops->record_stop||!ops->record_start)return -ENOSYS;
 int rc=ops->ready();if(rc)return rc;
 if(!on){
  const int running=ops->record_running();if(running<0)return running;
  if(running){memory->restore_recorder=true;rc=ops->record_stop();if(rc)return rc;}
  return ops->sensor(0);
 }
 rc=ops->sensor(1);if(rc)return rc;
 if(memory->restore_recorder){
  const int running=ops->record_running();if(running<0)return running;
  if(!running){rc=ops->record_start();if(rc)return rc;}
  memory->restore_recorder=false;
 }
 return 0;
}
