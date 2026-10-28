/* Experimental process-lifetime LD_PRELOAD helper. Not part of a release.
 * Opt in with G2H_CAPTURE_EXPERIMENT=1. No constructor calls cameraPreview.
 * Also requires G2H_CAPTURE_BACKEND=gc2053-power-experiment.
 * OFF/ON are requested native sensor calls, never observed privacy claims.
 * This does not stop recorders, flush native queues, or provide a restore lease.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#if defined(__arm__) && defined(__linux__) && !defined(G2H_CAPTURE_TESTING)
/* The installed ARM glibc 2.19 exports the versioned stat entry points,
 * not the direct lstat/fstat symbols introduced by newer glibc headers.
 * Version 3 uses the same 32-bit-time struct stat layout used here. */
#include <stddef.h>
_Static_assert(sizeof(struct stat)==88 && offsetof(struct stat,st_mode)==16 &&
               offsetof(struct stat,st_uid)==24,"unexpected native stat ABI");
extern int __lxstat(int,const char *,struct stat *);
extern int __fxstat(int,int,struct stat *);
static int native_lstat(const char *path,struct stat *out){return __lxstat(3,path,out);}
static int native_fstat(int fd,struct stat *out){return __fxstat(3,fd,out);}
#else
#define native_lstat lstat
#define native_fstat fstat
#endif
#ifndef G2H_CAPTURE_TESTING
#include "g2h_sensor_power_backend.h"
#include "g2h_record_task_backend.h"
#endif

