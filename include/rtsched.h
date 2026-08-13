/*
 * Copyright (c) 2023-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-01-19     Shell        Separate scheduling statements from rt_thread_t
 *                             to rt_sched_thread_ctx. Add definitions of scheduler.
 */
#ifndef __RT_SCHED_H__
#define __RT_SCHED_H__

/**
 * @file rtsched.h
 * @brief Scheduler-owned portion of a thread control block and internal APIs.
 *
 * RT_SCHED_THREAD_CTX is embedded in `struct rt_thread`. Keeping scheduling
 * state in a dedicated subobject makes ownership and locking requirements
 * explicit while allowing the UP and SMP schedulers to share thread code.
 *
 * Application code should use the public rt_thread_* and IPC APIs instead of
 * editing this state. Most fields participate in ready-queue membership,
 * priority bitmaps, timeout races, or cross-CPU decisions; observing or changing
 * them without the scheduler lock can corrupt a queue or produce a stale state.
 *
 * Priority values follow RT-Thread convention: a numerically smaller value has
 * higher scheduling priority. Threads at the same priority are ordered by the
 * ready list and time-slice/yield policy.
 */

#include "rttypes.h"
#include "rtcompiler.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rt_thread;

/**
 * Storage type for RT_THREAD_* state and auxiliary status flag bits.
 * The base lifecycle state is obtained with RT_THREAD_STAT_MASK; other bits may
 * record modifiers such as a pending yield.
 */
typedef rt_uint8_t rt_sched_thread_status_t;

/**
 * @brief Scheduler-private priority and time-slice bookkeeping for one thread.
 *
 * Callers outside scheduler implementation code must never access these fields
 * directly. The derived masks have to stay consistent with current_priority
 * and with the ready queues. Updating only one field can make a runnable thread
 * invisible to the highest-priority lookup.
 */
struct rt_sched_thread_priv
{
    /** Time slice reloaded when the thread yields or begins a new round. */
    rt_tick_t                   init_tick;
    /** Ticks left in the current time slice; decremented by scheduler ticks. */
    rt_tick_t                   remaining_tick;

    /** Effective priority currently used for ready-queue selection. */
    rt_uint8_t                  current_priority;
    /** Configured/base priority used when temporary inheritance is removed. */
    rt_uint8_t                  init_priority;
#if RT_THREAD_PRIORITY_MAX > 32
    /** Group index (`current_priority >> 3`) in the two-level priority bitmap. */
    rt_uint8_t                  number;
    /** Bit selecting this priority inside its eight-priority group. */
    rt_uint8_t                  high_mask;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
    /**
     * Ready-group bit. For at most 32 priorities it directly selects the
     * priority; for more priorities it selects the group named by number.
     */
    rt_uint32_t                 number_mask;

};

/**
 * @brief Scheduler-visible state embedded in every thread control block.
 *
 * Despite the historical word "public", members are public to cooperating
 * kernel subsystems, not to applications. A caller must hold the scheduler lock
 * before reading or writing mutable members unless an implementation explicitly
 * documents a lockless initialization phase.
 */
struct rt_sched_thread_ctx
{
    /** Intrusive node used by exactly one scheduler-owned list at a time. */
    rt_list_t                   thread_list_node;

    /** Base RT_THREAD_* lifecycle state plus RT_THREAD_STAT_* modifier bits. */
    rt_uint8_t                  stat;
    /** Per-thread marker reserved for scheduler-lock ownership bookkeeping. */
    rt_uint8_t                  sched_flag_locked:1;
    /** Scheduler tracks the embedded timeout timer as active for current wait. */
    rt_uint8_t                  sched_flag_ttmr_set:1;

#ifdef ARCH_USING_HW_THREAD_SELF
    /** Reschedule request deferred while the thread is in a critical section. */
    rt_uint8_t                  critical_switch_flag:1;
#endif /* ARCH_USING_HW_THREAD_SELF */

#ifdef RT_USING_SMP
    /**
     * Requested CPU affinity. RT_CPUS_NR is the sentinel for an unbound thread;
     * otherwise the value is a logical CPU index.
     */
    rt_uint8_t                  bind_cpu;
    /**
     * CPU currently executing this thread, or RT_CPU_DETACHED while it is not
     * running. This prevents a thread from running simultaneously on two CPUs.
     */
    rt_uint8_t                  oncpu;

