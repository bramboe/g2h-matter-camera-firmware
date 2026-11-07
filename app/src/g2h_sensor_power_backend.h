/* Exact-layout research adapter, excluded from the normal camera build.
 * It must only be used after the trial controller has verified the exact native
 * library digests in the retained analysis. No raw I2C, GPIO or register write.
 * Native power-off closes the sensor I2C interface; native power-on+init restores
 * its hardware configuration. Runtime effect/recovery is NOT yet qualified.
 */
#pragma once
#include "g2h_sensor_power_sequence.h"
#include <stdint.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
static void *g2h_native_pointer(const void *base,size_t offset){
    uint32_t address=0;memcpy(&address,(const char*)base+offset,4);return (void*)(uintptr_t)address;
}
static int g2h_native_location(void *address,const char *path,uintptr_t offset){
    Dl_info info;memset(&info,0,sizeof(info));
    return dladdr(address,&info)&&info.dli_fname&&!strcmp(info.dli_fname,path)&&
        (((uintptr_t)address&~(uintptr_t)1)-(uintptr_t)info.dli_fbase)==offset;
}
static int g2h_native_sensor_power(int on){
    if(sizeof(void*)!=4)return -ENOTSUP;
    const char *backend=getenv("G2H_CAPTURE_BACKEND");
    if(!backend||strcmp(backend,"gc2053-power-experiment"))return -ENOTSUP;
    void *library=dlopen("/lib/libcamera.so",RTLD_NOW|RTLD_NOLOAD);
    if(!library)return -EAGAIN;
    void *camera=dlsym(library,"strCameraInfo");
    struct g2h_sensor_ops ops;memset(&ops,0,sizeof(ops));
    int valid=g2h_native_location(camera,"/lib/libcamera.so",0x34278);
    if(valid){
        void *root=g2h_native_pointer(camera,156);
        valid=g2h_native_pointer(camera,132)&&g2h_native_pointer(camera,160)&&root;
        if(valid){
            ops.context=g2h_native_pointer(root,0x1b8);
            valid=ops.context==(char*)root+8&&!memcmp((char*)ops.context+2,"gc2053",6);
            if(valid){
                ops.mutex=(pthread_mutex_t*)g2h_native_pointer(root,0x350);
                ops.power_off=(g2h_sensor_call)g2h_native_pointer(ops.context,0x144);
                ops.power_on=(g2h_sensor_call)g2h_native_pointer(ops.context,0x140);
                ops.initialize=(g2h_sensor_call)g2h_native_pointer(ops.context,0x148);
                const char *driver="/lib/libdrv_ms_cus_gc2053.so";
                valid=ops.mutex&&!((uintptr_t)ops.mutex&3)&&
                    g2h_native_location((void*)ops.power_off,driver,0x9f4)&&
                    g2h_native_location((void*)ops.power_on,driver,0x134c)&&
                    g2h_native_location((void*)ops.initialize,driver,0x1520);
            }
        }
    }
    const int result=!valid?-EAGAIN:on<0?0:g2h_sensor_power_sequence(&ops,on);
    dlclose(library);return result;
}
