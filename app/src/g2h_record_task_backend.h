/* Research-only exact native task framework, excluded from normal app build.
 * Controller must bind the full native executable hash before loading this.
 * No config/defaults are changed; remember whether the recorder originally ran.
 * API flags/return values are not evidence of SD writes or physical capture.
 */
#pragma once
#include "g2h_capture_sequence.h"
static int g2h_record_bindings(void){
 if(sizeof(void*)!=4)return -ENOTSUP;
 const char *path="/tmp/out/camera";
 return g2h_native_location((void*)(uintptr_t)0x46821,path,0x3e820)&&
        g2h_native_location((void*)(uintptr_t)0x4690d,path,0x3e90c)&&
        g2h_native_location((void*)(uintptr_t)0x46899,path,0x3e898)?0:-EAGAIN;
}
static int g2h_record_running(void){
 int rc=g2h_record_bindings();if(rc)return rc;
 typedef int (*query)(const char*);
 const int value=((query)(uintptr_t)0x46821)("record_task");
 return value==0||value==1?value:-EIO;
}
static int g2h_record_stop(void){
 int rc=g2h_record_bindings();if(rc)return rc;
 typedef void (*stop_task)(const char*);
 ((stop_task)(uintptr_t)0x4690d)("record_task");
 return g2h_record_running()==0?0:-EIO;
}
static int g2h_record_start(void){
 int rc=g2h_record_bindings();if(rc)return rc;
 typedef void (*start_task)(const char*,int);
 ((start_task)(uintptr_t)0x46899)("record_task",0);
 return g2h_record_running()==1?0:-EIO;
}
static int g2h_capture_native_ready(void){
 int rc=g2h_native_sensor_power(-1);return rc?rc:g2h_record_bindings();
}
static int g2h_capture_native_transition(int on){
 static struct g2h_capture_memory memory;
 const char *enabled=getenv("G2H_CAPTURE_STOP_RECORDING");
 if(!enabled||strcmp(enabled,"1"))return g2h_native_sensor_power(on);
 const struct g2h_capture_ops ops={g2h_capture_native_ready,g2h_native_sensor_power,g2h_record_running,g2h_record_stop,g2h_record_start};
 return g2h_capture_transition(&ops,&memory,on);
}
