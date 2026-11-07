/* Own experimental sequencing only. Native callbacks are resolved separately.
 * Return values describe requests, not measured capture state. */
#pragma once
#include <errno.h>
#include <pthread.h>
typedef int (*g2h_sensor_call)(void *);
struct g2h_sensor_ops {
    void *context;
    pthread_mutex_t *mutex;
    g2h_sensor_call power_off,power_on,initialize;
};
static int g2h_sensor_power_sequence(const struct g2h_sensor_ops *ops,int on){
    if(!ops||!ops->context||!ops->mutex||!ops->power_off||!ops->power_on||!ops->initialize)return -ENOSYS;
    const int locked=pthread_mutex_lock(ops->mutex);if(locked)return -locked;
    int result;
    if(!on)result=ops->power_off(ops->context);
    else{
        result=ops->power_on(ops->context);
        if(!result){
            result=ops->initialize(ops->context);
            /* Do not leave a half-initialized sensor running after failure.
             * Preserve the original error; rollback is not an observed state. */
            if(result)(void)ops->power_off(ops->context);
        }
    }
    pthread_mutex_unlock(ops->mutex);return result;
}
