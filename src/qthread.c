#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <stdlib.h>		       /* for malloc() and abort() */
#ifndef QTHREAD_NO_ASSERTS
# include <assert.h>		       /* for assert() */
#endif
#if defined(HAVE_UCONTEXT_H) && defined(HAVE_NATIVE_MAKECONTEXT)
# include <ucontext.h>		       /* for make/get/swap-context functions */
#else
# include "osx_compat/taskimpl.h"
#endif
#include <stdarg.h>		       /* for va_start and va_end */
#include <qthread/qthread-int.h>       /* for UINT8_MAX */
#include <string.h>		       /* for memset() */
#if !HAVE_MEMCPY
# define memcpy(d, s, n) bcopy((s), (d), (n))
# define memmove(d, s, n) bcopy((s), (d), (n))
#endif
#ifdef NEED_RLIMIT
# include <sys/time.h>
# include <sys/resource.h>
#endif
#if (QTHREAD_SHEPHERD_PROFILING || QTHREAD_LOCK_PROFILING)
# include "qtimer/qtimer.h"
#endif

#ifdef QTHREAD_LOCK_PROFILING
#include <cprops/hashlist.h>
#endif

#include <cprops/mempool.h>
#include <cprops/hashtable.h>
#include <cprops/linked_list.h>

#include "qthread/qthread.h"
#include "qthread/futurelib.h"
#include "qthread_innards.h"
#include "futurelib_innards.h"

/* internal constants */
#define QTHREAD_STATE_NEW               0
#define QTHREAD_STATE_RUNNING           1
#define QTHREAD_STATE_YIELDED           2
#define QTHREAD_STATE_BLOCKED           3
#define QTHREAD_STATE_FEB_BLOCKED       4
#define QTHREAD_STATE_TERMINATED        5
#define QTHREAD_STATE_DONE              6
#define QTHREAD_STATE_TERM_SHEP         UINT8_MAX
/* flags (must be different bits) */
#define QTHREAD_FUTURE                  1
#define QTHREAD_REAL_MCCOY		2

#ifndef QTHREAD_NOALIGNCHECK
#define ALIGN(d, s, f) do { \
    s = (aligned_t *) (((size_t) d) & (~(sizeof(aligned_t)-1))); \
    if (s != d) { \
	fprintf(stderr, \
		"WARNING: " f ": unaligned address %p ... assuming %p\n", \
		(void *) d, (void *) s); \
    } \
} while(0)
#else /* QTHREAD_NOALIGNCHECK */
#define ALIGN(d, s, f) (s)=(d)
#endif

#ifdef DEBUG_DEADLOCK
#define REPORTLOCK(m) printf("%i:%i LOCKED %p's LOCK!\n", qthread_shep(NULL), __LINE__, m)
#define REPORTUNLOCK(m) printf("%i:%i UNLOCKED %p's LOCK!\n", qthread_shep(NULL), __LINE__, m)
#else
#define REPORTLOCK(m)
#define REPORTUNLOCK(m)
#endif

/* internal data structures */
typedef struct qthread_lock_s qthread_lock_t;
typedef struct qthread_shepherd_s qthread_shepherd_t;
typedef struct qthread_queue_s qthread_queue_t;

struct qthread_s
{
    unsigned int thread_id;
    uint8_t thread_state;
    unsigned char flags;

    /* the shepherd we run on */
    qthread_shepherd_t *shepherd_ptr;
    /* the shepherd our memory comes from */
    qthread_shepherd_t *creator_ptr;
    /* a pointer used for passing information back to the shepherd when
     * becoming blocked */
    struct qthread_lock_s *blockedon;

    /* the function to call (that defines this thread) */
    qthread_f f;
    void *arg;			/* user defined data */
    aligned_t *ret;		/* user defined retval location */

    ucontext_t *context;	/* the context switch info */
    void *stack;		/* the thread's stack */
    ucontext_t *return_context;	/* context of parent shepherd */

    struct qthread_s *next;
};

struct qthread_queue_s
{
    qthread_t *head;
    qthread_t *tail;
    qthread_shepherd_t *creator_ptr;
    pthread_mutex_t lock;
    pthread_cond_t notempty;
};

struct qthread_shepherd_s
{
    pthread_t shepherd;
    qthread_shepherd_id_t shepherd_id;	/* whoami */
    qthread_t *current;
    qthread_queue_t *ready;
    cp_mempool *qthread_pool;
    cp_mempool *list_pool;
    cp_mempool *queue_pool;
    cp_mempool *lock_pool;
    cp_mempool *addrres_pool;
    cp_mempool *addrstat_pool;
    cp_mempool *stack_pool;
    cp_mempool *context_pool;
    /* round robin scheduler - can probably be smarter */
    aligned_t sched_shepherd;
#ifdef QTHREAD_SHEPHERD_PROFILING
    double total_time;		/* how much time the shepherd spent running */
    double idle_maxtime;	/* max time the shepherd spent waiting for new threads */
    double idle_time;		/* how much time the shepherd spent waiting for new threads */
    size_t idle_count;		/* how many times the shepherd did a blocking dequeue */
    size_t num_threads;		/* number of threads handled */
#endif
#ifdef QTHREAD_LOCK_PROFILING
    cp_hashlist *uniquelockaddrs;	/* the unique addresses that are locked */
    double aquirelock_maxtime;	/* max time spent aquiring locks */
    double aquirelock_time;	/* total time spent aquiring locks */
    size_t aquirelock_count;	/* num locks aquired */
    double lockwait_maxtime;	/* max time spent blocked on a lock */
    double lockwait_time;	/* total time spent blocked on a lock */
    size_t lockwait_count;	/* num times blocked on a lock */
    double hold_maxtime;	/* max time spent holding locks */
    double hold_time;		/* total time spent holding locks (use aquirelock_count) */

    cp_hashlist *uniquefebaddrs;	/* unique addresses that are associated with febs */
    double febblock_maxtime;	/* max time spent aquiring FEB words */
    double febblock_time;	/* total time spent aquiring FEB words */
    size_t febblock_count;	/* num FEB words aquired */
    double febwait_maxtime;	/* max time spent blocking on FEBs */
    double febwait_time;	/* total time spent blocking on FEBs */
    size_t febwait_count;	/* num FEB blocking waits required */
    double empty_maxtime;	/* max time addresses spent empty */
    double empty_time;		/* total time addresses spent empty */
    size_t empty_count;		/* num times addresses were empty */
#endif
};

struct qthread_lock_s
{
    qthread_queue_t *waiting;
    qthread_shepherd_t *creator_ptr;
#ifdef QTHREAD_DEBUG
    unsigned owner;
#endif
    pthread_mutex_t lock;
#ifdef QTHREAD_LOCK_PROFILING
    qtimer_t hold_timer;
#endif
};

typedef struct qthread_addrres_s
{
    aligned_t *addr;		/* ptr to the memory NOT being blocked on */
    qthread_t *waiter;
    qthread_shepherd_t *creator_ptr;
    struct qthread_addrres_s *next;
} qthread_addrres_t;

typedef struct qthread_addrstat_s
{
    pthread_mutex_t lock;
    qthread_addrres_t *EFQ;
    qthread_addrres_t *FEQ;
    qthread_addrres_t *FFQ;
    qthread_shepherd_t *creator_ptr;
#ifdef QTHREAD_LOCK_PROFILING
    qtimer_t empty_timer;
#endif
    unsigned int full:1;
} qthread_addrstat_t;

pthread_key_t shepherd_structs;

/* shared globals (w/ futurelib) */
qlib_t qlib = NULL;

/* internal globals */
static cp_mempool *generic_qthread_pool = NULL;
static cp_mempool *generic_stack_pool = NULL;
static cp_mempool *generic_context_pool = NULL;
static cp_mempool *generic_queue_pool = NULL;
static cp_mempool *generic_lock_pool = NULL;
static cp_mempool *generic_addrstat_pool = NULL;

#ifdef QTHREAD_COUNT_THREADS
static aligned_t threadcount = 0;
static pthread_mutex_t threadcount_lock = PTHREAD_MUTEX_INITIALIZER;
static aligned_t maxconcurrentthreads = 0;
static aligned_t concurrentthreads = 0;
static pthread_mutex_t concurrentthreads_lock = PTHREAD_MUTEX_INITIALIZER;

#define QTHREAD_COUNT_THREADS_BINCOUNTER(TYPE, BIN) qthread_internal_incr(&qlib->TYPE##_stripes[(BIN)], &qlib->TYPE##_stripes_locks[(BIN)])
#else
#define QTHREAD_COUNT_THREADS_BINCOUNTER(TYPE, BIN) do{ }while(0)
#endif

/* Internal functions */
#ifdef QTHREAD_MAKECONTEXT_SPLIT
static void qthread_wrapper(unsigned int high, unsigned int low);
#else
static void qthread_wrapper(void *ptr);
#endif

static inline void qthread_makecontext(ucontext_t *, void *, size_t,
				       void (*)(void), const void *,
				       ucontext_t *);
static inline qthread_addrstat_t *qthread_addrstat_new(qthread_shepherd_t *
						       shepherd);
static void qthread_addrstat_delete(qthread_addrstat_t * m);
static inline qthread_t *qthread_thread_new(const qthread_f f,
					    const void *arg, aligned_t * ret,
					    const qthread_shepherd_id_t
					    shepherd);
static inline qthread_t *qthread_thread_bare(const qthread_f f,
					     const void *arg, aligned_t * ret,
					     const qthread_shepherd_id_t
					     shepherd);
static inline void qthread_thread_free(qthread_t * t);
static inline qthread_queue_t *qthread_queue_new(qthread_shepherd_t *
						 shepherd);
static inline void qthread_queue_free(qthread_queue_t * q);
static inline void qthread_enqueue(qthread_queue_t * q, qthread_t * t);
static inline qthread_t *qthread_dequeue(qthread_queue_t * q);
static inline qthread_t *qthread_dequeue_nonblocking(qthread_queue_t * q);
static inline void qthread_exec(qthread_t * t, ucontext_t * c);
static inline void qthread_back_to_master(qthread_t * t);
static inline void qthread_gotlock_fill(qthread_addrstat_t * m, void *maddr,
					const char recursive);
static inline void qthread_gotlock_empty(qthread_addrstat_t * m, void *maddr,
					 const char recursive);

#define QTHREAD_INITLOCK(l) do { if (pthread_mutex_init(l, NULL) != 0) { return QTHREAD_PTHREAD_ERROR; } } while(0)
#define QTHREAD_LOCK(l) qassert(pthread_mutex_lock(l), 0)
#define QTHREAD_UNLOCK(l) qassert(pthread_mutex_unlock(l), 0)
#define QTHREAD_DESTROYLOCK(l) qassert(pthread_mutex_destroy(l), 0)
#define QTHREAD_DESTROYCOND(l) qassert(pthread_cond_destroy(l), 0)
#define QTHREAD_SIGNAL(l) qassert(pthread_cond_signal(l), 0)
#define QTHREAD_CONDWAIT(c, l) qassert(pthread_cond_wait(c, l), 0)

#if defined(UNPOOLED_QTHREAD_T) || defined(UNPOOLED)
#define ALLOC_QTHREAD(shep) (qthread_t *) malloc(sizeof(qthread_t))
#define FREE_QTHREAD(t) free(t)
#else
static inline qthread_t *ALLOC_QTHREAD(qthread_shepherd_t * shep)
{
    qthread_t *tmp =
	(qthread_t *) cp_mempool_alloc(shep ? (shep->qthread_pool) :
				       generic_qthread_pool);
    if (tmp != NULL) {
	tmp->creator_ptr = shep;
    }
    return tmp;
}
static inline void FREE_QTHREAD(qthread_t * t)
{
    cp_mempool_free(t->
		    creator_ptr ? (t->creator_ptr->
				   qthread_pool) : generic_qthread_pool, t);
}
#endif

#if defined(UNPOOLED_STACKS) || defined(UNPOOLED)
#define ALLOC_STACK(shep) malloc(qlib->qthread_stack_size)
#define FREE_STACK(shep, t) free(t)
#else
#define ALLOC_STACK(shep) cp_mempool_alloc(shep?(shep->stack_pool):generic_stack_pool)
#define FREE_STACK(shep, t) cp_mempool_free(shep?(shep->stack_pool):generic_stack_pool, t)
#endif

#if defined(UNPOOLED_CONTEXTS) || defined(UNPOOLED)
#define ALLOC_CONTEXT(shep) (ucontext_t *) malloc(sizeof(ucontext_t))
#define FREE_CONTEXT(shep, t) free(t)
#else
#define ALLOC_CONTEXT(shep) (ucontext_t *) cp_mempool_alloc(shep?(shep->context_pool):generic_context_pool)
#define FREE_CONTEXT(shep, t) cp_mempool_free(shep?(shep->context_pool):generic_context_pool, t)
#endif