    /** Nested scheduler-critical-section depth owned by this thread. */
    rt_base_t                   critical_lock_nest;
#endif

    /** Private bitmap, priority, and time-slice data maintained by scheduler. */
    struct rt_sched_thread_priv sched_thread_priv;
};

/** Place the scheduler context member in `struct rt_thread` with its ABI name. */
#define RT_SCHED_THREAD_CTX struct rt_sched_thread_ctx sched_thread_ctx;

/** Access the scheduler-private subobject of a thread pointer. */
#define RT_SCHED_PRIV(thread) ((thread)->sched_thread_ctx.sched_thread_priv)
/** Access the scheduler-visible context of a thread pointer. */
#define RT_SCHED_CTX(thread) ((thread)->sched_thread_ctx)

/**
 * @brief Convert a scheduler list node back to its containing thread.
 *
 * The first rt_list_entry() recovers `struct rt_sched_thread_ctx` from the
 * embedded node. rt_container_of() then recovers `struct rt_thread` from its
 * embedded scheduling context. @p node must really be a thread_list_node;
 * passing an arbitrary list node gives undefined pointer arithmetic.
 */
#define RT_THREAD_LIST_NODE_ENTRY(node)                                      \
    rt_container_of(                                                         \
        rt_list_entry((node), struct rt_sched_thread_ctx, thread_list_node), \
        struct rt_thread, sched_thread_ctx)
/** Return a thread's scheduler list node as an lvalue. */
#define RT_THREAD_LIST_NODE(thread) (RT_SCHED_CTX(thread).thread_list_node)

/**
 * @name System scheduler locking
 *
 * A scheduler lock protects ready queues and state transitions against local
 * interrupt handlers and, on SMP, other CPUs. The saved level is an opaque
 * restore token, not a Boolean. Every successful lock must be paired with one
 * unlock using the exact returned value on every control-flow path.
 *
 * These short internal locks differ from the application-facing critical
 * section nesting API. Code holding one must not block or perform an operation
 * that expects the scheduler to make progress.
 * @{
 */

/** Opaque interrupt/lock state saved by rt_sched_lock(). */
typedef rt_ubase_t rt_sched_lock_level_t;

/**
 * @brief Lock scheduler state and save the previous level in @p plvl.
 * @return RT_EOK, or -RT_EINVAL when @p plvl is RT_NULL.
 */
rt_err_t rt_sched_lock(rt_sched_lock_level_t *plvl);

/**
 * @brief Unlock scheduler state without explicitly requesting rescheduling.
 * @param level Exact token returned through a successful rt_sched_lock().
 * @return RT_EOK after the saved scheduler/interrupt state is restored.
 */
rt_err_t rt_sched_unlock(rt_sched_lock_level_t level);

/**
 * @brief Unlock scheduler state and honor a pending scheduling decision.
 *
 * Depending on UP/SMP and call context, a switch may occur immediately, be
 * deferred until interrupt exit, or return an error describing why scheduling
 * could not be performed at that point.
 *
 * @param level Exact token returned through a successful rt_sched_lock().
 * @return RT_EOK on completion. The SMP implementation can report a negative
 *         status when scheduling is unavailable, is requested from an ISR, or
 *         remains locked by an enclosing scheduler-critical region.
 */
rt_err_t rt_sched_unlock_n_resched(rt_sched_lock_level_t level);

/**
 * Return whether the calling CPU owns the scheduler-context lock.
 *
 * The common implementation is present only in scheduler_mp.c.  There is no UP
 * definition in this tree, and UP code must not call the declaration directly;
 * the public debug macros below expand away in that configuration.
 */
rt_bool_t rt_sched_is_locked(void);

#ifdef RT_USING_SMP
/* Debug-only ownership assertions; do not acquire or release any lock. */
#define RT_SCHED_DEBUG_IS_LOCKED do { RT_ASSERT(rt_sched_is_locked()); } while (0)
#define RT_SCHED_DEBUG_IS_UNLOCKED do { RT_ASSERT(!rt_sched_is_locked()); } while (0)