typedef int (*preview_fn)(int);
static preview_fn real_preview;
static pthread_once_t resolve_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t calls=PTHREAD_MUTEX_INITIALIZER,state=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static atomic_bool active=false,blocked=false,stopping=false;
static unsigned long generation,completed_generation;
static bool queued,busy,desired_on,have_result;
static int last_rc,listener=-1,lock_fd=-1;
static pthread_t server_thread,worker_thread;
static char control_directory[80];
static int pause_marker(bool pause){
    char path[128];snprintf(path,sizeof(path),"%s/paused",control_directory);
    if(!pause)return !unlink(path)||errno==ENOENT?0:-errno;
    int fd=open(path,O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -errno;
    struct stat st;int rc=native_fstat(fd,&st);
    if(rc||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||st.st_nlink!=1)rc=-EPERM;
    close(fd);return rc;
}

static void resolve_real(void){
    if(!real_preview)real_preview=(preview_fn)dlsym(RTLD_NEXT,"cameraPreview");
#ifndef G2H_CAPTURE_TESTING
    /* The native OMX loader may keep libcamera in a local dlopen scope. Only
     * reuse an already-loaded DSO; never initialize a second camera context. */
    if(!real_preview){
        void *library=dlopen("/lib/libcamera.so",RTLD_NOW|RTLD_NOLOAD);
        if(library){real_preview=(preview_fn)dlsym(library,"cameraPreview");dlclose(library);}
    }
#endif
}
static int real_call(int on){
    pthread_once(&resolve_once,resolve_real);
    return real_preview?real_preview(on):-ENOSYS;
}

/* Keep the native exported ABI. An already running native ON may finish, but
 * the queued OFF follows it under calls; every later native ON is rejected.
 */
int cameraPreview(int on){
    if(!atomic_load(&active))return real_call(on);
    if(on&&atomic_load(&blocked))return -EPERM;
    pthread_mutex_lock(&calls);
    const int rc=on&&atomic_load(&blocked)?-EPERM:real_call(on);
    pthread_mutex_unlock(&calls);
    return rc;
}

static void request(bool on){
    pthread_mutex_lock(&state);
    /* ON never clears this flag; only a successful current worker call can. */
    atomic_store(&blocked,true);
    const int marker_rc=pause_marker(true);
    if(marker_rc){desired_on=false;queued=false;last_rc=marker_rc;have_result=true;completed_generation=++generation;pthread_mutex_unlock(&state);return;}
    desired_on=on;++generation;queued=true;
    pthread_cond_signal(&changed);pthread_mutex_unlock(&state);
}
static void *worker(void *unused){
    (void)unused;
    pthread_mutex_lock(&state);
    while(!atomic_load(&stopping)){
        while(!queued&&!atomic_load(&stopping))pthread_cond_wait(&changed,&state);
        if(atomic_load(&stopping))break;
        const bool on=desired_on;const unsigned long epoch=generation;
        queued=false;busy=true;pthread_mutex_unlock(&state);
        pthread_mutex_lock(&calls);
        #ifdef G2H_CAPTURE_TESTING
        const int rc=real_call(on?1:0);
#else
        const int rc=g2h_capture_native_transition(on?1:0);
#endif
        pthread_mutex_unlock(&calls);
        pthread_mutex_lock(&state);
        last_rc=rc;have_result=true;completed_generation=epoch;busy=false;
        /* rc5 means already-in-that-state, not proof of the actual HAL state.
         * A newer OFF also prevents a late successful ON from reopening. */
        if(on&&rc==0&&generation==epoch&&desired_on){
            const int marker_rc=pause_marker(false);
            if(marker_rc)last_rc=marker_rc;else atomic_store(&blocked,false);
        }
    }
    pthread_mutex_unlock(&state);return NULL;
}
static long long monotonic_ms(void){
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return 0;
    return (long long)now.tv_sec*1000+now.tv_nsec/1000000;
}
static uid_t owner(void){
#ifdef G2H_CAPTURE_TESTING
    return geteuid();
#else
    return 0;
#endif
}
static bool peer_allowed(int fd){
#if defined(__linux__)
    struct ucred peer;socklen_t size=sizeof(peer);
    return !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)&&size==sizeof(peer)&&peer.uid==owner();
#elif defined(__APPLE__)
    uid_t uid;gid_t gid;return !getpeereid(fd,&uid,&gid)&&uid==owner();
#else
    (void)fd;return false;
#endif
}
static void status(char *out,size_t size){
    pthread_mutex_lock(&state);
    const char *result=!have_result?"none":last_rc==0?"request-returned":
        last_rc==5?"state-unknown":last_rc==6?"not-ready":"error";
    char rc[32];if(have_result)snprintf(rc,sizeof(rc),"%d",last_rc);else strcpy(rc,"null");
    snprintf(out,size,"{\"generation\":%lu,\"blocked\":%s,\"pending\":%s,\"completed_generation\":%lu,\"call_result\":\"%s\",\"rc\":%s}\n",
        generation,atomic_load(&blocked)?"true":"false",queued||busy?"true":"false",completed_generation,result,rc);
    pthread_mutex_unlock(&state);
}
struct client {int fd;size_t used,sent,length;long long until;char input[16],output[256];};
static void close_client(struct client *c){if(c->fd>=0)close(c->fd);memset(c,0,sizeof(*c));c->fd=-1;}
static void *serve(void *unused){
    (void)unused;struct client clients[4];for(unsigned i=0;i<4;++i){memset(&clients[i],0,sizeof(clients[i]));clients[i].fd=-1;}
    while(!atomic_load(&stopping)){
        struct pollfd fds[5];fds[0]=(struct pollfd){listener,POLLIN,0};
        for(unsigned i=0;i<4;++i)fds[i+1]=(struct pollfd){clients[i].fd,clients[i].length?POLLOUT:POLLIN,0};
        int ready=poll(fds,5,20);if(ready<0&&errno==EINTR)continue;if(ready<0)break;
        if(fds[0].revents&POLLIN){
            int fd=accept(listener,NULL,NULL);
            if(fd>=0){
                unsigned i;for(i=0;i<4&&clients[i].fd>=0;++i){}
                if(i==4||!peer_allowed(fd)||fcntl(fd,F_SETFL,O_NONBLOCK)||fcntl(fd,F_SETFD,FD_CLOEXEC))close(fd);
                else{clients[i].fd=fd;clients[i].until=monotonic_ms()+500;}
            }
        }
        for(unsigned i=0;i<4;++i){
            struct client *c=&clients[i];if(c->fd<0)continue;
            if(monotonic_ms()>=c->until||(fds[i+1].revents&(POLLERR|POLLHUP|POLLNVAL))){close_client(c);continue;}
            if(!c->length&&(fds[i+1].revents&POLLIN)){
                ssize_t n=recv(c->fd,c->input+c->used,sizeof(c->input)-1-c->used,0);
                if(n<0&&(errno==EINTR||errno==EAGAIN))continue;if(n<=0){close_client(c);continue;}
                c->used+=(size_t)n;c->input[c->used]=0;
                char *end=memchr(c->input,'\n',c->used);
                if(!end){if(c->used==sizeof(c->input)-1)close_client(c);continue;}
                if(end!=c->input+c->used-1){close_client(c);continue;}
                if(!strcmp(c->input,"OFF\n"))request(false);
                else if(!strcmp(c->input,"ON\n"))request(true);
                else if(strcmp(c->input,"STATUS\n")){close_client(c);continue;}
                status(c->output,sizeof(c->output));c->length=strlen(c->output);
            }
            if(c->length&&(fds[i+1].revents&POLLOUT)){
                ssize_t n=send(c->fd,c->output+c->sent,c->length-c->sent,
#ifdef MSG_NOSIGNAL
                    MSG_NOSIGNAL
#else
                    0
#endif
                );
                if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
                if(n<=0){close_client(c);continue;}c->sent+=(size_t)n;if(c->sent==c->length)close_client(c);
            }
        }
    }
    for(unsigned i=0;i<4;++i)close_client(&clients[i]);return NULL;
}
static bool start_control(const char *directory){
    struct stat st;char path[sizeof(((struct sockaddr_un*)0)->sun_path)],lock_path[128];
    const char *enabled=getenv("G2H_CAPTURE_EXPERIMENT");
    if(!enabled||strcmp(enabled,"1")||atomic_load(&active)||geteuid()!=owner()||strlen(directory)>70)return false;
    if(mkdir(directory,0700)&&errno!=EEXIST)return false;
    if(native_lstat(directory,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=owner()||(st.st_mode&077))return false;
    snprintf(lock_path,sizeof(lock_path),"%s/lock",directory);
    lock_fd=open(lock_path,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lock_fd<0||native_fstat(lock_fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=owner()||(st.st_mode&077)||flock(lock_fd,LOCK_EX|LOCK_NB))goto fail;
    snprintf(path,sizeof(path),"%s/control.sock",directory);
    if(!native_lstat(path,&st)){
        if(!S_ISSOCK(st.st_mode)||st.st_uid!=owner()||(st.st_mode&077)||unlink(path))goto fail;
    }else if(errno!=ENOENT)goto fail;
    listener=socket(AF_UNIX,SOCK_STREAM,0);if(listener<0)goto fail;
    if(fcntl(listener,F_SETFL,O_NONBLOCK)||fcntl(listener,F_SETFD,FD_CLOEXEC))goto fail;
#ifdef SO_NOSIGPIPE
    int one=1;if(setsockopt(listener,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one)))goto fail;
#endif
    struct sockaddr_un address;memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;strcpy(address.sun_path,path);
    if(bind(listener,(struct sockaddr*)&address,sizeof(address))||chmod(path,0600)||listen(listener,4))goto fail;
    strcpy(control_directory,directory);
    if(pthread_create(&worker_thread,NULL,worker,NULL))goto fail;
    if(pthread_create(&server_thread,NULL,serve,NULL)){
        atomic_store(&stopping,true);pthread_cond_signal(&changed);pthread_join(worker_thread,NULL);goto fail;
    }
    atomic_store(&active,true);return true;
fail:
    if(listener>=0){close(listener);listener=-1;}if(lock_fd>=0){close(lock_fd);lock_fd=-1;}return false;
}
#ifndef G2H_CAPTURE_TESTING
__attribute__((constructor)) static void capture_constructor(void){
    const char *enabled=getenv("G2H_CAPTURE_EXPERIMENT");
    const char *backend=getenv("G2H_CAPTURE_BACKEND");
    if(enabled&&!strcmp(enabled,"1")&&backend&&!strcmp(backend,"gc2053-power-experiment")){
        if(!start_control("/tmp/g2h-capture-control")){
            char message[96];int n=snprintf(message,sizeof(message),
                "g2h capture experiment: control startup failed (errno=%d)\n",errno);
            if(n>0&&(size_t)n<sizeof(message))(void)write(STDERR_FILENO,message,(size_t)n);
        }
    }
}
#endif