#if defined(UNPOOLED_QUEUES) || defined(UNPOOLED)
#define ALLOC_QUEUE(shep) (qthread_queue_t *) malloc(sizeof(qthread_queue_t))
#define FREE_QUEUE(t) free(t)
#else
static inline qthread_queue_t *ALLOC_QUEUE(qthread_shepherd_t * shep)
{
    qthread_queue_t *tmp =
	(qthread_queue_t *) cp_mempool_alloc(shep ? (shep->queue_pool) :
					     generic_queue_pool);
    if (tmp != NULL) {
	tmp->creator_ptr = shep;
    }
    return tmp;
}
static inline void FREE_QUEUE(qthread_queue_t * t)
{
    cp_mempool_free(t->
		    creator_ptr ? (t->creator_ptr->
				   queue_pool) : generic_queue_pool, t);
}
#endif

#if defined(UNPOOLED_LOCKS) || defined(UNPOOLED)
#define ALLOC_LOCK(shep) (qthread_lock_t *) malloc(sizeof(qthread_lock_t))
#define FREE_LOCK(t) free(t)
#else
static inline qthread_lock_t *ALLOC_LOCK(qthread_shepherd_t * shep)
{
    qthread_lock_t *tmp =
	(qthread_lock_t *) cp_mempool_alloc(shep ? (shep->lock_pool) :
					    generic_lock_pool);
    if (tmp != NULL) {
	tmp->creator_ptr = shep;
    }
    return tmp;
}
static inline void FREE_LOCK(qthread_lock_t * t)
{
    cp_mempool_free(t->
		    creator_ptr ? (t->creator_ptr->
				   lock_pool) : generic_lock_pool, t);
}
#endif

#if defined(UNPOOLED_ADDRRES) || defined(UNPOOLED)
#define ALLOC_ADDRRES(shep) (qthread_addrres_t *) malloc(sizeof(qthread_addrres_t))
#define FREE_ADDRRES(t) free(t)
#else
static inline qthread_addrres_t *ALLOC_ADDRRES(qthread_shepherd_t * shep)
{
    qthread_addrres_t *tmp =
	(qthread_addrres_t *) cp_mempool_alloc(shep->addrres_pool);
    if (tmp != NULL) {
	tmp->creator_ptr = shep;
    }
    return tmp;
}
static inline void FREE_ADDRRES(qthread_addrres_t * t)
{
    cp_mempool_free(t->creator_ptr->addrres_pool, t);
}
#endif

#if defined(UNPOOLED_ADDRSTAT) || defined(UNPOOLED)
#define ALLOC_ADDRSTAT(shep) (qthread_addrstat_t *) malloc(sizeof(qthread_addrstat_t))
#define FREE_ADDRSTAT(t) free(t)
#else
static inline qthread_addrstat_t *ALLOC_ADDRSTAT(qthread_shepherd_t * shep)
{
    qthread_addrstat_t *tmp =
	(qthread_addrstat_t *) cp_mempool_alloc(shep ? (shep->addrstat_pool) :
						generic_addrstat_pool);
    if (tmp != NULL) {
	tmp->creator_ptr = shep;
    }
    return tmp;
}
static inline void FREE_ADDRSTAT(qthread_addrstat_t * t)
{
    cp_mempool_free(t->
		    creator_ptr ? (t->creator_ptr->
				   addrstat_pool) : generic_addrstat_pool, t);
}
#endif


/* guaranteed to be between 0 and 32, using the first parts of addr that are
 * significant */
#define QTHREAD_CHOOSE_STRIPE(addr) (((size_t)addr >> 4) & 0x1f)
#define QTHREAD_LOCKING_STRIPES 32

#if defined(HAVE_GCC_INLINE_ASSEMBLY)
#define qthread_internal_incr(op,lock) qthread_incr(op, 1)
#else
static inline aligned_t qthread_internal_incr(volatile aligned_t * operand,
					      pthread_mutex_t * lock)
{				       /*{{{ */
    aligned_t retval;

    pthread_mutex_lock(lock);
    retval = ++(*operand);
    pthread_mutex_unlock(lock);
    return retval;
}				       /*}}} */
#endif

static inline aligned_t qthread_internal_incr_mod(volatile aligned_t *
						  operand, const int max,
						  pthread_mutex_t * lock)
{				       /*{{{ */
    aligned_t retval;

#if defined(HAVE_GCC_INLINE_ASSEMBLY)

#if (QTHREAD_ASSEMBLY_ARCH == QTHREAD_POWERPC32) || \
    ((QTHREAD_ASSEMBLY_ARCH == QTHREAD_POWERPC64) && !defined(QTHREAD_64_BIT_ALIGN_T))

    register unsigned int incrd = incrd;	/* these don't need to be initialized */
    register unsigned int compd = compd;	/* they're just tmp variables */

    /* the minus in bne- means "this bne is unlikely to be taken" */
    asm volatile ("1:\n\t"	/* local label */
		  "lwarx  %0,0,%1\n\t"	/* load operand */
		  "addi   %3,%0,1\n\t"	/* increment it into incrd */
		  "cmplw  7,%3,%2\n\t"	/* compare incrd to the max */
		  "mfcr   %4\n\t"	/* move the result into compd */
		  "rlwinm %4,%4,29,1\n\t"	/* isolate the result bit */
		  "mullw  %3,%3,%4\n\t"	/* incrd *= compd */
		  "stwcx. %3,0,%1\n\t"	/* *operand = incrd */
		  "bne-   1b\n\t"	/* if it failed, go to label 1 back */
		  "isync"	/* make sure it wasn't all a dream */
		  /* = means this operand is write-only (previous value is discarded)
		   * & means this operand is an earlyclobber (i.e. cannot use the same register as any of the input operands)
		   * b means this is a register but must not be r0 */
		  :"=&b"   (retval)
		  :"r"     (operand), "r"(max), "r"(incrd), "r"(compd)
		  :"cc", "memory");

#elif (QTHREAD_ASSEMBLY_ARCH == QTHREAD_POWERPC64)

    register uint64_t incrd = incrd;
    register uint64_t compd = compd;
    asm volatile ("1:\n\t"	/* local label */
		  "ldarx  %0,0,%1\n\t"	/* load operand */
		  "addi   %3,%0,1\n\t"	/* increment it into incrd */
		  "cmpl   7,1,%3,%2\n\t"	/* compare incrd to the max */
		  "mfcr   %4\n\t"	/* move the result into compd */
		  "rlwinm %4,%4,29,1\n\t"	/* isolate the result bit */
		  "mulld  %3,%3,%4\n\t"	/* incrd *= compd */
		  "stdcx. %3,0,%1\n\t"	/* *operand = incrd */
		  "bne-   1b\n\t"	/* if it failed, to to label 1 back */
		  "isync"	/* make sure it wasn't all just a dream */
		  :"=&b"   (retval)
		  :"r"     (operand), "r"(max), "r"(incrd), "r"(compd)
		  :"cc", "memory");

#warning "The PPC64 mod assembly needs to be tested. Please remove this warning when its been tested."

#elif (QTHREAD_ASSEMBLY_ARCH == QTHREAD_SPARCV9_32) || \
      ((QTHREAD_ASSEMBLY_ARCH == QTHREAD_SPARCV9_64) && !defined(QTHREAD_64_BIT_ALIGN_T))

    register uint32_t oldval, newval;

    /* newval = *operand; */
    do {
	/* you *should* be able to move the *operand reference outside the
	 * loop and use the output of the CAS (namely, newval) instead.
	 * However, there seems to be a bug in gcc 4.0.4 wherein, if you do
	 * that, the while() comparison uses a temporary register value for
	 * newval that has nothing to do with the output of the CAS
	 * instruction. (See how obviously wrong that is?) For some reason that
	 * I haven't been able to figure out, moving the *operand reference
	 * inside the loop fixes that problem, even at -O2 optimization. */
	retval = oldval = *operand;
	newval = oldval + 1;
	newval *= (newval < max);
	/* if (*operand == oldval)
	 * swap(newval, *operand)
	 * else
	 * newval = *operand
	 */
	__asm__ __volatile__("cas [%1] , %2, %0"	/* */
			     :"=&r"  (newval)
			     :"r"    (operand), "r"(oldval), "0"(newval)
			     :"cc", "memory");
    } while (oldval != newval);

#elif (QTHREAD_ASSEMBLY_ARCH == QTHREAD_SPARCV9_64)

    register aligned_t oldval, newval;

    /* newval = *operand; */
    do {
	/* you *should* be able to move the *operand reference outside the
	 * loop and use the output of the CAS (namely, newval) instead.
	 * However, there seems to be a bug in gcc 4.0.4 wherein, if you do
	 * that, the while() comparison uses a temporary register value for
	 * newval that has nothing to do with the output of the CAS
	 * instruction. (See how obviously wrong that is?) For some reason that
	 * I haven't been able to figure out, moving the *operand reference
	 * inside the loop fixes that problem, even at -O2 optimization. */
	retval = oldval = *operand;
	newval = oldval + 1;
	newval *= (newval < max);
	/* if (*operand == oldval)
	 * swap(newval, *operand)
	 * else
	 * newval = *operand
	 */
	__asm__ __volatile__("casx [%1] , %2, %0":"=&r"(newval)
			     :"r"    (operand), "r"(oldval), "0"(newval)
			     :"cc", "memory");
    } while (oldval != newval);

#elif (QTHREAD_ASSEMBLY_ARCH == QTHREAD_IA64)

# if defined(QTHREAD_64_BIT_ALIGNED_T)

    int64_t res, old, new;

    do {
	old = *operand;		       /* atomic, because operand is aligned */
	new = old + 1;
	new *= (new < max);
	asm volatile ("mov ar.ccv=%0;;":	/* no output */
		      :"rO"    (old));

	/* separate so the compiler can insert its junk */
	asm volatile ("cmpxchg8.acq %0=[%1],%2,ar.ccv":"=r" (res)
		      :"r"     (operand), "r"(new)
		      :"memory");
    } while (res != old);	       /* if res==old, new is out of date */
    retval = old;

# else /* 32-bit aligned_t */

    int32_t res, old, new;

    do {
	old = *operand;		       /* atomic, because operand is aligned */
	new = old + 1;
	new *= (new < max);
	asm volatile ("mov ar.ccv=%0;;":	/* no output */
		      :"rO"    (old));

	/* separate so the compiler can insert its junk */
	asm volatile ("cmpxchg4.acq %0=[%1],%2,ar.ccv":"=r" (res)
		      :"r"     (operand), "r"(new)
		      :"memory");
    } while (res != old);	       /* if res==old, new is out of date */
    retval = old;

# endif

#elif (QTHREAD_ASSEMBLY_ARCH == QTHREAD_IA32) || \
      ((QTHREAD_ASSEMBLY_ARCH == QTHREAD_AMD64) && !defined(QTHREAD_64_BIT_ALIGN_T))

    unsigned int oldval, newval;

    do {
	oldval = *operand;
	newval = oldval + 1;
	newval *= (newval < max);
	asm volatile ("lock; cmpxchgl %1, %2":"=a" (retval)
		      :"r"     (newval), "m"(*operand), "0"(oldval)
		      :"memory");
    } while (retval != oldval);

#elif (QTHREAD_ASSEMBLY_ARCH == QTHREAD_AMD64)

    unsigned long oldval, newval;

    do {
	oldval = *operand;
	newval = oldval + 1;
	newval *= (newval < max);
	asm volatile ("lock; cmpxchgq %1, %2":"=a" (retval)
		      :"r"     (newval), "m"(*operand), "0"(oldval)
		      :"memory");
    } while (retval != oldval);

#else

#error "Unimplemented assembly architecture"

#endif

#elif defined(QTHREAD_MUTEX_INCREMENT)

    pthread_mutex_lock(lock);
    retval = (*operand)++;
    *operand *= (*operand < max);
    pthread_mutex_unlock(lock);

#else

#error "Neither atomic or mutex increment enabled"

#endif

    return retval;
}				       /*}}} */

/*#define QTHREAD_DEBUG 1*/
/* for debugging */
#ifdef QTHREAD_DEBUG
static inline void qthread_debug(char *format, ...)
{				       /*{{{ */
    static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
    va_list args;

    QTHREAD_LOCK(&output_lock);

    fprintf(stderr, "qthread_debug(): ");

    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fflush(stderr);		       /* KBW: helps keep things straight */

    QTHREAD_UNLOCK(&output_lock);
}				       /*}}} */

#define QTHREAD_NONLAZY_THREADIDS
#else
#define qthread_debug(...) do{ }while(0)
#endif

#ifdef QTHREAD_LOCK_PROFILING
# define QTHREAD_ACCUM_MAX(a, b) do { if ((a) < (b)) { a = b; } } while (0)
# define QTHREAD_WAIT_TIMER_DECLARATION qtimer_t wait_timer = qtimer_new();
# define QTHREAD_WAIT_TIMER_START() qtimer_start(wait_timer)
# define QTHREAD_WAIT_TIMER_STOP(ME, TYPE) do { double secs; \
    qtimer_stop(wait_timer); \
    secs = qtimer_secs(wait_timer); \
    if ((ME)->shepherd_ptr->TYPE##_maxtime < secs) { \
	(ME)->shepherd_ptr->TYPE##_maxtime = secs; } \
    (ME)->shepherd_ptr->TYPE##_time += secs; \
    (ME)->shepherd_ptr->TYPE##_count ++; \
    qtimer_free(wait_timer); } while(0)