#else /* !RT_USING_SMP */

#define RT_SCHED_DEBUG_IS_LOCKED
#define RT_SCHED_DEBUG_IS_UNLOCKED
#endif /* RT_USING_SMP */
/** @} */

/**
 * @name Kernel-private thread scheduling operations
 *
 * User code must never call these directly. Use rt_thread_* or an IPC API,
 * which validates lifecycle state and performs the complete lock/list/timer
 * protocol. The declarations are exposed only while compiling kernel or IPC
 * sources to discourage accidental use by components and applications.
 * @{
 */
#if defined(__RT_KERNEL_SOURCE__) || defined(__RT_IPC_SOURCE__)

/**
 * Initialize the complete scheduling context for a newly constructed thread.
 * Sets its lifecycle state and SMP detached/affinity sentinels, then initializes
 * private priority and time-slice data.  The enclosing object can already have
 * been linked into the object registry, so the creating path must prevent it
 * from becoming schedulable until this initialization is complete.
 *
 * @param thread Newly constructed thread control block.
 * @param tick Initial time-slice length in scheduler ticks.
 * @param priority Base priority in [0, RT_THREAD_PRIORITY_MAX).
 */
void rt_sched_thread_init_ctx(struct rt_thread *thread, rt_uint32_t tick, rt_uint8_t priority);

/**
 * Initialize intrusive-list, priority bitmap, and time-slice fields. UP and SMP
 * provide separate implementations because their lock bookkeeping differs.
 *
 * @param thread Thread whose private scheduling data is uninitialized.
 * @param tick Initial and remaining time slice.
 * @param priority Initial effective and base priority.
 */
void rt_sched_thread_init_priv(struct rt_thread *thread, rt_uint32_t tick, rt_uint8_t priority);

/**
 * Calculate ready-bitmap masks from current priority and put a new thread into
 * the suspended state from which the ordinary resume/start path can ready it.
 * @param thread Initialized thread not yet visible in a scheduling queue.
 */
void rt_sched_thread_startup(struct rt_thread *thread);

/**
 * Complete SMP scheduler bookkeeping after the low-level context has changed.
 * Called with local interrupts disabled as part of the stack-pointer switch
 * transaction; not a general post-switch hook for applications.  The common
 * implementation exists only in the SMP scheduler, despite this unconditional
 * declaration; UP code must not call it unless its port supplies an override.
 * @param thread Incoming thread that now owns the processor context.
 */
void rt_sched_post_ctx_switch(struct rt_thread *thread);

/**
 * Charge @p tick ticks to the current thread's time slice. On exhaustion it
 * marks the thread as yielded and requests rescheduling. Typically called from
 * the system tick interrupt path.
 * @return RT_EOK after accounting and any scheduling request.
 */
rt_err_t rt_sched_tick_increase(rt_tick_t tick);

/**
 * Return the base RT_THREAD_* state of @p thread after masking modifier bits.
 * The scheduler lock must be held.
 */
rt_uint8_t rt_sched_thread_get_stat(struct rt_thread *thread);

/** Return @p thread's effective/current priority; scheduler lock must be held. */
rt_uint8_t rt_sched_thread_get_curr_prio(struct rt_thread *thread);

/** Return @p thread's configured/base priority used as inheritance baseline. */
rt_uint8_t rt_sched_thread_get_init_prio(struct rt_thread *thread);

/**
 * Reload @p thread's time slice and set its yield modifier under scheduler lock.
 * @return RT_EOK.
 */
rt_err_t rt_sched_thread_yield(struct rt_thread *thread);

/**
 * Mark @p thread closed after runnable/wait membership has been resolved.
 * @return RT_EOK; scheduler lock must be held.
 */
rt_err_t rt_sched_thread_close(struct rt_thread *thread);

/**
 * Atomically transition a suspended thread to ready state. If a timeout timer
 * is active it is stopped first, allowing a timeout/producer race to be
 * detected without placing the thread in two queues.
 * @param thread Suspended thread to make runnable; scheduler lock must be held.
 * @return RT_EOK on success, or a negative status if the state/timer race
 *         prevents this caller from completing the transition.
 */
