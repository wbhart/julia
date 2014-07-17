/*
  threading infrastructure
  . thread and threadgroup creation
  . thread function
  . invoke Julia function from multiple threads
*/


#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

#include "julia.h"
#include "julia_internal.h"
#include "uv.h"

#include "ia_misc.h"
#include "threadgroup.h"
#include "threading.h"


// Julia uses this for managing libuv and gc interactions
uint64_t jl_main_thread_id = 0;

// locks for Julia's gc and code generation
// TODO: too coarse-grained
//JL_DEFINE_MUTEX(gc);
JL_DEFINE_MUTEX(codegen);

// exceptions that happen in threads are caught and thrown in the main thread
__JL_THREAD jl_jmp_buf jl_thread_eh;
__JL_THREAD jl_value_t* jl_thread_exception_in_transit;

// thread ID
__JL_THREAD int16_t ti_tid = 0;

// thread heap
__JL_THREAD struct _jl_thread_heap_t *jl_thread_heap;
struct _jl_thread_heap_t **jl_all_heaps;
jl_gcframe_t ***jl_all_pgcstacks;

// only one thread group for now
ti_threadgroup_t *tgworld;

// to let threads go to sleep
uv_mutex_t tgw_alarmlock;
uv_cond_t  tgw_alarm;

// for broadcasting work to threads
// TODO: should be in the thread group?
ti_threadwork_t threadwork;


#if PROFILE_JL_THREADING
uint64_t prep_ticks;
uint64_t fork_ticks[TI_MAX_THREADS];
uint64_t join_ticks[TI_MAX_THREADS];
uint64_t user_ticks[TI_MAX_THREADS];

void jl_threading_profile();
#endif


// create a thread and affinitize it
int ti_threadcreate(uint64_t *pthread_id, int proc_num,
                    void *(*thread_fun)(void *), void *thread_arg)
{
    cpu_set_t cset;
    pthread_attr_t attr;

    CPU_ZERO(&cset);
    CPU_SET(proc_num, &cset);

    pthread_attr_init(&attr);
    pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cset);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    return pthread_create(pthread_id, &attr, thread_fun, thread_arg);
}


// set thread affinity
void ti_threadsetaffinity(uint64_t pthread_id, int proc_num)
{
    cpu_set_t cset;

    CPU_ZERO(&cset);
    CPU_SET(proc_num, &cset);
    pthread_setaffinity_np(pthread_id, sizeof(cpu_set_t), &cset);
}


void *ti_threadfun(void *);

// start threads on the thread function affinitized
void ti_start_threads()
{
    int i;
    uint64_t ptid;
    ti_threadarg_t *targ[TI_MAX_THREADS - 1];

    jl_all_heaps = malloc(TI_MAX_THREADS * sizeof(void*));
    jl_all_pgcstacks = malloc(TI_MAX_THREADS * sizeof(void*));

    // current thread will be tid 1; set tid and affinitize to proc 0
    ti_threadsetaffinity(uv_thread_self(), 0);
    ti_initthread(0);

    // create threads on correct procs
    for (i = 0;  i < TI_MAX_THREADS - 1;  ++i) {
        targ[i] = (ti_threadarg_t *)malloc(sizeof (ti_threadarg_t));
        targ[i]->state = TI_THREAD_INIT;
        targ[i]->tid = i + 1;
        ti_threadcreate(&ptid, i + 1, ti_threadfun, targ[i]);
    }

    // set up the world thread group
    ti_threadgroup_create(TI_MAX_SOCKETS, TI_MAX_CORES,
                          TI_MAX_THREADS_PER_CORE, &tgworld);
    for (i = 0;  i < TI_MAX_THREADS;  ++i)
        ti_threadgroup_addthread(tgworld, i, NULL);
    ti_threadgroup_initthread(tgworld, ti_tid);

    // give the threads the world thread group; they will block waiting for fork
    for (i = 0;  i < TI_MAX_THREADS - 1;  ++i) {
        targ[i]->tg = tgworld;
        cpu_sfence();
        targ[i]->state = TI_THREAD_WORK;
    }
}


// stop the spinning threads by sending them a command
void ti_stop_threads()
{
    ti_threadwork_t *work = &threadwork;

    work->command = TI_THREADWORK_DONE;
    ti_threadgroup_fork(tgworld, ti_tid, (void **)&work);

    sleep(1);

    ti_threadgroup_destroy(tgworld);
}


struct _jl_thread_heap_t *jl_mk_thread_heap(void);

// interface to thread function: set the calling thread's global ID
void ti_initthread(int16_t tid)
{
    ti_tid = tid;
    jl_all_pgcstacks[tid] = &jl_pgcstack;
    jl_all_heaps[tid] = jl_mk_thread_heap();
    jl_thread_heap = jl_all_heaps[tid];
}


// all threads call this function to run user code
jl_value_t *ti_run_fun(jl_function_t *f, jl_tuple_t *args, size_t nargs)
{
    jl_value_t **argrefs = (jl_value_t **)alloca(sizeof (jl_value_t *) * nargs);
    argrefs[0] = jl_box_int16(ti_tid);

    int i;
    for (i = 1;  i < nargs;  i++)
        argrefs[i] = jl_tupleref(args, i);

    // try/catch
    if (!jl_setjmp(jl_thread_eh, 0)) {
        jl_apply(f, argrefs, nargs);
        return jl_nothing;
    }
    else {
        return jl_thread_exception_in_transit;
    }
}