# define QTHREAD_LOCK_TIMER_DECLARATION(TYPE) qtimer_t TYPE##_timer = qtimer_new();
# define QTHREAD_LOCK_TIMER_START(TYPE) qtimer_start(TYPE##_timer)
# define QTHREAD_LOCK_TIMER_STOP(TYPE, ME) do { double secs; \
    qtimer_stop(TYPE##_timer); \
    secs = qtimer_secs(TYPE##_timer); \
    if ((ME)->shepherd_ptr->TYPE##_maxtime < secs) { \
	(ME)->shepherd_ptr->TYPE##_maxtime = secs; } \
    (ME)->shepherd_ptr->TYPE##_time += qtimer_secs(TYPE##_timer); \
    (ME)->shepherd_ptr->TYPE##_count ++; \
    qtimer_free(TYPE##_timer); } while(0)
# define QTHREAD_HOLD_TIMER_INIT(LOCKSTRUCT_P) (LOCKSTRUCT_P)->hold_timer = qtimer_new()
# define QTHREAD_HOLD_TIMER_START(LOCKSTRUCT_P) qtimer_start((LOCKSTRUCT_P)->hold_timer)
# define QTHREAD_HOLD_TIMER_STOP(LOCKSTRUCT_P, SHEP) do { double secs; \
    qtimer_stop((LOCKSTRUCT_P)->hold_timer); \
    secs = qtimer_secs((LOCKSTRUCT_P)->hold_timer); \
    if ((SHEP)->hold_maxtime < secs) { \
	(SHEP)->hold_maxtime = secs; } \
    (SHEP)->hold_time += secs; } while(0)
# define QTHREAD_HOLD_TIMER_DESTROY(LOCKSTRUCT_P) qtimer_free((LOCKSTRUCT_P)->hold_timer)
# define QTHREAD_EMPTY_TIMER_INIT(LOCKSTRUCT_P) (LOCKSTRUCT_P)->empty_timer = qtimer_new()
# define QTHREAD_EMPTY_TIMER_START(LOCKSTRUCT_P) qtimer_start((LOCKSTRUCT_P)->empty_timer)
# define QTHREAD_EMPTY_TIMER_STOP(LOCKSTRUCT_P) do { qthread_shepherd_t *ret; \
    double secs; \
    qtimer_stop((LOCKSTRUCT_P)->empty_timer); \
    ret = pthread_getspecific(shepherd_structs); \
    assert(ret != NULL); \
    secs = qtimer_secs((LOCKSTRUCT_P)->empty_timer); \
    if (ret->empty_maxtime < secs) { \
	ret->empty_maxtime = secs; } \
    ret->empty_time += secs; \
    ret->empty_count ++; } while (0)
# define QTHREAD_LOCK_UNIQUERECORD(TYPE, ADDR, ME) cp_hashlist_insert((ME)->shepherd_ptr->unique##TYPE##addrs, (void*)(ADDR), (void*)(ADDR))
static inline int qthread_unique_collect(void *key, void *value, void *id)
{
    cp_hashtable_put((cp_hashtable *) id, key, value);
    return 0;
}
#else
# define QTHREAD_WAIT_TIMER_DECLARATION
# define QTHREAD_WAIT_TIMER_START() do{ }while(0)
# define QTHREAD_WAIT_TIMER_STOP(ME, TYPE) do{ }while(0)
# define QTHREAD_LOCK_TIMER_DECLARATION(TYPE)
# define QTHREAD_LOCK_TIMER_START(TYPE) do{ }while(0)
# define QTHREAD_LOCK_TIMER_STOP(TYPE, ME) do{ }while(0)
# define QTHREAD_HOLD_TIMER_INIT(LOCKSTRUCT_P) do{ }while(0)
# define QTHREAD_HOLD_TIMER_START(LOCKSTRUCT_P) do{ }while(0)
# define QTHREAD_HOLD_TIMER_STOP(LOCKSTRUCT_P, SHEP) do{ }while(0)
# define QTHREAD_HOLD_TIMER_DESTROY(LOCKSTRUCT_P) do{ }while(0)
# define QTHREAD_EMPTY_TIMER_INIT(LOCKSTRUCT_P) do{ }while(0)
# define QTHREAD_EMPTY_TIMER_START(LOCKSTRUCT_P) do{ }while(0)
# define QTHREAD_EMPTY_TIMER_STOP(LOCKSTRUCT_P) do{ }while(0)
# define QTHREAD_LOCK_UNIQUERECORD(TYPE, ADDR, ME) do{ }while(0)
#endif

/* the qthread_shepherd() is the pthread responsible for actually
 * executing the work units
 *
 * this function is the workhorse of the library: this is the function that
 * gets spawned several times and runs all the qthreads. */
#ifdef QTHREAD_MAKECONTEXT_SPLIT
static void *qthread_shepherd(void *arg);
static void *qthread_shepherd_wrapper(unsigned int high, unsigned int low)
{				       /*{{{ */
    qthread_shepherd_t *me = (qthread_shepherd_t *) ((((uintptr_t) high) << 32) | low);
    return qthread_shepherd(me);
}
#endif
static void *qthread_shepherd(void *arg)
{
    qthread_shepherd_t *me = (qthread_shepherd_t *) arg;
    ucontext_t my_context;
    qthread_t *t;
    int done = 0;

#ifdef QTHREAD_SHEPHERD_PROFILING
    qtimer_t total = qtimer_new();
    qtimer_t idle = qtimer_new();
#endif

    qthread_debug("qthread_shepherd(%u): forked\n", me->shepherd_id);

    /* Initialize myself */
    pthread_setspecific(shepherd_structs, arg);

    /* workhorse loop */
    while (!done) {
#ifdef QTHREAD_SHEPHERD_PROFILING
	qtimer_start(idle);
#endif
	t = qthread_dequeue(me->ready);
#ifdef QTHREAD_SHEPHERD_PROFILING
	qtimer_stop(idle);
	me->idle_count++;
	me->idle_time += qtimer_secs(idle);
	if (me->idle_maxtime < qtimer_secs(idle)) {
	    me->idle_maxtime = qtimer_secs(idle);
	}
#endif

	qthread_debug
	    ("qthread_shepherd(%u): dequeued thread id %d/state %d\n",
	     me->shepherd_id, t->thread_id, t->thread_state);

	if (t->thread_state == QTHREAD_STATE_TERM_SHEP) {
#ifdef QTHREAD_SHEPHERD_PROFILING
	    qtimer_stop(total);
	    me->total_time = qtimer_secs(total);
#endif
	    done = 1;
	    qthread_thread_free(t);
	} else {
	    assert((t->thread_state == QTHREAD_STATE_NEW) ||
		   (t->thread_state == QTHREAD_STATE_RUNNING));

	    assert(t->f != NULL);
#ifdef QTHREAD_SHEPHERD_PROFILING
	    if (t->thread_state == QTHREAD_STATE_NEW) {
		me->num_threads++;
	    }
#endif

	    assert(t->shepherd_ptr == me);
	    me->current = t;

	    getcontext(&my_context);
	    /* note: there's a good argument that the following should
	     * be: (*t->f)(t), however the state management would be
	     * more complex
	     */
	    qthread_exec(t, &my_context);

	    me->current = NULL;
	    qthread_debug("qthread_shepherd(%u): back from qthread_exec\n",
			  me->shepherd_id);
	    switch (t->thread_state) {
		case QTHREAD_STATE_YIELDED:	/* reschedule it */
		    qthread_debug
			("qthread_shepherd(%u): rescheduling thread %p\n",
			 me->shepherd_id, t);
		    t->thread_state = QTHREAD_STATE_RUNNING;
		    qthread_enqueue(t->shepherd_ptr->ready, t);
		    break;

		case QTHREAD_STATE_FEB_BLOCKED:	/* unlock the related FEB address locks, and re-arrange memory to be correct */
		    qthread_debug
			("qthread_shepherd(%u): unlocking FEB address locks of thread %p\n",
			 me->shepherd_id, t);
		    t->thread_state = QTHREAD_STATE_BLOCKED;
		    QTHREAD_UNLOCK(&
				   (((qthread_addrstat_t *) (t->blockedon))->
				    lock));
		    REPORTUNLOCK(t->blockedon);
		    break;

		case QTHREAD_STATE_BLOCKED:	/* put it in the blocked queue */
		    qthread_debug
			("qthread_shepherd(%u): adding blocked thread %p to blocked queue\n",
			 me->shepherd_id, t);
		    qthread_enqueue((qthread_queue_t *) t->blockedon->waiting,
				    t);
		    QTHREAD_UNLOCK(&(t->blockedon->lock));
		    break;

		case QTHREAD_STATE_TERMINATED:
		    qthread_debug
			("qthread_shepherd(%u): thread %p is in state terminated.\n",
			 me->shepherd_id, t);
		    t->thread_state = QTHREAD_STATE_DONE;
		    /* we can remove the stack and the context... */
		    qthread_thread_free(t);
		    break;
	    }
	}
    }

#ifdef QTHREAD_SHEPHERD_PROFILING
    qtimer_free(total);
    qtimer_free(idle);
#endif
    qthread_debug("qthread_shepherd(%u): finished\n", me->shepherd_id);
    pthread_exit(NULL);
    return NULL;
}				       /*}}} */

int qthread_init(const qthread_shepherd_id_t nshepherds)
{				       /*{{{ */
    int r;
    size_t i;
    ucontext_t *shep0 = NULL;
    void *shepstack = NULL;
    int syncmode = COLLECTION_MODE_PLAIN;

#ifdef NEED_RLIMIT
    struct rlimit rlp;
#endif

    qthread_debug("qthread_init(): began.\n");

#ifdef QTHREAD_USE_PTHREADS
    switch (nshepherds) {
	case 0:
	    return QTHREAD_BADARGS;
	case 1:
	    syncmode |= COLLECTION_MODE_NOSYNC;
    }
#else
    nshepherds = 1;
    syncmode |= COLLECTION_MODE_NOSYNC;
#endif
    qlib = (qlib_t) malloc(sizeof(struct qlib_s));
    if (qlib == NULL) {
	return QTHREAD_MALLOC_ERROR;
    }

    /* initialize the FEB-like locking structures */

    /* this is synchronized with read/write locks by default */
    for (i = 0; i < QTHREAD_LOCKING_STRIPES; i++) {
#ifdef QTHREAD_COUNT_THREADS
	qlib->locks_stripes[i] = 0;
	qlib->febs_stripes[i] = 0;
# ifdef QTHREAD_MUTEX_INCREMENT
	QTHREAD_INITLOCK(&(qlib->locks_stripes_locks[i]));
	QTHREAD_INITLOCK(&(qlib->febs_stripes_locks[i]));
# endif
#endif
	if ((qlib->locks[i] =
	     cp_hashtable_create_by_mode(syncmode, 10000, cp_hash_addr,
					 cp_hash_compare_addr)) == NULL) {
	    return QTHREAD_MALLOC_ERROR;
	}
	cp_hashtable_set_min_fill_factor(qlib->locks[i], 0);
	if ((qlib->FEBs[i] =
	     cp_hashtable_create_by_mode(syncmode, 10000, cp_hash_addr,
					 cp_hash_compare_addr)) == NULL) {
	    return QTHREAD_MALLOC_ERROR;
	}
	cp_hashtable_set_min_fill_factor(qlib->FEBs[i], 0);
    }

    /* initialize the kernel threads and scheduler */
    pthread_key_create(&shepherd_structs, NULL);
    qlib->nshepherds = nshepherds;
    if ((qlib->shepherds = (qthread_shepherd_t *)
	 calloc(nshepherds, sizeof(qthread_shepherd_t))) == NULL) {
	return QTHREAD_MALLOC_ERROR;
    }

    qlib->qthread_stack_size = QTHREAD_DEFAULT_STACK_SIZE;
    qlib->max_thread_id = 0;
    qlib->sched_shepherd = 0;
    QTHREAD_INITLOCK(&qlib->max_thread_id_lock);
    QTHREAD_INITLOCK(&qlib->sched_shepherd_lock);
#ifdef NEED_RLIMIT
    qassert(getrlimit(RLIMIT_STACK, &rlp), 0);
    qthread_debug("stack sizes ... cur: %u max: %u\n", rlp.rlim_cur,
		  rlp.rlim_max);
    qlib->master_stack_size = rlp.rlim_cur;
    qlib->max_stack_size = rlp.rlim_max;
#endif

    /* set up the memory pools */
    for (i = 0; i < nshepherds; i++) {
	/* the following SHOULD only be accessed by one thread at a time, so
	 * should be quite safe unsynchronized. If things fail, though...
	 * resynchronize them and see if that fixes it. */
	qlib->shepherds[i].qthread_pool =
	    cp_mempool_create_by_option(syncmode, sizeof(qthread_t),
					sizeof(qthread_t) * 100);
	qlib->shepherds[i].stack_pool =
	    cp_mempool_create_by_option(syncmode, qlib->qthread_stack_size,
					qlib->qthread_stack_size * 100);
#if ALIGNMENT_PROBLEMS_RETURN
	if (sizeof(ucontext_t) < 2048) {
	    qlib->shepherds[i].context_pool =
		cp_mempool_create_by_option(syncmode, 2048, 2048 * 100);
	} else {
	    qlib->shepherds[i].context_pool =
		cp_mempool_create_by_option(syncmode, sizeof(ucontext_t),
					    sizeof(ucontext_t) * 100);
	}
#else
	qlib->shepherds[i].context_pool =
	    cp_mempool_create_by_option(syncmode, sizeof(ucontext_t),
					sizeof(ucontext_t) * 100);
#endif
	qlib->shepherds[i].list_pool =
	    cp_mempool_create_by_option(syncmode, sizeof(cp_list_entry), 0);
	qlib->shepherds[i].queue_pool =
	    cp_mempool_create_by_option(syncmode, sizeof(qthread_queue_t), 0);
	qlib->shepherds[i].lock_pool =
	    cp_mempool_create_by_option(syncmode, sizeof(qthread_lock_t), 0);
	qlib->shepherds[i].addrres_pool =
	    cp_mempool_create_by_option(syncmode, sizeof(qthread_addrres_t),
					0);
	qlib->shepherds[i].addrstat_pool =
	    cp_mempool_create_by_option(syncmode, sizeof(qthread_addrstat_t),
					0);
    }
    /* these are used when qthread_fork() is called from a non-qthread. */
    generic_qthread_pool =
	cp_mempool_create_by_option(syncmode, sizeof(qthread_t),
				    sizeof(qthread_t) * 100);
    generic_stack_pool =
	cp_mempool_create_by_option(syncmode, qlib->qthread_stack_size, 0);
    generic_context_pool =
	cp_mempool_create_by_option(syncmode, sizeof(ucontext_t),
				    sizeof(ucontext_t) * 100);
    generic_queue_pool =
	cp_mempool_create_by_option(syncmode, sizeof(qthread_queue_t), 0);
    generic_lock_pool =
	cp_mempool_create_by_option(syncmode, sizeof(qthread_lock_t), 0);
    generic_addrstat_pool =
	cp_mempool_create_by_option(syncmode, sizeof(qthread_addrstat_t), 0);

    /* spawn the number of shepherd threads that were specified */
    for (i = 0; i < nshepherds; i++) {
	qlib->shepherds[i].shepherd_id = i;
	if ((qlib->shepherds[i].ready = qthread_queue_new(NULL)) == NULL) {
	    perror("qthread_init creating shepherd queue");
	    return QTHREAD_MALLOC_ERROR;
	}
#ifdef QTHREAD_LOCK_PROFILING
	qlib->shepherds[i].uniquelockaddrs =
	    cp_hashlist_create_by_mode(COLLECTION_MODE_NOSYNC, 100,
				       cp_hash_addr, cp_hash_compare_addr);
	qlib->shepherds[i].uniquefebaddrs =
	    cp_hashlist_create_by_mode(COLLECTION_MODE_NOSYNC, 100,
				       cp_hash_addr, cp_hash_compare_addr);
#endif

	qthread_debug("qthread_init(): forking shepherd thread %p\n",
		      &qlib->shepherds[i]);

	if (i > 0) {
	    if ((r =
		 pthread_create(&qlib->shepherds[i].shepherd, NULL,
				qthread_shepherd,
				&qlib->shepherds[i])) != 0) {
		fprintf(stderr,
			"qthread_init: pthread_create() failed (%d)\n", r);
		perror("qthread_init spawning shepherd");
		return r;
	    }
	}
    }

    /* now, transform the current main context into a qthread,
     * and make the main thread a shepherd (shepherd 0) */
    if ((shep0 = ALLOC_CONTEXT((&qlib->shepherds[0]))) == NULL) {
	perror("qthread_init allocating shepherd context");
	return QTHREAD_MALLOC_ERROR;
    }
    if ((shepstack = malloc(1024 * 1024 * 8)) == NULL) {
	perror("qthread_init allocating shepherd stack");
	return QTHREAD_MALLOC_ERROR;
    }
    {
	qthread_t *t = qthread_thread_new(NULL, NULL, NULL, 0);

	if ((t = ALLOC_QTHREAD((&qlib->shepherds[0]))) == NULL) {
	    perror("qthread_init allocating qthread");
	    return QTHREAD_MALLOC_ERROR;
	}
	if ((t->context = ALLOC_CONTEXT((&qlib->shepherds[0]))) == NULL) {
	    perror("qthread_init allocating context");
	    FREE_QTHREAD(t);
	    return QTHREAD_MALLOC_ERROR;
	}
	if ((t->stack = ALLOC_STACK((&qlib->shepherds[0]))) == NULL) {
	    perror("qthread_init allocating stack");
	    FREE_CONTEXT((&qlib->shepherds[0]), t->context);
	    FREE_QTHREAD(t);
	    return QTHREAD_MALLOC_ERROR;
	}
	t->thread_state = QTHREAD_STATE_YIELDED;
	t->f = NULL;
	t->arg = NULL;
	t->blockedon = NULL;
	t->shepherd_ptr = &qlib->shepherds[0];
	t->ret = NULL;
	t->flags = QTHREAD_REAL_MCCOY;
#ifdef QTHREAD_NONLAZY_THREADIDS
	/* give the thread an ID number */
	t->thread_id =
	    qthread_internal_incr(&(qlib->max_thread_id),
				  &qlib->max_thread_id_lock);
#else
	t->thread_id = (unsigned int)-1;
#endif
	qthread_enqueue(qlib->shepherds[0].ready, t);
	qassert(getcontext(t->context), 0);
	qassert(getcontext(shep0), 0);
	qthread_makecontext(shep0, shepstack, 1024 * 1024 * 8,
#ifdef QTHREAD_MAKECONTEXT_SPLIT
	(void (*)(void))qthread_shepherd_wrapper,
#else
	(void (*)(void))qthread_shepherd,
#endif
			    &(qlib->shepherds[0]), t->context);
	/* this launches shepherd 0 */
	qthread_debug("qthread_init(): launching shepherd 0\n");
	qassert(swapcontext(t->context, shep0), 0);
    }

    qthread_debug("qthread_init(): finished.\n");
    return QTHREAD_SUCCESS;
}				       /*}}} */

/* This initializes a context (c) to run the function (func) with a single
 * argument (arg). This is just a wrapper around makecontext that isolates some
 * of the portability garbage. */
static inline void qthread_makecontext(ucontext_t * c, void *stack,
				       size_t stacksize, void (*func) (void),
				       const void *arg, ucontext_t * returnc)
{
#ifdef QTHREAD_MAKECONTEXT_SPLIT
    const unsigned int high = ((uintptr_t) arg) >> 32;
    const unsigned int low = ((uintptr_t) arg) & 0xffffffff;
#endif

    /* Several other libraries that do this reserve a few words on either end
     * of the stack for some reason. To avoid problems, I'll also do this (even
     * though I have no idea why they do this). */
#ifdef INVERSE_STACK_POINTER
    c->uc_stack.ss_sp = (char *)(stack) + stacksize - 8;
#else
    c->uc_stack.ss_sp = (char *)(stack) + 8;
#endif
    c->uc_stack.ss_size = stacksize - 64;
#ifdef UCSTACK_HAS_SSFLAGS
    c->uc_stack.ss_flags = 0;
#endif
#ifdef HAVE_NATIVE_MAKECONTEXT
    /* the makecontext man page (Linux) says: set the uc_link FIRST.
     * why? no idea */
    c->uc_link = returnc;	       /* NULL pthread_exit() */
#endif
#ifdef QTHREAD_MAKECONTEXT_SPLIT
#ifdef EXTRA_MAKECONTEXT_ARGC
    makecontext(c, func, 3, high, low);
#else
    makecontext(c, func, 2, high, low);
#endif /* EXTRA_MAKECONTEXT_ARGC */
#else /* QTHREAD_MAKECONTEXT_SPLIT */
#ifdef EXTRA_MAKECONTEXT_ARGC
    makecontext(c, func, 2, arg);
#else
    makecontext(c, func, 1, arg);
#endif /* EXTRA_MAKECONTEXT_ARGC */
#endif /* QTHREAD_MAKECONTEXT_SPLIT */
}

void qthread_finalize(void)
{				       /*{{{ */
    int i, r;
    qthread_t *t;

#ifdef QTHREAD_LOCK_PROFILING
    double aquirelock_maxtime = 0.0;
    double aquirelock_time = 0.0;
    size_t aquirelock_count = 0;
    double lockwait_maxtime = 0.0;
    double lockwait_time = 0.0;
    size_t lockwait_count = 0;
    double hold_maxtime = 0.0;
    double hold_time = 0.0;
    double febblock_maxtime = 0.0;
    double febblock_time = 0.0;
    size_t febblock_count = 0;
    double febwait_maxtime = 0.0;
    double febwait_time = 0.0;
    size_t febwait_count = 0;
    double empty_maxtime = 0.0;
    double empty_time = 0.0;
    double empty_count = 0;
    cp_hashtable *uniquelockaddrs =
	cp_hashtable_create_by_mode(COLLECTION_MODE_NOSYNC, 100, cp_hash_addr,
				    cp_hash_compare_addr);
    cp_hashtable *uniquefebaddrs =
	cp_hashtable_create_by_mode(COLLECTION_MODE_NOSYNC, 100, cp_hash_addr,
				    cp_hash_compare_addr);
#endif

    assert(qlib != NULL);

    qthread_debug("qthread_finalize(): began.\n");

    /* rcm - probably need to put a "turn off the library flag" here, but,
     * the programmer can ensure that no further threads are forked for now
     */

    /* enqueue the termination thread sentinal */
    for (i = 0; i < qlib->nshepherds; i++) {
	t = qthread_thread_bare(NULL, NULL, (aligned_t *) NULL, i);
	assert(t != NULL);	       /* what else can we do? */
	t->thread_state = QTHREAD_STATE_TERM_SHEP;
	t->thread_id = (unsigned int)-1;
	qthread_enqueue(qlib->shepherds[i].ready, t);
    }

    /* wait for each SPAWNED shepherd to drain it's queue
     * (note: not shepherd 0, because that one wasn't spawned) */
    for (i = 1; i < qlib->nshepherds; i++) {
	if ((r = pthread_join(qlib->shepherds[i].shepherd, NULL)) != 0) {
	    fprintf(stderr,
		    "qthread_finalize: pthread_join() of shep %i failed (%d)\n",
		    i, r);
	    perror("qthread_finalize");
	    abort();
	}
	qthread_queue_free(qlib->shepherds[i].ready);
#ifdef QTHREAD_SHEPHERD_PROFILING
	printf
	    ("QTHREADS: Shepherd %i spent %f%% of the time idle (%f:%f) handling %lu threads\n",
	     i,
	     qlib->shepherds[i].idle_time / qlib->shepherds[i].total_time *
	     100.0, qlib->shepherds[i].total_time,
	     qlib->shepherds[i].idle_time,
	     (unsigned long)qlib->shepherds[i].num_threads);
	printf
	    ("QTHREADS: Shepherd %i averaged %g secs to find a new thread, max %g secs\n",
	     i, qlib->shepherds[i].idle_time / qlib->shepherds[i].idle_count,
	     qlib->shepherds[i].idle_maxtime);
#endif
#ifdef QTHREAD_LOCK_PROFILING
	QTHREAD_ACCUM_MAX(aquirelock_maxtime,
			  qlib->shepherds[i].aquirelock_maxtime);
	aquirelock_time += qlib->shepherds[i].aquirelock_time;
	aquirelock_count += qlib->shepherds[i].aquirelock_count;
	QTHREAD_ACCUM_MAX(lockwait_maxtime,
			  qlib->shepherds[i].lockwait_maxtime);
	lockwait_time += qlib->shepherds[i].lockwait_time;
	lockwait_count += qlib->shepherds[i].lockwait_count;
	QTHREAD_ACCUM_MAX(hold_maxtime, qlib->shepherds[i].hold_maxtime);
	hold_time += qlib->shepherds[i].hold_time;
	QTHREAD_ACCUM_MAX(febblock_maxtime,
			  qlib->shepherds[i].febblock_maxtime);
	febblock_time += qlib->shepherds[i].febblock_time;
	febblock_count += qlib->shepherds[i].febblock_count;
	QTHREAD_ACCUM_MAX(febwait_maxtime,
			  qlib->shepherds[i].febwait_maxtime);
	febwait_time += qlib->shepherds[i].febwait_time;
	febwait_count += qlib->shepherds[i].febwait_count;
	QTHREAD_ACCUM_MAX(empty_maxtime, qlib->shepherds[i].empty_maxtime);
	empty_time += qlib->shepherds[i].empty_time;
	empty_count += qlib->shepherds[i].empty_count;
	cp_hashlist_callback(qlib->shepherds[i].uniquelockaddrs,
			     qthread_unique_collect, uniquelockaddrs);
	cp_hashlist_callback(qlib->shepherds[i].uniquefebaddrs,
			     qthread_unique_collect, uniquefebaddrs);
	cp_hashlist_destroy(qlib->shepherds[i].uniquelockaddrs);
	cp_hashlist_destroy(qlib->shepherds[i].uniquefebaddrs);
#endif
    }

#ifdef QTHREAD_LOCK_PROFILING
    printf
	("QTHREADS: %llu locks aquired (%ld unique), average %g secs, max %g secs\n",
	 (unsigned long long)aquirelock_count,
	 cp_hashtable_count(uniquelockaddrs),
	 (aquirelock_count == 0) ? 0 : (aquirelock_time / aquirelock_count),
	 aquirelock_maxtime);
    printf
	("QTHREADS: Blocked on a lock %llu times, average %g secs, max %g secs\n",
	 (unsigned long long)lockwait_count,
	 (lockwait_count == 0) ? 0 : (lockwait_time / lockwait_count),
	 lockwait_maxtime);
    printf("QTHREADS: Locks held an average of %g seconds, max %g seconds\n",
	   (aquirelock_count == 0) ? 0 : (hold_time / aquirelock_count),
	   hold_maxtime);
    printf("QTHREADS: %ld unique addresses used with FEB\n",
	   cp_hashtable_count(uniquefebaddrs));
    printf
	("QTHREADS: %llu potentially-blocking FEB operations, average %g secs, max %g secs\n",
	 (unsigned long long)febblock_count,
	 (febblock_count == 0) ? 0 : (febblock_time / febblock_count),
	 febblock_maxtime);
    printf
	("QTHREADS: %llu FEB operations blocked, average wait %g secs, max %g secs\n",
	 (unsigned long long)febwait_count,
	 (febwait_count == 0) ? 0 : (febwait_time / febwait_count),
	 febwait_maxtime);
    printf
	("QTHREADS: %llu FEB bits emptied, stayed empty average %g secs, max %g secs\n",
	 (unsigned long long)empty_count,
	 (empty_count == 0) ? 0 : (empty_time / empty_count), empty_maxtime);
#endif

    for (i = 0; i < QTHREAD_LOCKING_STRIPES; i++) {
	cp_hashtable_destroy(qlib->locks[i]);
	cp_hashtable_destroy_custom(qlib->FEBs[i], NULL, (cp_destructor_fn)
				    qthread_addrstat_delete);
#ifdef QTHREAD_COUNT_THREADS
	printf("QTHREADS: bin %i used %i/%i times\n", i,
	       qlib->locks_stripes[i], qlib->febs_stripes[i]);
# ifdef QTHREAD_MUTEX_INCREMENT
	QTHREAD_DESTROYLOCK(&qlib->locks_stripes_locks[i]);
	QTHREAD_DESTROYLOCK(&qlib->febs_stripes_locks[i]);
# endif
#endif
    }

#ifdef QTHREAD_COUNT_THREADS
    printf("spawned %lu threads, max concurrency %lu\n",
	   (unsigned long)threadcount, (unsigned long)maxconcurrentthreads);
    QTHREAD_DESTROYLOCK(&threadcount_lock);
    QTHREAD_DESTROYLOCK(&concurrentthreads_lock);
#endif

    QTHREAD_DESTROYLOCK(&qlib->max_thread_id_lock);
    QTHREAD_DESTROYLOCK(&qlib->sched_shepherd_lock);

    for (i = 0; i < qlib->nshepherds; ++i) {
	cp_mempool_destroy(qlib->shepherds[i].qthread_pool);
	cp_mempool_destroy(qlib->shepherds[i].list_pool);
	cp_mempool_destroy(qlib->shepherds[i].queue_pool);
	cp_mempool_destroy(qlib->shepherds[i].lock_pool);
	cp_mempool_destroy(qlib->shepherds[i].addrres_pool);
	cp_mempool_destroy(qlib->shepherds[i].addrstat_pool);
	cp_mempool_destroy(qlib->shepherds[i].stack_pool);
	cp_mempool_destroy(qlib->shepherds[i].context_pool);
    }
    cp_mempool_destroy(generic_qthread_pool);
    cp_mempool_destroy(generic_stack_pool);
    cp_mempool_destroy(generic_context_pool);
    cp_mempool_destroy(generic_queue_pool);
    cp_mempool_destroy(generic_lock_pool);
    cp_mempool_destroy(generic_addrstat_pool);
    free(qlib->shepherds);
    free(qlib);
    qlib = NULL;

    qthread_debug("qthread_finalize(): finished.\n");
}				       /*}}} */

qthread_t *qthread_self(void)
{				       /*{{{ */
    qthread_shepherd_t *shep;

#if 0
    /* size_t mask; */

    printf("stack size: %lu\n", qlib->qthread_stack_size);
    printf("ret is at %p\n", &ret);
    mask = qlib->qthread_stack_size - 1;	/* assuming the stack is a power of two */
    printf("mask is: %p\n", ((size_t) (qlib->qthread_stack_size) - 1));
    printf("low order bits: 0x%lx\n",
	   ((size_t) (&ret) % (size_t) (qlib->qthread_stack_size)));
    printf("low order bits: 0x%lx\n", (size_t) (&ret) & mask);
    printf("calc stack pointer is: %p\n", ((size_t) (&ret) & ~mask));
    printf("top is then: 0x%lx\n",
	   ((size_t) (&ret) & ~mask) + qlib->qthread_stack_size);
    /* printf("stack pointer should be %p\n", t->stack); */
#endif
    shep = (qthread_shepherd_t *) pthread_getspecific(shepherd_structs);
    return shep ? shep->current : NULL;
}				       /*}}} */

size_t qthread_stackleft(const qthread_t * t)
{				       /*{{{ */
    if (t != NULL) {
	return (size_t) (&t) - (size_t) (t->stack);
    } else {
	return 0;
    }
}				       /*}}} */

aligned_t *qthread_retlock(const qthread_t * t)
{				       /*{{{ */
    if (t) {
	return t->ret;
    } else {
	qthread_t *me = qthread_self();

	if (me) {
	    return me->ret;
	} else {
	    return NULL;
	}
    }
}				       /*}}} */

/************************************************************/
/* functions to manage thread stack allocation/deallocation */
/************************************************************/

static inline qthread_t *qthread_thread_bare(const qthread_f f,
					     const void *arg, aligned_t * ret,
					     const qthread_shepherd_id_t
					     shepherd)
{				       /*{{{ */
    qthread_t *t;

#ifndef UNPOOLED
    t = ALLOC_QTHREAD((&(qlib->shepherds[shepherd])));
#else
    t = ALLOC_QTHREAD(NULL);
#endif
    if (t != NULL) {
#ifdef QTHREAD_NONLAZY_THREADIDS
	/* give the thread an ID number */
	t->thread_id =
	    qthread_internal_incr(&(qlib->max_thread_id),
				  &qlib->max_thread_id_lock);
#else
	t->thread_id = (unsigned int)-1;
#endif
	t->thread_state = QTHREAD_STATE_NEW;
	t->f = f;
	t->arg = (void *)arg;
	t->blockedon = NULL;
	t->shepherd_ptr = &(qlib->shepherds[shepherd]);
	t->ret = ret;
	t->context = NULL;
	t->stack = NULL;
    }
    return t;
}				       /*}}} */

static inline int qthread_thread_plush(qthread_t * t)
{				       /*{{{ */
    ucontext_t *uc;
    void *stack;
    qthread_shepherd_t *shepherd =
	(qthread_shepherd_t *) pthread_getspecific(shepherd_structs);

    uc = ALLOC_CONTEXT(shepherd);
    if (uc != NULL) {
	stack = ALLOC_STACK(shepherd);
	if (stack != NULL) {
	    t->context = uc;
	    t->stack = stack;
	    return QTHREAD_SUCCESS;
	}
	FREE_CONTEXT(shepherd, uc);
    }
    return QTHREAD_MALLOC_ERROR;
}				       /*}}} */

/* this could be reduced to a qthread_thread_bare() and qthread_thread_plush(),
 * but I *think* doing it this way makes it faster. maybe not, I haven't tested
 * it. */
static inline qthread_t *qthread_thread_new(const qthread_f f,
					    const void *arg, aligned_t * ret,
					    const qthread_shepherd_id_t
					    shepherd)
{				       /*{{{ */
    qthread_t *t;
    ucontext_t *uc;
    void *stack;

#ifndef UNPOOLED
    qthread_shepherd_t *myshep = &(qlib->shepherds[shepherd]);
#else
    qthread_shepherd_t *myshep = NULL;
#endif

    t = ALLOC_QTHREAD(myshep);
    if (t == NULL) {
	return NULL;
    }
    uc = ALLOC_CONTEXT(myshep);
    if (uc == NULL) {
	FREE_QTHREAD(t);
	return NULL;
    }
    stack = ALLOC_STACK(myshep);
    if (stack == NULL) {
	FREE_QTHREAD(t);
	FREE_CONTEXT(myshep, uc);
	return NULL;
    }

    t->thread_state = QTHREAD_STATE_NEW;
    t->f = f;
    t->arg = (void *)arg;
    t->blockedon = NULL;
    t->shepherd_ptr = &(qlib->shepherds[shepherd]);
    t->ret = ret;
    t->flags = 0;
    t->context = uc;
    t->stack = stack;

#ifdef QTHREAD_NONLAZY_THREADIDS
    /* give the thread an ID number */
    t->thread_id =
	qthread_internal_incr(&(qlib->max_thread_id),
			      &qlib->max_thread_id_lock);
#else
    t->thread_id = (unsigned int)-1;
#endif

    return t;
}				       /*}}} */

static inline void qthread_thread_free(qthread_t * t)
{				       /*{{{ */
    assert(t != NULL);

    if (t->context) {
	FREE_CONTEXT(t->creator_ptr, t->context);
    }
    if (t->stack != NULL) {
	FREE_STACK(t->creator_ptr, t->stack);
    }
    FREE_QTHREAD(t);
}				       /*}}} */


/*****************************************/
/* functions to manage the thread queues */
/*****************************************/

static inline qthread_queue_t *qthread_queue_new(qthread_shepherd_t *
						 shepherd)
{				       /*{{{ */
    qthread_queue_t *q;

    q = ALLOC_QUEUE(shepherd);
    if (q != NULL) {
	q->head = NULL;
	q->tail = NULL;
	if (pthread_mutex_init(&q->lock, NULL) != 0) {
	    FREE_QUEUE(q);
	    return NULL;
	}
	if (pthread_cond_init(&q->notempty, NULL) != 0) {
	    QTHREAD_DESTROYLOCK(&q->lock);
	    FREE_QUEUE(q);
	    return NULL;
	}
    }
    return q;
}				       /*}}} */

static inline void qthread_queue_free(qthread_queue_t * q)
{				       /*{{{ */
    assert((q->head == NULL) && (q->tail == NULL));
    QTHREAD_DESTROYLOCK(&q->lock);
    QTHREAD_DESTROYCOND(&q->notempty);
    FREE_QUEUE(q);
}				       /*}}} */

static inline void qthread_enqueue(qthread_queue_t * q, qthread_t * t)
{				       /*{{{ */
    assert(t != NULL);
    assert(q != NULL);

    qthread_debug("qthread_enqueue(%p,%p): started\n", q, t);

    QTHREAD_LOCK(&q->lock);

    t->next = NULL;

    if (q->head == NULL) {	       /* surely then tail is also null; no need to check */
	q->head = t;
	q->tail = t;
	QTHREAD_SIGNAL(&q->notempty);
    } else {
	q->tail->next = t;
	q->tail = t;
    }

    qthread_debug("qthread_enqueue(%p,%p): finished\n", q, t);
    QTHREAD_UNLOCK(&q->lock);
}				       /*}}} */

static inline qthread_t *qthread_dequeue(qthread_queue_t * q)
{				       /*{{{ */
    qthread_t *t;

    qthread_debug("qthread_dequeue(%p): started\n", q);

    QTHREAD_LOCK(&q->lock);

    while (q->head == NULL) {	       /* if head is null, then surely tail is also null */
	QTHREAD_CONDWAIT(&q->notempty, &q->lock);
    }

    assert(q->head != NULL);

    t = q->head;
    if (q->head != q->tail) {
	q->head = q->head->next;
    } else {
	q->head = NULL;
	q->tail = NULL;
    }
    t->next = NULL;

    QTHREAD_UNLOCK(&q->lock);

    qthread_debug("qthread_dequeue(%p,%p): finished\n", q, t);
    return (t);
}				       /*}}} */

static inline qthread_t *qthread_dequeue_nonblocking(qthread_queue_t * q)
{				       /*{{{ */
    qthread_t *t = NULL;

    /* NOTE: it's up to the caller to lock/unlock the queue! */
    qthread_debug("qthread_dequeue_nonblocking(%p,%p): started\n", q, t);

    if (q->head == NULL) {
	qthread_debug
	    ("qthread_dequeue_nonblocking(%p,%p): finished (nobody in list)\n",
	     q, t);
	return (NULL);
    }

    t = q->head;
    if (q->head != q->tail) {
	q->head = q->head->next;
    } else {
	q->head = NULL;
	q->tail = NULL;
    }
    t->next = NULL;

    qthread_debug("qthread_dequeue_nonblocking(%p,%p): finished\n", q, t);
    return (t);
}				       /*}}} */

/* this function is for maintenance of the FEB hashtables. SHOULD only be
 * necessary for things left over when qthread_finalize is called */
static void qthread_addrstat_delete(qthread_addrstat_t * m)
{				       /*{{{ */
#ifdef QTHREAD_LOCK_PROFILING
    qtimer_free(m->empty_timer);
#endif
    QTHREAD_DESTROYLOCK(&m->lock);
    FREE_ADDRSTAT(m);
}				       /*}}} */

/* this function runs a thread until it completes or yields */
#ifdef QTHREAD_MAKECONTEXT_SPLIT
static void qthread_wrapper(unsigned int high, unsigned int low)
{				       /*{{{ */
    qthread_t *t = (qthread_t *) ((((uintptr_t) high) << 32) | low);
#else
static void qthread_wrapper(void *ptr)
{
    qthread_t *t = (qthread_t *) ptr;
#endif

    qthread_debug("qthread_wrapper(): executing f=%p arg=%p.\n", t->f,
		  t->arg);
#ifdef QTHREAD_COUNT_THREADS
    qthread_internal_incr(&threadcount, &threadcount_lock);
    qassert(pthread_mutex_lock(&concurrentthreads_lock), 0);
    concurrentthreads++;
    if (concurrentthreads > maxconcurrentthreads)
	maxconcurrentthreads = concurrentthreads;
    qassert(pthread_mutex_unlock(&concurrentthreads_lock), 0);
#endif
    if (t->ret) {
	/* XXX: if this fails, we should probably do something */
	qthread_writeEF_const(t, t->ret, (t->f) (t, t->arg));
    } else {
	(t->f) (t, t->arg);
    }
    t->thread_state = QTHREAD_STATE_TERMINATED;

    qthread_debug("qthread_wrapper(): f=%p arg=%p completed.\n", t->f,
		  t->arg);
#ifdef QTHREAD_COUNT_THREADS
    qassert(pthread_mutex_lock(&concurrentthreads_lock), 0);
    concurrentthreads--;
    qassert(pthread_mutex_unlock(&concurrentthreads_lock), 0);
#endif
    if (t->flags & QTHREAD_FUTURE) {
	future_exit(t);
    }
#if !defined(HAVE_NATIVE_MAKECONTEXT) || defined(NEED_RLIMIT)
    /* without a built-in make/get/swapcontext, we're relying on the portable
     * one in context.c (stolen from libtask). unfortunately, this home-made
     * context stuff does not allow us to set up a uc_link pointer that will be
     * returned to once qthread_wrapper returns, so we have to do it by hand.
     *
     * We also have to do it by hand if the context switch requires a
     * stack-size modification.
     */
    qthread_back_to_master(t);
#endif
}				       /*}}} */

/* This function means "run thread t". The second argument (c) is a pointer
 * to the current context. */
static inline void qthread_exec(qthread_t * t, ucontext_t * c)
{				       /*{{{ */
#ifdef NEED_RLIMIT
    struct rlimit rlp;
#endif

    assert(t != NULL);
    assert(c != NULL);

    if (t->thread_state == QTHREAD_STATE_NEW) {

	qthread_debug("qthread_exec(%p, %p): type is QTHREAD_THREAD_NEW!\n",
		      t, c);
	t->thread_state = QTHREAD_STATE_RUNNING;

	qassert(getcontext(t->context), 0);	/* puts the current context into t->context */
	qthread_makecontext(t->context, t->stack, qlib->qthread_stack_size,
			    (void (*)(void))qthread_wrapper, t, c);
#ifdef HAVE_NATIVE_MAKECONTEXT
    } else {
	t->context->uc_link = c;       /* NULL pthread_exit() */
#endif
    }

    t->return_context = c;

#ifdef NEED_RLIMIT
    qthread_debug
	("qthread_exec(%p): setting stack size limits... hopefully we don't currently exceed them!\n",
	 t);
    rlp.rlim_cur = qlib->qthread_stack_size;
    rlp.rlim_max = qlib->max_stack_size;
    qassert(setrlimit(RLIMIT_STACK, &rlp), 0);
#endif

    qthread_debug("qthread_exec(%p): executing swapcontext()...\n", t);
    /* return_context (aka "c") is being written over with the current context */
    qassert(swapcontext(t->return_context, t->context), 0);
#ifdef NEED_RLIMIT
    qthread_debug
	("qthread_exec(%p): setting stack size limits back to normal...\n",
	 t);
    rlp.rlim_cur = qlib->master_stack_size;
    qassert(setrlimit(RLIMIT_STACK, &rlp), 0);
#endif

    assert(t != NULL);
    assert(c != NULL);

    qthread_debug("qthread_exec(%p): finished\n", t);
}				       /*}}} */

/* this function yields thread t to the master kernel thread */
void qthread_yield(qthread_t * t)
{				       /*{{{ */
    if (t == NULL) {
	t = qthread_self();
    }
    if (t != NULL) {
	qthread_debug("qthread_yield(): thread %p yielding.\n", t);
	t->thread_state = QTHREAD_STATE_YIELDED;
	qthread_back_to_master(t);
	qthread_debug("qthread_yield(): thread %p resumed.\n", t);
    }
}				       /*}}} */

/***********************************************
 * FORKING                                     *
 ***********************************************/
/* fork a thread by putting it in somebody's work queue
 * NOTE: scheduling happens here
 */
int qthread_fork(const qthread_f f, const void *arg, aligned_t * ret)
{				       /*{{{ */
    qthread_t *t;
    qthread_shepherd_id_t shep;
    qthread_shepherd_t *myshep =
	(qthread_shepherd_t *) pthread_getspecific(shepherd_structs);

    if (myshep) {		       /* note: for forking from a qthread, NO LOCKS! */
	shep = myshep->sched_shepherd++;
	if (myshep->sched_shepherd == qlib->nshepherds) {
	    myshep->sched_shepherd = 0;
	}
    } else {
	shep =
	    qthread_internal_incr_mod(&qlib->sched_shepherd, qlib->nshepherds,
				      &qlib->sched_shepherd_lock);
	assert(shep < qlib->nshepherds);
    }
    t = qthread_thread_new(f, arg, ret, shep);
    if (t) {
	qthread_debug("qthread_fork(): tid %u shep %u\n", t->thread_id, shep);

	if (ret) {
	    int test = qthread_empty(qthread_self(), ret);

	    if (test != QTHREAD_SUCCESS) {
		qthread_thread_free(t);
		return test;
	    }
	}
	qthread_enqueue(qlib->shepherds[shep].ready, t);
	return QTHREAD_SUCCESS;
    }
    return QTHREAD_MALLOC_ERROR;
}				       /*}}} */

int qthread_fork_to(const qthread_f f, const void *arg, aligned_t * ret,
		    const qthread_shepherd_id_t shepherd)
{				       /*{{{ */
    qthread_t *t;

    if (shepherd > qlib->nshepherds || f == NULL) {
	return QTHREAD_BADARGS;
    }
    t = qthread_thread_new(f, arg, ret, shepherd);
    if (t) {
	qthread_debug("qthread_fork_to(): tid %u shep %u\n", t->thread_id,
		      shepherd);

	if (ret) {
	    int test = qthread_empty(qthread_self(), ret);

	    if (test != QTHREAD_SUCCESS) {
		qthread_thread_free(t);
		return test;
	    }
	}
	qthread_enqueue(qlib->shepherds[shepherd].ready, t);
	return QTHREAD_SUCCESS;
    }
    return QTHREAD_MALLOC_ERROR;
}				       /*}}} */

int qthread_fork_future_to(const qthread_f f, const void *arg,
			   aligned_t * ret,
			   const qthread_shepherd_id_t shepherd)
{				       /*{{{ */
    qthread_t *t;

    if (shepherd > qlib->nshepherds) {
	return QTHREAD_BADARGS;
    }
    t = qthread_thread_new(f, arg, ret, shepherd);
    if (t) {
	t->flags |= QTHREAD_FUTURE;
	qthread_debug("qthread_fork_future_to(): tid %u shep %u\n",
		      t->thread_id, shepherd);

	if (ret) {
	    int test = qthread_empty(qthread_self(), ret);

	    if (test != QTHREAD_SUCCESS) {
		qthread_thread_free(t);
		return test;
	    }
	}
	qthread_enqueue(qlib->shepherds[shepherd].ready, t);
	return QTHREAD_SUCCESS;
    }
    return QTHREAD_MALLOC_ERROR;
}				       /*}}} */

static inline void qthread_back_to_master(qthread_t * t)
{				       /*{{{ */
#ifdef NEED_RLIMIT
    struct rlimit rlp;

    qthread_debug
	("qthread_back_to_master(%p): setting stack size limits for master thread...\n",
	 t);
    rlp.rlim_cur = qlib->master_stack_size;
    rlp.rlim_max = qlib->max_stack_size;
    qassert(setrlimit(RLIMIT_STACK, &rlp), 0);
#endif
    /* now back to your regularly scheduled master thread */
    qassert(swapcontext(t->context, t->return_context), 0);
#ifdef NEED_RLIMIT
    qthread_debug
	("qthread_back_to_master(%p): setting stack size limits back to qthread size...\n",
	 t);
    rlp.rlim_cur = qlib->qthread_stack_size;
    qassert(setrlimit(RLIMIT_STACK, &rlp), 0);
#endif
}				       /*}}} */

qthread_t *qthread_prepare(const qthread_f f, const void *arg,
			   aligned_t * ret)
{				       /*{{{ */
    qthread_t *t;
    qthread_shepherd_id_t shep;
    qthread_shepherd_t *myshep =
	(qthread_shepherd_t *) pthread_getspecific(shepherd_structs);

    if (myshep) {
	shep = myshep->sched_shepherd++;
	if (myshep->sched_shepherd == qlib->nshepherds) {
	    myshep->sched_shepherd = 0;
	}
    } else {
	shep =
	    qthread_internal_incr_mod(&qlib->sched_shepherd, qlib->nshepherds,
				      &qlib->sched_shepherd_lock);
	assert(shep < qlib->nshepherds);
    }

    t = qthread_thread_bare(f, arg, ret, shep);
    if (t && ret) {
	if (qthread_empty(qthread_self(), ret) != QTHREAD_SUCCESS) {
	    qthread_thread_free(t);
	    return NULL;
	}
    }
    return t;
}				       /*}}} */

qthread_t *qthread_prepare_for(const qthread_f f, const void *arg,
			       aligned_t * ret,
			       const qthread_shepherd_id_t shepherd)
{				       /*{{{ */
    qthread_t *t = qthread_thread_bare(f, arg, ret, shepherd);

    if (t && ret) {
	if (qthread_empty(qthread_self(), ret) != QTHREAD_SUCCESS) {
	    qthread_thread_free(t);
	    return NULL;
	}
    }
    return t;
}				       /*}}} */

int qthread_schedule(qthread_t * t)
{				       /*{{{ */
    int ret = qthread_thread_plush(t);

    if (ret == QTHREAD_SUCCESS) {
	qthread_enqueue(t->shepherd_ptr->ready, t);
    }
    return ret;
}				       /*}}} */

int qthread_schedule_on(qthread_t * t, const qthread_shepherd_id_t shepherd)
{				       /*{{{ */
    int ret = qthread_thread_plush(t);

    if (ret == QTHREAD_SUCCESS) {
	qthread_enqueue(qlib->shepherds[shepherd].ready, t);
    }
    return ret;
}				       /*}}} */

/* functions to implement FEB locking/unlocking */

/* The lock ordering in these functions is very particular, and is designed to
 * reduce the impact of having only one hashtable. Don't monkey with it unless
 * you REALLY know what you're doing! If one hashtable becomes a problem, we
 * may need to move to a new mechanism.
 */

/* This is just a little function that should help in debugging */
int qthread_feb_status(const void *addr)
{				       /*{{{ */
    qthread_addrstat_t *m;
    aligned_t *alignedaddr;
    int status = 1;		/* full */
    const int lockbin = QTHREAD_CHOOSE_STRIPE(addr);

    ALIGN(addr, alignedaddr, "qthread_feb_status()");
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
    cp_hashtable_rdlock(qlib->FEBs[lockbin]); {
	m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs[lockbin],
						    (void *)alignedaddr);
	if (m) {
	    QTHREAD_LOCK(&m->lock);
	    REPORTLOCK(m);
	    status = m->full;
	    QTHREAD_UNLOCK(&m->lock);
	    REPORTUNLOCK(m);
	}
    }
    cp_hashtable_unlock(qlib->FEBs[lockbin]);
    return status;
}				       /*}}} */

/* This allocates a new, initialized addrstat structure, which is used for
 * keeping track of the FEB status of an address. It expects a shepherd pointer
 * to use to find the right memory pool to use. */
static inline qthread_addrstat_t *qthread_addrstat_new(qthread_shepherd_t *
						       shepherd)
{				       /*{{{ */
    qthread_addrstat_t *ret = ALLOC_ADDRSTAT(shepherd);

    if (ret != NULL) {
	if (pthread_mutex_init(&ret->lock, NULL) != 0) {
	    FREE_ADDRSTAT(ret);
	    return NULL;
	}
	ret->full = 1;
	ret->EFQ = NULL;
	ret->FEQ = NULL;
	ret->FFQ = NULL;
	QTHREAD_EMPTY_TIMER_INIT(ret);
    }
    return ret;
}				       /*}}} */

/* this function removes the FEB data structure for the address maddr from the
 * hash table */
static inline void qthread_FEB_remove(void *maddr)
{				       /*{{{ */
    qthread_addrstat_t *m;
    const int lockbin = QTHREAD_CHOOSE_STRIPE(maddr);

    qthread_debug("qthread_FEB_remove(): attempting removal\n");
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
    cp_hashtable_wrlock(qlib->FEBs[lockbin]); {
	m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs[lockbin],
						    maddr);
	if (m) {
	    QTHREAD_LOCK(&(m->lock));
	    REPORTLOCK(m);
	    if (m->FEQ == NULL && m->EFQ == NULL && m->FFQ == NULL &&
		m->full == 1) {
		qthread_debug
		    ("qthread_FEB_remove(): all lists are empty, and status is full\n");
		cp_hashtable_remove(qlib->FEBs[lockbin], maddr);
	    } else {
		QTHREAD_UNLOCK(&(m->lock));
		REPORTUNLOCK(m);
		qthread_debug
		    ("qthread_FEB_remove(): address cannot be removed; in use\n");
		m = NULL;
	    }
	}
    }
    cp_hashtable_unlock(qlib->FEBs[lockbin]);
    if (m != NULL) {
	QTHREAD_UNLOCK(&m->lock);
	REPORTUNLOCK(m);
	qthread_addrstat_delete(m);
    }
}				       /*}}} */

static inline void qthread_gotlock_empty(qthread_addrstat_t * m, void *maddr,
					 const char recursive)
{				       /*{{{ */
    qthread_addrres_t *X = NULL;
    int removeable;

    m->full = 0;
    QTHREAD_EMPTY_TIMER_START(m);
    if (m->EFQ != NULL) {
	/* dQ */
	X = m->EFQ;
	m->EFQ = X->next;
	/* op */
	if (maddr && maddr != X->addr) {
	    memcpy(maddr, X->addr, sizeof(aligned_t));
	}
	/* requeue */
	X->waiter->thread_state = QTHREAD_STATE_RUNNING;
	qthread_enqueue(X->waiter->shepherd_ptr->ready, X->waiter);
	FREE_ADDRRES(X);
	qthread_gotlock_fill(m, maddr, 1);
    }
    if (m->full == 1 && m->EFQ == NULL && m->FEQ == NULL && m->FFQ == NULL)
	removeable = 1;
    else
	removeable = 0;
    if (recursive == 0) {
	QTHREAD_UNLOCK(&m->lock);
	REPORTUNLOCK(m);
	if (removeable) {
	    qthread_FEB_remove(maddr);
	}
    }
}				       /*}}} */

static inline void qthread_gotlock_fill(qthread_addrstat_t * m, void *maddr,
					const char recursive)
{				       /*{{{ */
    qthread_addrres_t *X = NULL;
    int removeable;

    qthread_debug("qthread_gotlock_fill(%p, %p)\n", m, maddr);
    m->full = 1;
    QTHREAD_EMPTY_TIMER_STOP(m);
    /* dequeue all FFQ, do their operation, and schedule them */
    qthread_debug("qthread_gotlock_fill(): dQ all FFQ\n");
    while (m->FFQ != NULL) {
	/* dQ */
	X = m->FFQ;
	m->FFQ = X->next;
	/* op */
	if (X->addr && X->addr != maddr) {
	    memcpy(X->addr, maddr, sizeof(aligned_t));
	}
	/* schedule */
	X->waiter->thread_state = QTHREAD_STATE_RUNNING;
	qthread_enqueue(X->waiter->shepherd_ptr->ready, X->waiter);
	FREE_ADDRRES(X);
    }
    if (m->FEQ != NULL) {
	/* dequeue one FEQ, do their operation, and schedule them */
	qthread_t *waiter;

	qthread_debug("qthread_gotlock_fill(): dQ 1 FEQ\n");
	X = m->FEQ;
	m->FEQ = X->next;
	/* op */
	if (X->addr && X->addr != maddr) {
	    memcpy(X->addr, maddr, sizeof(aligned_t));
	}
	waiter = X->waiter;
	waiter->thread_state = QTHREAD_STATE_RUNNING;
	qthread_enqueue(waiter->shepherd_ptr->ready, waiter);
	FREE_ADDRRES(X);
	qthread_gotlock_empty(m, maddr, 1);
    }
    if (m->EFQ == NULL && m->FEQ == NULL && m->full == 1)
	removeable = 1;
    else
	removeable = 1;
    if (recursive == 0) {
	QTHREAD_UNLOCK(&m->lock);
	REPORTUNLOCK(m);
	/* now, remove it if it needs to be removed */
	if (removeable) {
	    qthread_FEB_remove(maddr);
	}
    }
}				       /*}}} */

int qthread_empty(qthread_t * me, const void *dest)
{				       /*{{{ */
    qthread_addrstat_t *m;
    aligned_t *alignedaddr;
    const int lockbin = QTHREAD_CHOOSE_STRIPE(dest);

    ALIGN(dest, alignedaddr, "qthread_empty()");
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
    cp_hashtable_wrlock(qlib->FEBs[lockbin]);
    {				       /* BEGIN CRITICAL SECTION */
	m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs[lockbin],
						    (void *)alignedaddr);
	if (!m) {
	    /* currently full, and must be added to the hash to empty */
	    m = qthread_addrstat_new(me ? (me->shepherd_ptr) :
				     pthread_getspecific(shepherd_structs));
	    if (!m) {
		cp_hashtable_unlock(qlib->FEBs[lockbin]);
		return QTHREAD_MALLOC_ERROR;
	    }
	    m->full = 0;
	    QTHREAD_EMPTY_TIMER_START(m);
	    cp_hashtable_put(qlib->FEBs[lockbin], (void *)alignedaddr, m);
	    m = NULL;
	} else {
	    /* it could be either full or not, don't know */
	    QTHREAD_LOCK(&m->lock);
	    REPORTLOCK(m);
	}
    }				       /* END CRITICAL SECTION */
    cp_hashtable_unlock(qlib->FEBs[lockbin]);
    if (m) {
	qthread_gotlock_empty(m, (void *)alignedaddr, 0);
    }
    return QTHREAD_SUCCESS;
}				       /*}}} */

