// i386 pthread_t is a guest TCB address; Darwin pthread_t never crosses the ABI.
#include "modules.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct ThreadAttr {
    uint32_t address,size;
    struct ThreadAttr *next;
} ThreadAttr;
typedef struct Thread {
    CPU cpu;
    pthread_t native;
    uint32_t handle,entry,argument,stack,length,result;
    int finished,joining,joined;
    struct Thread *next;
} Thread;
static ThreadAttr *attributes;
static Thread *threads;
static uint32_t next_tid=2,main_handle;
static pthread_t main_native;
static _Thread_local Thread *current;
static uint32_t argument(CPU *c,unsigned index){return R32(c->esp+4+index*4);}
static ThreadAttr *find_attr(uint32_t address){for(ThreadAttr *a=attributes;a;a=a->next)if(a->address==address)return a;return NULL;}
static Thread *find_thread(uint32_t handle){for(Thread *t=threads;t;t=t->next)if(t->handle==handle)return t;return NULL;}
static void *thread_entry(void *opaque) {
    Thread *t=opaque;current=t;
    rt_guest_enter();
    t->result=rt_invoke(&t->cpu,t->entry,&t->argument,1);
    rt_thread_destructors(&t->cpu);
    t->finished=1;
    rt_guest_leave();
    return NULL;
}
int rt_thread_shim(CPU *c,const char *name) {
    if(!c->tid)c->tid=1;
    if(c->tid==1 && !main_handle){main_handle=rt_thread_pointer(c);main_native=pthread_self();}
    uint32_t address=argument(c,0);int error=0;
    if(!strcmp(name,"pthread_self"))c->eax=rt_thread_pointer(c);
    else if(!strcmp(name,"pthread_equal"))c->eax=address==argument(c,1);
    else if(!strcmp(name,"pthread_attr_init")) {
        if(!address || find_attr(address))error=EINVAL;
        else {ThreadAttr *a=calloc(1,sizeof(*a));if(!a)error=ENOMEM;
            else {a->address=address;a->size=8*1024*1024;a->next=attributes;attributes=a;memset(rt_write_span(c,address,36),0,36);}}
        c->eax=error;
    } else if(!strcmp(name,"pthread_attr_setstacksize") || !strcmp(name,"pthread_attr_destroy")) {
        ThreadAttr **link=&attributes;while(*link && (*link)->address!=address)link=&(*link)->next;
        if(!*link)error=EINVAL;
        else if(!strcmp(name,"pthread_attr_destroy")){ThreadAttr *a=*link;*link=a->next;free(a);}
        else {uint32_t size=argument(c,1);if(size<16384 || size>256*1024*1024)error=EINVAL;else (*link)->size=size;}
        c->eax=error;
    } else if(!strcmp(name,"pthread_create")) {
        uint32_t attr_address=argument(c,1);ThreadAttr *attr=find_attr(attr_address);
        if(attr_address && !attr){c->eax=EINVAL;return 1;}
        Thread *t=calloc(1,sizeof(*t));if(!t){c->eax=ENOMEM;return 1;}
        uint32_t size=attr?attr->size:8*1024*1024;size=(size+4095)&~4095u;
        rt_cpu_reset(&t->cpu);t->cpu.tid=next_tid++;
        t->cpu.fcw=c->fcw;t->cpu.mxcsr=c->mxcsr;
        t->length=(size+8192+16383)&~16383u;
        t->stack=rt_mmap(c,0,t->length,3,0x22,-1,0);
        if(t->stack==UINT32_MAX){free(t);c->eax=EAGAIN;return 1;}
        if(rt_mprotect(t->stack,4096,0) || rt_mprotect(t->stack+4096+size,t->length-4096-size,0))rt_trap(c,c->eip,"guest thread stack protection failed");
        t->cpu.esp=t->stack+4096+size;t->handle=rt_thread_pointer(&t->cpu);
        t->entry=argument(c,2);t->argument=argument(c,3);
        pthread_attr_t native_attr;error=pthread_attr_init(&native_attr);
        if(!error){
            error=pthread_attr_setstacksize(&native_attr,16*1024*1024);
            if(!error)error=pthread_create(&t->native,&native_attr,thread_entry,t);
            pthread_attr_destroy(&native_attr);
        }
        if(error){rt_munmap(t->stack,t->length);free(t);}
        else {t->next=threads;threads=t;W32(address,t->handle);}
        c->eax=error==EAGAIN?11:(uint32_t)error;
    } else if(!strcmp(name,"pthread_join")) {
        Thread *t=find_thread(address);uint32_t output=argument(c,1);
        if(!t)error=ESRCH;
        else if(t==current)error=EDEADLK;
        else if(t->joined || t->joining)error=EINVAL;
        else {t->joining=1;unsigned saved=rt_guest_suspend();error=pthread_join(t->native,NULL);rt_guest_resume(saved);
            t->joining=0;
            if(!error){t->joined=1;if(output)W32(output,t->result);rt_munmap(t->stack,t->length);}}
        c->eax=error==EDEADLK?35:(uint32_t)error;
    } else if(!strcmp(name,"pthread_getschedparam") || !strcmp(name,"pthread_setschedparam")) {
        Thread *t=find_thread(address);pthread_t native;
        if(t && !t->joined)native=t->native;
        else if(address==main_handle)native=main_native;
        else {c->eax=ESRCH;return 1;}
        struct sched_param param;int policy;
        if(!strcmp(name,"pthread_getschedparam")){
            error=pthread_getschedparam(native,&policy,&param);
            if(!error){W32(argument(c,1),policy==SCHED_FIFO?1:policy==SCHED_RR?2:0);W32(argument(c,2),(uint32_t)param.sched_priority);}
        } else {
            uint32_t guest_policy=argument(c,1);
            if(guest_policy>2){c->eax=EINVAL;return 1;}
            policy=guest_policy==1?SCHED_FIFO:guest_policy==2?SCHED_RR:SCHED_OTHER;
            memset(&param,0,sizeof(param));param.sched_priority=(int32_t)R32(argument(c,2));
            error=pthread_setschedparam(native,policy,&param);
        }
        c->eax=(uint32_t)error;
    } else if(!strcmp(name,"pthread_exit")) {
        if(!current)rt_trap(c,c->eip,"pthread_exit outside worker thread");
        current->result=address;rt_thread_destructors(c);current->finished=1;
        rt_guest_suspend();pthread_exit(NULL);
    } else return 0;
    return 1;
}
void rt_threads_destroy(void) {
    // Refuse to unmap a CPU or TLS while a native worker can still access it.
    for(Thread *t=threads;t;t=t->next)if(!t->joined){
        if(!t->finished){fputs("cannot destroy runtime with a live guest worker\n",stderr);abort();}
        unsigned saved=rt_guest_suspend();pthread_join(t->native,NULL);rt_guest_resume(saved);
    }
    while(threads){Thread *next=threads->next;free(threads);threads=next;}
    while(attributes){ThreadAttr *next=attributes->next;free(attributes);attributes=next;}
    next_tid=2;main_handle=0;current=NULL;
}
