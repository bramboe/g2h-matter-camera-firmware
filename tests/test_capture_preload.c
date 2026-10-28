/* Fake native entry point and real private Unix server; never accesses a camera. */
#define G2H_CAPTURE_TESTING
#include "../app/src/g2h_capture_preload.c"
#include <assert.h>
#include "../app/src/g2h_sensor_power_sequence.h"
#include "../app/src/g2h_capture_sequence.h"

static pthread_mutex_t fake_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_changed=PTHREAD_COND_INITIALIZER;
static bool fake_hold,fake_entered,fake_release;
static int fake_rc,trace[64];
static unsigned count;
static int fake_preview(int on){
    pthread_mutex_lock(&fake_mutex);assert(count<64);trace[count++]=on;
    fake_entered=true;pthread_cond_broadcast(&fake_changed);
    while(fake_hold&&!fake_release)pthread_cond_wait(&fake_changed,&fake_mutex);
    const int result=fake_rc;pthread_mutex_unlock(&fake_mutex);return result;
}
static void fake_arm(bool hold,int rc){
    pthread_mutex_lock(&fake_mutex);fake_hold=hold;fake_rc=rc;fake_entered=false;fake_release=false;pthread_mutex_unlock(&fake_mutex);
}
static void fake_wait(void){
    struct timespec until;assert(!clock_gettime(CLOCK_REALTIME,&until));until.tv_sec+=2;
    pthread_mutex_lock(&fake_mutex);
    while(!fake_entered)assert(!pthread_cond_timedwait(&fake_changed,&fake_mutex,&until));
    pthread_mutex_unlock(&fake_mutex);
}
static void fake_continue(void){
    pthread_mutex_lock(&fake_mutex);fake_hold=false;fake_release=true;pthread_cond_broadcast(&fake_changed);pthread_mutex_unlock(&fake_mutex);
}
static void command(const char *directory,const char *text,char out[512]){
    int fd=socket(AF_UNIX,SOCK_STREAM,0);assert(fd>=0);
    struct sockaddr_un address;memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;
    assert(snprintf(address.sun_path,sizeof(address.sun_path),"%s/control.sock",directory)>0);
    assert(!connect(fd,(struct sockaddr*)&address,sizeof(address)));
    assert(send(fd,text,strlen(text),0)==(ssize_t)strlen(text));size_t used=0;
    for(;;){
        struct pollfd p={fd,POLLIN,0};assert(poll(&p,1,1500)>0);
        ssize_t n=recv(fd,out+used,511-used,0);assert(n>=0);if(!n)break;
        used+=(size_t)n;assert(used<511);
    }
    out[used]=0;close(fd);
}
static void await_idle(const char *directory){
    char response[512];const long long until=monotonic_ms()+2000;
    for(;;){command(directory,"STATUS\n",response);if(strstr(response,"\"pending\":false"))return;
        assert(monotonic_ms()<until);poll(NULL,0,5);}
}
static int power_trace[8],power_count,power_failure;
static int power_off(void *ctx){assert(ctx==power_trace);power_trace[power_count++]=0;return 0;}
static int power_on(void *ctx){assert(ctx==power_trace);power_trace[power_count++]=1;return power_failure==1?-EIO:0;}
static int sensor_init(void *ctx){assert(ctx==power_trace);power_trace[power_count++]=2;return power_failure==2?-EIO:0;}
static void test_power_sequence(void){
    pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
    const struct g2h_sensor_ops ops={power_trace,&mutex,power_off,power_on,sensor_init};
    assert(g2h_sensor_power_sequence(NULL,0)==-ENOSYS);
    power_count=0;power_failure=0;assert(!g2h_sensor_power_sequence(&ops,0));assert(power_count==1&&power_trace[0]==0);
    power_count=0;assert(!g2h_sensor_power_sequence(&ops,1));assert(power_count==2&&power_trace[0]==1&&power_trace[1]==2);
    power_count=0;power_failure=1;assert(g2h_sensor_power_sequence(&ops,1)==-EIO);assert(power_count==1&&power_trace[0]==1);
    power_count=0;power_failure=2;assert(g2h_sensor_power_sequence(&ops,1)==-EIO);assert(power_count==3&&power_trace[0]==1&&power_trace[1]==2&&power_trace[2]==0);
    assert(!pthread_mutex_trylock(&mutex));pthread_mutex_unlock(&mutex);pthread_mutex_destroy(&mutex);
}
static char sequence_trace[64];static unsigned sequence_count;static int record_active,sequence_fail;
static void sequence_step(char step){assert(sequence_count<63);sequence_trace[sequence_count++]=step;sequence_trace[sequence_count]=0;}
static int capture_ready(void){sequence_step('Q');return sequence_fail==1?-EAGAIN:0;}
static int capture_sensor(int on){sequence_step(on?'I':'O');return sequence_fail==(on?3:5)?-EIO:0;}
static int capture_running(void){sequence_step('R');return record_active;}
static int capture_stop(void){sequence_step('S');if(sequence_fail==2)return -EIO;record_active=0;return 0;}
static int capture_start(void){sequence_step('T');if(sequence_fail==4)return -EIO;record_active=1;return 0;}
static void sequence_reset(int fail){sequence_count=0;sequence_trace[0]=0;sequence_fail=fail;}
static void test_capture_sequence(void){
 const struct g2h_capture_ops ops={capture_ready,capture_sensor,capture_running,capture_stop,capture_start};
 struct g2h_capture_memory memory={0};assert(g2h_capture_transition(NULL,&memory,0)==-ENOSYS);
 record_active=0;sequence_reset(0);assert(!g2h_capture_transition(&ops,&memory,0));assert(!strcmp(sequence_trace,"QRO")&&!memory.restore_recorder);
 sequence_reset(0);assert(!g2h_capture_transition(&ops,&memory,1));assert(!strcmp(sequence_trace,"QI")&&!record_active);
 record_active=1;sequence_reset(0);assert(!g2h_capture_transition(&ops,&memory,0));assert(!strcmp(sequence_trace,"QRSO")&&memory.restore_recorder&&!record_active);
 sequence_reset(0);assert(!g2h_capture_transition(&ops,&memory,0));assert(!strcmp(sequence_trace,"QRO")&&memory.restore_recorder);
 sequence_reset(3);assert(g2h_capture_transition(&ops,&memory,1)==-EIO);assert(!strcmp(sequence_trace,"QI")&&!record_active&&memory.restore_recorder);
 sequence_reset(4);assert(g2h_capture_transition(&ops,&memory,1)==-EIO);assert(!strcmp(sequence_trace,"QIRT")&&!record_active&&memory.restore_recorder);
 sequence_reset(0);assert(!g2h_capture_transition(&ops,&memory,1));assert(!strcmp(sequence_trace,"QIRT")&&record_active&&!memory.restore_recorder);
 sequence_reset(1);assert(g2h_capture_transition(&ops,&memory,0)==-EAGAIN);assert(!strcmp(sequence_trace,"Q")&&record_active);
 sequence_reset(2);assert(g2h_capture_transition(&ops,&memory,0)==-EIO);assert(!strcmp(sequence_trace,"QRS")&&record_active&&memory.restore_recorder);
 sequence_reset(5);assert(g2h_capture_transition(&ops,&memory,0)==-EIO);assert(!strcmp(sequence_trace,"QRSO")&&!record_active&&memory.restore_recorder);
 sequence_reset(0);assert(!g2h_capture_transition(&ops,&memory,1)&&record_active&&!memory.restore_recorder);
}
int main(int argc,char **argv){
    test_power_sequence();test_capture_sequence();
    assert(argc==2);real_preview=fake_preview;fake_arm(false,0);
    unsetenv("G2H_CAPTURE_EXPERIMENT");assert(!start_control(argv[1]));
    assert(cameraPreview(1)==0&&count==1); // Opt-out is actual pass-through.
    assert(!setenv("G2H_CAPTURE_EXPERIMENT","1",1));
    assert(start_control(argv[1]));assert(count==1); // Starting the helper never calls native.
    char socket_path[128];snprintf(socket_path,sizeof(socket_path),"%s/control.sock",argv[1]);
    struct stat st;assert(!lstat(socket_path,&st)&&S_ISSOCK(st.st_mode)&&(st.st_mode&0777)==0600);
    char response[512];command(argv[1],"STATUS\n",response);
    assert(strstr(response,"\"generation\":0")&&strstr(response,"\"blocked\":false")&&strstr(response,"\"rc\":null"));
    char pause_path[128];snprintf(pause_path,sizeof(pause_path),"%s/paused",argv[1]);
    assert(access(pause_path,F_OK));
    // A hung real OFF does not hang requests/status or permit native ON.
    fake_arm(true,0);command(argv[1],"OFF\n",response);fake_wait();
    const long long before=monotonic_ms();command(argv[1],"STATUS\n",response);
    assert(monotonic_ms()-before<500&&strstr(response,"\"pending\":true"));
    assert(!lstat(pause_path,&st)&&S_ISREG(st.st_mode)&&(st.st_mode&0777)==0600);
    assert(cameraPreview(1)==-EPERM);assert(count==2);fake_continue();await_idle(argv[1]);
    command(argv[1],"STATUS\n",response);assert(strstr(response,"\"blocked\":true")&&strstr(response,"\"call_result\":\"request-returned\""));
    // rc5 and rc6 do not claim successful state or open the policy gate.
    fake_arm(false,5);command(argv[1],"ON\n",response);await_idle(argv[1]);command(argv[1],"STATUS\n",response);
    assert(strstr(response,"state-unknown")&&atomic_load(&blocked));assert(cameraPreview(1)==-EPERM);
    fake_arm(false,6);command(argv[1],"ON\n",response);await_idle(argv[1]);command(argv[1],"STATUS\n",response);
    assert(strstr(response,"not-ready")&&atomic_load(&blocked));
    // A successful ON superseded by OFF never clears blocked, even briefly.
    fake_arm(true,0);command(argv[1],"ON\n",response);fake_wait();
    command(argv[1],"OFF\n",response);assert(cameraPreview(1)==-EPERM);fake_continue();await_idle(argv[1]);
    assert(atomic_load(&blocked));assert(trace[count-2]==1&&trace[count-1]==0);
    // Failed OFF remains explicitly blocked; a later successful ON may release.
    fake_arm(false,-EIO);command(argv[1],"OFF\n",response);await_idle(argv[1]);
    command(argv[1],"STATUS\n",response);assert(strstr(response,"\"call_result\":\"error\"")&&atomic_load(&blocked));
    fake_arm(false,0);command(argv[1],"ON\n",response);await_idle(argv[1]);assert(!atomic_load(&blocked));
    assert(access(pause_path,F_OK));
    const unsigned before_native=count;assert(cameraPreview(1)==0&&count==before_native+1);
    const unsigned before_bad=count;command(argv[1],"OFF\nON\n",response);assert(!response[0]);
    command(argv[1],"unsupported\n",response);assert(!response[0]&&count==before_bad);
    command(argv[1],"01234567890123456789\n",response);assert(!response[0]&&count==before_bad);
    // An unsafe pause marker must fail before any native sensor request.
    int bad_marker=open(pause_path,O_CREAT|O_EXCL|O_WRONLY,0644);assert(bad_marker>=0);close(bad_marker);assert(!chmod(pause_path,0644));
    const unsigned before_marker=count;command(argv[1],"OFF\n",response);await_idle(argv[1]);
    assert(count==before_marker&&atomic_load(&blocked));command(argv[1],"STATUS\n",response);assert(strstr(response,"\"call_result\":\"error\""));
    assert(!unlink(pause_path));fake_arm(false,0);command(argv[1],"ON\n",response);await_idle(argv[1]);assert(!atomic_load(&blocked)&&access(pause_path,F_OK));
    atomic_store(&stopping,true);pthread_cond_broadcast(&changed);pthread_join(server_thread,NULL);pthread_join(worker_thread,NULL);
    atomic_store(&active,false);close(listener);close(lock_fd);unlink(socket_path);
    char lock_path[128];snprintf(lock_path,sizeof(lock_path),"%s/lock",argv[1]);unlink(lock_path);
    puts("capture preload tests passed");return 0;
}