int qthread_fill(qthread_t * me, const void *dest)
{				       /*{{{ */
    qthread_addrstat_t *m;
    aligned_t *alignedaddr;
    const int lockbin = QTHREAD_CHOOSE_STRIPE(dest);

    ALIGN(dest, alignedaddr, "qthread_fill()");
    /* lock hash */
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
    cp_hashtable_wrlock(qlib->FEBs[lockbin]);
    {				       /* BEGIN CRITICAL SECTION */
	m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs[lockbin],
						    (void *)alignedaddr);
	if (m) {
	    QTHREAD_LOCK(&m->lock);
	    REPORTLOCK(m);
	}
    }				       /* END CRITICAL SECTION */
    cp_hashtable_unlock(qlib->FEBs[lockbin]);	/* unlock hash */
    if (m) {
	/* if dest wasn't in the hash, it was already full. Since it was,
	 * we need to fill it. */
	qthread_gotlock_fill(m, (void *)alignedaddr, 0);
    }
    return QTHREAD_SUCCESS;
}				       /*}}} */

/* the way this works is that:
 * 1 - data is copies from src to destination
 * 2 - the destination's FEB state gets changed from empty to full
 */

int qthread_writeF(qthread_t * me, void *dest, const void *src)
{				       /*{{{ */
    qthread_addrstat_t *m;
    aligned_t *alignedaddr;
    const int lockbin = QTHREAD_CHOOSE_STRIPE(dest);

    if (me == NULL) {
	me = qthread_self();
    }
    ALIGN(dest, alignedaddr, "qthread_fill_with()");
    QTHREAD_LOCK_UNIQUERECORD(feb, dest, me);
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
    cp_hashtable_wrlock(qlib->FEBs[lockbin]); {	/* lock hash */
	m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs[lockbin],
						    (void *)alignedaddr);
	if (!m) {
	    m = qthread_addrstat_new(me->shepherd_ptr);
	    if (!m) {
		cp_hashtable_unlock(qlib->FEBs[lockbin]);
		return QTHREAD_MALLOC_ERROR;
	    }
	    cp_hashtable_put(qlib->FEBs[lockbin], alignedaddr, m);
	}
	QTHREAD_LOCK(&m->lock);
	REPORTLOCK(m);
    }
    cp_hashtable_unlock(qlib->FEBs[lockbin]);	/* unlock hash */
    /* we have the lock on m, so... */
    if (dest && dest != src) {
	memcpy(dest, src, sizeof(aligned_t));
    }
    qthread_gotlock_fill(m, alignedaddr, 0);
    return QTHREAD_SUCCESS;
}				       /*}}} */