// thread function: used by all except the main thread
void *ti_threadfun(void *arg)
{
    ti_threadarg_t *ta = (ti_threadarg_t *)arg;
    ti_threadgroup_t *tg;
    ti_threadwork_t *work;

    // set the thread-local tid and wait for a thread group
    ti_initthread(ta->tid);
    while (ta->state == TI_THREAD_INIT)
        cpu_pause();
    cpu_lfence();

    // initialize this thread in the thread group
    tg = ta->tg;
    ti_threadgroup_initthread(tg, ti_tid);

    // work loop
    for (; ;) {
#if PROFILE_JL_THREADING
        uint64_t t0 = rdtsc();
#endif

        ti_threadgroup_fork(tg, ti_tid, (void **)&work);

#if PROFILE_JL_THREADING
        uint64_t t1 = rdtsc();
        fork_ticks[ti_tid] += t1 - t0;
#endif

        if (work) {
            if (work->command == TI_THREADWORK_DONE)
                break;
            else if (work->command == TI_THREADWORK_RUN)
                // TODO: return value? reduction?
                ti_run_fun(work->fun, work->args, work->numargs);
        }

        ti_threadgroup_join(tg, ti_tid);

#if PROFILE_JL_THREADING
        join_ticks[ti_tid] += rdtsc() - t1;
#endif

        // TODO:
        // nowait should skip the join, but confirm that fork is reentrant
    }

    return NULL;
}


// interface to Julia; sets up to make the runtime thread-safe
void jl_init_threading()
{
    jl_main_thread_id = uv_thread_self();

    uv_mutex_init(&tgw_alarmlock);
    uv_cond_init(&tgw_alarm);

    ti_start_threads();
}


// interface to Julia; where to call this???
void jl_cleanup_threading()
{
    ti_stop_threads();

    uv_mutex_destroy(&tgw_alarmlock);
    uv_cond_destroy(&tgw_alarm);
}


// return calling thread's ID
int16_t jl_threadid()
{
    return ti_tid;
}


// return thread's thread group
void *jl_threadgroup()
{
    return (void *)tgworld;
}


// utility
void jl_cpu_pause()
{
    cpu_pause();
}


// interface to user code: specialize and compile the user thread function
// and run it in all threads
jl_value_t *jl_threading_run(jl_function_t *f, jl_tuple_t *args)
{
#if PROFILE_JL_THREADING
    uint64_t tstart = rdtsc();
#endif

    size_t nargs = jl_tuple_len(args);
    if (nargs < 1)
        jl_error("wrong number of arguments");

    jl_tuple_t *argtypes = arg_type_tuple(&jl_tupleref(args, 0), nargs);
    jl_function_t *fun = jl_get_specialization(f, argtypes);
    if (fun == NULL)
        fun = f;
    jl_compile(fun);
    jl_generate_fptr(fun);

    threadwork.command = TI_THREADWORK_RUN;
    threadwork.fun = fun;
    threadwork.args = args;
    threadwork.numargs = nargs;
    threadwork.ret = jl_nothing;

#if PROFILE_JL_THREADING
    uint64_t tcompile = rdtsc();
    prep_ticks += (tcompile - tstart);
#endif

    // fork the world thread group
    ti_threadwork_t *tw = (ti_threadwork_t *)&threadwork;
    ti_threadgroup_fork(tgworld, ti_tid, (void **)&tw);

#if PROFILE_JL_THREADING
    uint64_t tfork = rdtsc();
    fork_ticks[ti_tid] += (tfork - tcompile);
#endif

    // this thread must do work too (TODO: reduction?)
    tw->ret = ti_run_fun(fun, args, nargs);

#if PROFILE_JL_THREADING
    uint64_t trun = rdtsc();
    user_ticks[ti_tid] += (trun - tfork);
#endif

    // wait for completion (TODO: nowait?)
    ti_threadgroup_join(tgworld, ti_tid);

#if PROFILE_JL_THREADING
    uint64_t tjoin = rdtsc();
    join_ticks[ti_tid] += (tjoin - trun);
#endif

    return tw->ret;
}


#if PROFILE_JL_THREADING

void ti_timings(uint64_t *times, uint64_t *min, uint64_t *max, uint64_t *avg)
{
    int i;
    *min = UINT64_MAX;
    *max = *avg = 0;
    for (i = 0;  i < TI_MAX_THREADS;  i++) {
        if (times[i] < *min)
            *min = times[i];
        if (times[i] > *max)
            *max = times[i];
        *avg += times[i];
    }
    *avg /= TI_MAX_THREADS;
}

#define TICKS_TO_SECS(t)        (((double)(t)) / (2.7 * 1e9))

void jl_threading_profile()
{
    printf("\nti profiling:\n");
    printf("prep: %g (%lu)\n", TICKS_TO_SECS(prep_ticks), prep_ticks);

    uint64_t min, max, avg;
    ti_timings(fork_ticks, &min, &max, &avg);
    printf("fork: %g (%g - %g)\n", TICKS_TO_SECS(min), TICKS_TO_SECS(max), TICKS_TO_SECS(avg));
    ti_timings(join_ticks, &min, &max, &avg);
    printf("join: %g (%g - %g)\n", TICKS_TO_SECS(min), TICKS_TO_SECS(max), TICKS_TO_SECS(avg));
    ti_timings(user_ticks, &min, &max, &avg);
    printf("user: %g (%g - %g)\n", TICKS_TO_SECS(min), TICKS_TO_SECS(max), TICKS_TO_SECS(avg));
}

#else

void jl_threading_profile()
{
}

#endif