rt_err_t rt_sched_thread_ready(struct rt_thread *thread);

/**
 * Reserved scheduler/port suspend-transition declaration.
 *
 * The common UP and SMP scheduler sources in this tree do not provide a generic
 * definition or public calling contract for this symbol.  Kernel code uses
 * rt_thread_suspend_to_list() and the existing ready/list protocol instead.
 * A port that supplies this optional symbol must define the meaning of @p level
 * and its locking and return-value contract.
 */
rt_err_t rt_sched_thread_suspend(struct rt_thread *thread, rt_sched_lock_level_t level);

/**
 * Change only @p thread's effective priority, for example during inheritance.
 * A ready thread is removed and reinserted so queue/bitmap state stays coherent.
 * The scheduler lock must be held and @p priority must be in the configured
 * priority range.
 */
rt_err_t rt_sched_thread_change_priority(struct rt_thread *thread, rt_uint8_t priority);

/**
 * Change both effective and configured/base priority of @p thread to @p priority.
 * A ready thread is requeued under the new priority. The scheduler lock must be
 * held and @p priority must be in the configured priority range.
 */
rt_err_t rt_sched_thread_reset_priority(struct rt_thread *thread, rt_uint8_t priority);

/**
 * Bind a thread to logical CPU @p cpu in SMP. Valid bound IDs are
 * `0 .. RT_CPUS_NR - 1`; RT_CPUS_NR represents unbound, and larger values are
 * normalized to that sentinel by the SMP implementation. Negative IDs are not
 * valid input. The UP implementation rejects the operation with -RT_EINVAL.
 * @param thread Thread whose ready-queue placement/affinity may be updated.
 * @return RT_EOK in SMP after the update, or -RT_EINVAL in a UP build.
 * @note The SMP implementation acquires the scheduler lock itself and therefore
 *       expects to be called without that lock already held.
 */
rt_err_t rt_sched_thread_bind_cpu(struct rt_thread *thread, int cpu);

/**
 * Return nonzero when @p thread carries the scheduler suspended state mask.
 * The scheduler lock must be held.
 */
rt_uint8_t rt_sched_thread_is_suspended(struct rt_thread *thread);

/**
 * Stop @p thread's scheduler-managed timeout timer and clear its tracking flag.
 * @return RT_EOK if no active timer needed stopping, otherwise rt_timer_stop()
 *         status. The tracking flag is cleared even if that stop reports error.
 * @note The scheduler lock must be held.
 */
rt_err_t rt_sched_thread_timer_stop(struct rt_thread *thread);

/**
 * Mark the embedded timeout timer as scheduler-managed for the current wait.
 * This updates the race-tracking flag; insertion into a timer list is performed
 * by the timer subsystem around this call.
 * @return RT_EOK; scheduler lock must be held.
 */
rt_err_t rt_sched_thread_timer_start(struct rt_thread *thread);

/**
 * Insert @p thread in the appropriate ready queue and update bitmaps/state.
 * Yield/time-slice state determines head-versus-tail placement at its priority.
 * The SMP implementation asserts that the scheduler lock is already held; UP
 * kernel callers likewise serialize the complete state transition externally.
 */
void rt_sched_insert_thread(struct rt_thread *thread);

/**
 * Remove @p thread from its ready queue and clear now-empty priority bits.
 * The SMP implementation requires the scheduler lock to be held.
 */
void rt_sched_remove_thread(struct rt_thread *thread);

/**
 * Reserved scheduler/port current-thread accessor.
 *
 * The common scheduler sources in this tree use rt_thread_self() and do not
 * define this symbol.  Code must not call it unless the selected architecture
 * or scheduler extension provides and documents an implementation.
 */
struct rt_thread *rt_sched_thread_self(void);

#endif /* defined(__RT_KERNEL_SOURCE__) || defined(__RT_IPC_SOURCE__) */
/** @} */

#ifdef __cplusplus
}
#endif

#endif /* __RT_SCHED_H__ */