int qthread_writeF_const(qthread_t * me, void *dest, const aligned_t src)
{				       /*{{{ */
    return qthread_writeF(me, dest, &src);
}				       /*}}} */

/* the way this works is that:
 * 1 - destination's FEB state must be "empty"
 * 2 - data is copied from src to destination
 * 3 - the destination's FEB state gets changed from empty to full
 */

int qthread_writeEF(qthread_t * me, void *dest, const void *src)
{				       /*{{{ */
    qthread_addrstat_t *m;
    qthread_addrres_t *X = NULL;
    aligned_t *alignedaddr;
    const int lockbin = QTHREAD_CHOOSE_STRIPE(dest);

    QTHREAD_LOCK_TIMER_DECLARATION(febblock);

    if (me == NULL) {
	me = qthread_self();
    }
    QTHREAD_LOCK_UNIQUERECORD(feb, dest, me);
    QTHREAD_LOCK_TIMER_START(febblock);
    qthread_debug("qthread_writeEF(%p, %p, %p): init\n", me, dest, src);
    ALIGN(dest, alignedaddr, "qthread_writeEF()");
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
    cp_hashtable_wrlock(qlib->FEBs[lockbin]); {
	m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs[lockbin],
						    (void *)alignedaddr);
	if (!m) {
	    m = qthread_addrstat_new(me->shepherd_ptr);
	    if (!m) {
		cp_hashtable_unlock(qlib->FEBs[lockbin]);
		return QTHREAD_MALLOC_ERROR;
	    }
	    cp_hashtable_put(qlib->FEBs[lockbin], alignedaddr, m);
	}
	QTHREAD_LOCK(&(m->lock));
	REPORTLOCK(m);
    }
    cp_hashtable_unlock(qlib->FEBs[lockbin]);
    qthread_debug("qthread_writeEF(): data structure locked\n");
    /* by this point m is locked */
    qthread_debug("qthread_writeEF(): m->full == %i\n", m->full);
    if (m->full == 1) {		       /* full, thus, we must block */
	QTHREAD_WAIT_TIMER_DECLARATION;
	X = ALLOC_ADDRRES(me->shepherd_ptr);
	if (X == NULL) {
	    QTHREAD_UNLOCK(&(m->lock));
	    REPORTUNLOCK(m);
	    return QTHREAD_MALLOC_ERROR;
	}
	X->addr = (aligned_t *) src;
	X->waiter = me;
	X->next = m->EFQ;
	m->EFQ = X;
	qthread_debug("qthread_writeEF(): back to parent\n");
	me->thread_state = QTHREAD_STATE_FEB_BLOCKED;
	me->blockedon = (struct qthread_lock_s *)m;
	QTHREAD_WAIT_TIMER_START();
	qthread_back_to_master(me);
	QTHREAD_WAIT_TIMER_STOP(me, febwait);
    } else {
	if (dest && dest != src) {
	    memcpy(dest, src, sizeof(aligned_t));
	}
	qthread_gotlock_fill(m, alignedaddr, 0);
    }
    QTHREAD_LOCK_TIMER_STOP(febblock, me);
    return QTHREAD_SUCCESS;
}				       /*}}} */

