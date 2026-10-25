// Native threads share one serialized guest CPU. AOT backward branches and
// calls yield this gate; blocking native operations release it entirely.
// This keeps ordinary guest loads/stores ordered on ARM and protects registries.
#include "rt.h"
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>

static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned depth, ticks;
void rt_guest_enter(void) {
    if(!depth && pthread_mutex_lock(&gate))abort();
    ++depth;
    rt_guest_poll();
}
void rt_guest_leave(void) {
    if(!depth)abort();
    if(!--depth && pthread_mutex_unlock(&gate))abort();
}
unsigned rt_guest_suspend(void) {
    unsigned saved=depth;depth=0;
    if(saved && pthread_mutex_unlock(&gate))abort();
    return saved;
}
void rt_guest_resume(unsigned saved) {
    if(depth)abort();
    if(saved && pthread_mutex_lock(&gate))abort();
    depth=saved;
}
void rt_guest_poll(void) {
    if(!depth || (++ticks & 1023))return;
    unsigned saved=rt_guest_suspend();
    sched_yield();
    rt_guest_resume(saved);
}
