#ifndef THREADING_H
#define THREADING_H

#include <stdint.h>
#include "julia.h"


#define PROFILE_JL_THREADING            1


// thread ID
extern __thread int16_t ti_tid;
extern __JL_THREAD struct _jl_thread_heap_t *jl_thread_heap;
extern struct _jl_thread_heap_t **jl_all_heaps;
extern jl_gcframe_t ***jl_all_pgcstacks;

extern int jl_n_threads;  // # threads we're actually using

// thread state
enum {
    TI_THREAD_INIT,
    TI_THREAD_WORK
};


// passed to thread function
typedef struct {
    int16_t volatile    state;
    int16_t             tid;
    ti_threadgroup_t    *tg;

} ti_threadarg_t;


// commands to thread function
enum {
    TI_THREADWORK_DONE,
    TI_THREADWORK_RUN
};


// work command to thread function
typedef struct {
    uint8_t             command;
    jl_function_t       *fun;
    jl_tuple_t          *args;
    int                 numargs;
    jl_value_t          *ret;

} ti_threadwork_t;


// basic functions for thread creation
int  ti_threadcreate(uint64_t *pthread_id, int proc_num,
                     void *(*thread_fun)(void *), void *thread_arg);
void ti_threadsetaffinity(uint64_t pthread_id, int proc_num);

// thread function
void *ti_threadfun(void *arg);

// helpers for thread function
void ti_initthread(int16_t tid);
jl_value_t *ti_runthread(jl_function_t *f, jl_tuple_t *args, size_t nargs);


/*
  external interface in julia.h

void jl_threading_init();
void jl_threading_cleanup();
void *jl_threading_prepare_work(jl_function_t *f, jl_tuple_t *args);
void jl_threading_do_work(void *w);
 */


#endif  /* THREADING_H */