int qthread_writeEF_const(qthread_t * me, void *dest, const aligned_t src)
{				       /*{{{ */
    return qthread_writeEF(me, dest, &src);
}				       /*}}} */

/* the way this works is that:
 * 1 - src's FEB state must be "full"
 * 2 - data is copied from src to destination
 */

int qthread_readFF(qthread_t * me, void *dest, const void *src)
{				       /*{{{ */
    qthread_addrstat_t *m = NULL;
    qthread_addrres_t *X = NULL;
    aligned_t *alignedaddr;
    const int lockbin = QTHREAD_CHOOSE_STRIPE(src);

    QTHREAD_LOCK_TIMER_DECLARATION(febblock);

    if (me == NULL) {
	me = qthread_self();
    }
    QTHREAD_LOCK_UNIQUERECORD(feb, src, me);
    QTHREAD_LOCK_TIMER_START(febblock);
    qthread_debug("qthread_readFF(%p, %p, %p): init\n", me, dest, src);
    ALIGN(src, alignedaddr, "qthread_readFF()");
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
    cp_hashtable_wrlock(qlib->FEBs[lockbin]); {
	m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs[lockbin],
						    (void *)alignedaddr);
	if (!m) {
	    if (dest && dest != src) {
		memcpy(dest, src, sizeof(aligned_t));
	    }
	} else {
	    QTHREAD_LOCK(&m->lock);
	    REPORTLOCK(m);
	}
    }
    cp_hashtable_unlock(qlib->FEBs[lockbin]);
    qthread_debug("qthread_readFF(): data structure locked\n");
    /* now m, if it exists, is locked - if m is NULL, then we're done! */
    if (m == NULL) {		       /* already full! */
	if (dest && dest != src) {
	    memcpy(dest, src, sizeof(aligned_t));
	}
    } else if (m->full != 1) {	       /* not full... so we must block */
	QTHREAD_WAIT_TIMER_DECLARATION;
	X = ALLOC_ADDRRES(me->shepherd_ptr);
	if (X == NULL) {
	    QTHREAD_UNLOCK(&m->lock);
	    REPORTUNLOCK(m);
	    return QTHREAD_MALLOC_ERROR;
	}
	X->addr = (aligned_t *) dest;
	X->waiter = me;
	X->next = m->FFQ;
	m->FFQ = X;
	qthread_debug("qthread_readFF(): back to parent\n");
	me->thread_state = QTHREAD_STATE_FEB_BLOCKED;
	me->blockedon = (struct qthread_lock_s *)m;
	QTHREAD_WAIT_TIMER_START();
	qthread_back_to_master(me);
	QTHREAD_WAIT_TIMER_STOP(me, febwait);
    } else {			       /* exists AND is empty... weird, but that's life */
	if (dest && dest != src) {
	    memcpy(dest, src, sizeof(aligned_t));
	}
	QTHREAD_UNLOCK(&m->lock);
	REPORTUNLOCK(m);
    }
    QTHREAD_LOCK_TIMER_STOP(febblock, me);
    return QTHREAD_SUCCESS;
}				       /*}}} */

/* the way this works is that:
 * 1 - src's FEB state must be "full"
 * 2 - data is copied from src to destination
 * 3 - the src's FEB bits get changed from full to empty
 */

int qthread_readFE(qthread_t * me, void *dest, void *src)
{				       /*{{{ */
    qthread_addrstat_t *m;
    aligned_t *alignedaddr;
    const int lockbin = QTHREAD_CHOOSE_STRIPE(src);

    QTHREAD_LOCK_TIMER_DECLARATION(febblock);

    if (me == NULL) {
	me = qthread_self();
    }
    QTHREAD_LOCK_UNIQUERECORD(feb, src, me);
    QTHREAD_LOCK_TIMER_START(febblock);
    qthread_debug("qthread_readFE(%p, %p, %p): init\n", me, dest, src);
    ALIGN(src, alignedaddr, "qthread_readFE()");
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
    cp_hashtable_wrlock(qlib->FEBs[lockbin]); {
	m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs[lockbin],
						    alignedaddr);
	if (!m) {
	    m = qthread_addrstat_new(me->shepherd_ptr);
	    if (!m) {
		cp_hashtable_unlock(qlib->FEBs[lockbin]);
		return QTHREAD_MALLOC_ERROR;
	    }
	    cp_hashtable_put(qlib->FEBs[lockbin], alignedaddr, m);
	}
	QTHREAD_LOCK(&(m->lock));
	REPORTLOCK(m);
    }
    cp_hashtable_unlock(qlib->FEBs[lockbin]);
    qthread_debug("qthread_readFE(): data structure locked\n");
    /* by this point m is locked */
    if (m->full == 0) {		       /* empty, thus, we must block */
	QTHREAD_WAIT_TIMER_DECLARATION;
	qthread_addrres_t *X = ALLOC_ADDRRES(me->shepherd_ptr);

	if (X == NULL) {
	    QTHREAD_UNLOCK(&m->lock);
	    REPORTUNLOCK(m);
	    return QTHREAD_MALLOC_ERROR;
	}
	X->addr = (aligned_t *) dest;
	X->waiter = me;
	X->next = m->FEQ;
	m->FEQ = X;
	qthread_debug("qthread_readFE(): back to parent\n");
	me->thread_state = QTHREAD_STATE_FEB_BLOCKED;
	/* so that the shepherd will unlock it */
	me->blockedon = (struct qthread_lock_s *)m;
	QTHREAD_WAIT_TIMER_START();
	qthread_back_to_master(me);
	QTHREAD_WAIT_TIMER_STOP(me, febwait);
    } else {			       /* full, thus IT IS OURS! MUAHAHAHA! */
	if (dest && dest != src) {
	    memcpy(dest, src, sizeof(aligned_t));
	}
	qthread_gotlock_empty(m, alignedaddr, 0);
    }
    QTHREAD_LOCK_TIMER_STOP(febblock, me);
    return QTHREAD_SUCCESS;
}				       /*}}} */

/* functions to implement FEB-ish locking/unlocking
 *
 * These are atomic and functional, but do not have the same semantics as full
 * FEB locking/unlocking (for example, unlocking cannot block)
 *
 * NOTE: these have not been profiled, and so may need tweaking for speed
 * (e.g. multiple hash tables, shortening critical section, etc.)
 */

/* The lock ordering in these functions is very particular, and is designed to
 * reduce the impact of having a centralized hashtable. Don't monkey with it
 * unless you REALLY know what you're doing!
 */

int qthread_lock(qthread_t * me, const void *a)
{				       /*{{{ */
    qthread_lock_t *m;
    const int lockbin = QTHREAD_CHOOSE_STRIPE(a);

    QTHREAD_LOCK_TIMER_DECLARATION(aquirelock);

    if (me == NULL) {
	me = qthread_self();
    }
    QTHREAD_LOCK_UNIQUERECORD(lock, a, me);
    QTHREAD_LOCK_TIMER_START(aquirelock);

    QTHREAD_COUNT_THREADS_BINCOUNTER(locks, lockbin);
    cp_hashtable_wrlock(qlib->locks[lockbin]);
    m = (qthread_lock_t *) cp_hashtable_get(qlib->locks[lockbin], (void *)a);
    if (m == NULL) {
	m = ALLOC_LOCK(me->shepherd_ptr);
	if (m == NULL) {
	    cp_hashtable_unlock(qlib->locks[lockbin]);
	    return QTHREAD_MALLOC_ERROR;
	}
	assert(me->shepherd_ptr == (qthread_shepherd_t *)
	       pthread_getspecific(shepherd_structs));
	m->waiting = qthread_queue_new(me->shepherd_ptr);
	if (m->waiting == NULL) {
	    FREE_LOCK(m);
	    cp_hashtable_unlock(qlib->locks[lockbin]);
	    return QTHREAD_MALLOC_ERROR;
	}
	if (pthread_mutex_init(&m->lock, NULL) != 0) {
	    qthread_queue_free(m->waiting);
	    FREE_LOCK(m);
	    cp_hashtable_unlock(qlib->locks[lockbin]);
	    return QTHREAD_PTHREAD_ERROR;
	}
	QTHREAD_HOLD_TIMER_INIT(m);
	cp_hashtable_put(qlib->locks[lockbin], (void *)a, m);
	/* since we just created it, we own it */
	QTHREAD_LOCK(&m->lock);
	/* can only unlock the hash after we've locked the address, because
	 * otherwise there's a race condition: the address could be removed
	 * before we have a chance to add ourselves to it */
	cp_hashtable_unlock(qlib->locks[lockbin]);

#ifdef QTHREAD_DEBUG
	m->owner = me->thread_id;
#endif
	QTHREAD_UNLOCK(&m->lock);
	qthread_debug("qthread_lock(%p, %p): returned (wasn't locked)\n", me,
		      a);
    } else {
	QTHREAD_WAIT_TIMER_DECLARATION;
	/* success==failure: because it's in the hash, someone else owns
	 * the lock; dequeue this thread and yield. NOTE: it's up to the
	 * master thread to enqueue this thread and unlock the address
	 */
	QTHREAD_LOCK(&m->lock);
	/* for an explanation of the lock/unlock ordering here, see above */
	cp_hashtable_unlock(qlib->locks[lockbin]);

	me->thread_state = QTHREAD_STATE_BLOCKED;
	me->blockedon = m;

	QTHREAD_WAIT_TIMER_START();

	qthread_back_to_master(me);

	QTHREAD_WAIT_TIMER_STOP(me, lockwait);

	/* once I return to this context, I own the lock! */
	/* conveniently, whoever unlocked me already set up everything too */
	qthread_debug("qthread_lock(%p, %p): returned (was locked)\n", me, a);
    }
    QTHREAD_LOCK_TIMER_STOP(aquirelock, me);
    QTHREAD_HOLD_TIMER_START(m);
    return QTHREAD_SUCCESS;
}				       /*}}} */

int qthread_unlock(qthread_t * me, const void *a)
{				       /*{{{ */
    qthread_lock_t *m;
    qthread_t *u;
    const int lockbin = QTHREAD_CHOOSE_STRIPE(a);

    qthread_debug("qthread_unlock(%p, %p): started\n", me, a);

    QTHREAD_COUNT_THREADS_BINCOUNTER(locks, lockbin);
    cp_hashtable_wrlock(qlib->locks[lockbin]);
    m = (qthread_lock_t *) cp_hashtable_get(qlib->locks[lockbin], (void *)a);
    if (m == NULL) {
	/* unlocking an address that's already unlocked */
	cp_hashtable_unlock(qlib->locks[lockbin]);
	return QTHREAD_REDUNDANT;
    }
    QTHREAD_LOCK(&m->lock);

    QTHREAD_HOLD_TIMER_STOP(m, me->shepherd_ptr);

    /* unlock the address... if anybody's waiting for it, give them the lock
     * and put them in a ready queue.  If not, delete the lock structure.
     */

    QTHREAD_LOCK(&m->waiting->lock);
    u = qthread_dequeue_nonblocking(m->waiting);
    if (u == NULL) {
	qthread_debug("qthread_unlock(%p,%p): deleting waiting queue\n", me,
		      a);
	cp_hashtable_remove(qlib->locks[lockbin], (void *)a);
	cp_hashtable_unlock(qlib->locks[lockbin]);
	QTHREAD_HOLD_TIMER_DESTROY(m);
	QTHREAD_UNLOCK(&m->waiting->lock);
	qthread_queue_free(m->waiting);
	QTHREAD_UNLOCK(&m->lock);
	QTHREAD_DESTROYLOCK(&m->lock);
	FREE_LOCK(m);
    } else {
	cp_hashtable_unlock(qlib->locks[lockbin]);
	qthread_debug
	    ("qthread_unlock(%p,%p): pulling thread from queue (%p)\n", me, a,
	     u);
	u->thread_state = QTHREAD_STATE_RUNNING;
#ifdef QTHREAD_DEBUG
	m->owner = u->thread_id;
#endif

	/* NOTE: because of the use of getcontext()/setcontext(), threads
	 * return to the shepherd that setcontext()'d into them, so they
	 * must remain in that queue.
	 */
	qthread_enqueue(u->shepherd_ptr->ready, u);

	QTHREAD_UNLOCK(&m->waiting->lock);
	QTHREAD_UNLOCK(&m->lock);
    }

    return QTHREAD_SUCCESS;
}				       /*}}} */

/* These are just accessor functions */
unsigned qthread_id(const qthread_t * t)
{				       /*{{{ */
#ifdef QTHREAD_NONLAZY_THREADIDS
    return t ? t->thread_id : (unsigned int)-1;
#else
    if (!t) {
	return (unsigned int)-1;
    }
    if (t->thread_id != (unsigned int)-1) {
	return t->thread_id;
    }
    ((qthread_t *) t)->thread_id =
	qthread_internal_incr(&(qlib->max_thread_id),
			      &qlib->max_thread_id_lock);
    return t->thread_id;
#endif
}				       /*}}} */

qthread_shepherd_id_t qthread_shep(const qthread_t * t)
{				       /*{{{ */
    qthread_shepherd_t *ret;

    if (t) {
	return t->shepherd_ptr->shepherd_id;
    }
    ret = (qthread_shepherd_t *) pthread_getspecific(shepherd_structs);
    if (ret == NULL) {
	return NO_SHEPHERD;
    } else {
	return ret->shepherd_id;
    }
}				       /*}}} */

/* these two functions are helper functions for futurelib
 * (nobody else gets to have 'em!) */
unsigned int qthread_isfuture(const qthread_t * t)
{				       /*{{{ */
    return t ? (t->flags & QTHREAD_FUTURE) : 0;
}				       /*}}} */

void qthread_assertfuture(qthread_t * t)
{				       /*{{{ */
    t->flags |= QTHREAD_FUTURE;
}				       /*}}} */

void qthread_assertnotfuture(qthread_t * t)
{				       /*{{{ */
    t->flags &= ~QTHREAD_FUTURE;
}				       /*}}} */
