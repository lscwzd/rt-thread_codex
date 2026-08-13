/*
 * Copyright (c) 2006-2024 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-18     Bernard      the first version
 * 2006-04-26     Bernard      add semaphore APIs
 * 2006-08-10     Bernard      add version information
 * 2007-01-28     Bernard      rename RT_OBJECT_Class_Static to RT_Object_Class_Static
 * 2007-03-03     Bernard      clean up the definitions to rtdef.h
 * 2010-04-11     yi.qiu       add module feature
 * 2013-06-24     Bernard      add rt_kprintf re-define when not use RT_USING_CONSOLE.
 * 2016-08-09     ArdaFu       add new thread and interrupt hook.
 * 2018-11-22     Jesven       add all cpu's lock and ipi handler
 * 2021-02-28     Meco Man     add RT_KSERVICE_USING_STDLIB
 * 2021-11-14     Meco Man     add rtlegacy.h for compatibility
 * 2022-06-04     Meco Man     remove strnlen
 * 2023-05-20     Bernard      add rtatomic.h header file to included files.
 * 2023-06-30     ChuShicheng  move debug check from the rtdebug.h
 * 2023-10-16     Shell        Support a new backtrace framework
 * 2023-12-10     xqyjlj       fix spinlock in up
 * 2024-01-25     Shell        Add rt_susp_list for IPC primitives
 * 2024-03-10     Meco Man     move std libc related functions to rtklibc
 */

#ifndef __RT_THREAD_H__
#define __RT_THREAD_H__

/**
 * @file rtthread.h
 * @brief Public RT-Thread kernel API and cross-subsystem service declarations.
 *
 * Applications, components, BSP drivers, and kernel implementation files use
 * this umbrella header to access object, clock, timer, thread, scheduler, IPC,
 * memory, device, interrupt, console, and diagnostic services.  Object layouts
 * and command/flag values live in rtdef.h; architecture contracts live in
 * rthw.h; intrusive containers and low-level helpers live in rtservice.h.
 *
 * Context rules are part of the API contract.  Any operation that may wait
 * requires a running scheduler, thread context, and an available scheduler.
 * Non-blocking release/notification operations may be callable from an ISR only
 * where the implementation explicitly supports it.  Hook callbacks execute
 * synchronously at the hook point and inherit that point's interrupt, locking,
 * and reentrancy constraints.
 */

#include <rtconfig.h>
#include <rtdef.h>
#include <rtservice.h>
#include <rtm.h>
#include <rtatomic.h>
#include <rtklibc.h>
#ifdef RT_USING_LEGACY
#include <rtlegacy.h>
#endif
#ifdef RT_USING_FINSH
#include <finsh.h>
#endif /* RT_USING_FINSH */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __GNUC__
/** GCC-compatible C entry wrapper used by the standard startup path. */
int entry(void);
#endif

/** @name Kernel object registry and lifetime management
 *
 * In the generic object API, rt_object_init() marks an object with the Static
 * attribute and pairs with detach, whereas rt_object_allocate() omits that bit
 * and pairs with delete.  The bit records the initialization/lifetime path, not
 * the physical origin of the backing storage: a class wrapper may initialize a
 * heap-allocated instance and later free it through its own destroy API.
 * @{ */
/** Return the internal class registry, or NULL when @p type is unsupported. */
struct rt_object_information *
rt_object_get_information(enum rt_object_class_type type);
/**
 * Return the number of objects in the global registry for @p type.
 * Module-owned objects kept on a module-private list are not included.
 */
int rt_object_get_length(enum rt_object_class_type type);
/**
 * Copy up to @p maxlen pointers from @p type's global registry.  Objects on a
 * loadable module's private object list are not included.
 */
int rt_object_get_pointers(enum rt_object_class_type type, rt_object_t *pointers, int maxlen);

/** Initialize and register a static object in caller-owned memory. */
void rt_object_init(struct rt_object         *object,
                    enum rt_object_class_type type,
                    const char               *name);
/** Detach a static object from its class registry without freeing its storage. */
void rt_object_detach(rt_object_t object);
#ifdef RT_USING_HEAP
/** Allocate, initialize, and register a dynamic object of the requested class. */
rt_object_t rt_object_allocate(enum rt_object_class_type type, const char *name);
/** Unregister and free an object created by rt_object_allocate(). */
void rt_object_delete(rt_object_t object);
/**
 * Create a dynamic custom wrapper for @p data.  The data itself is released
 * only through @p data_destroy; the wrapper does not otherwise know its type or
 * allocation origin.
 */
rt_object_t rt_custom_object_create(const char *name, void *data, rt_err_t (*data_destroy)(void *));
/**
 * Invoke the data destructor when present, then delete the wrapper regardless
 * of the destructor result.  A missing destructor leaves the initial negative
 * return status even though a valid wrapper is still deleted.
 */
rt_err_t rt_custom_object_destroy(rt_object_t obj);
#endif /* RT_USING_HEAP */
/** Return whether the current object type carries RT_Object_Class_Static. */
rt_bool_t rt_object_is_systemobject(rt_object_t object);
/** Return the logical class with ownership bits removed as defined by object.c. */
rt_uint8_t rt_object_get_type(rt_object_t object);
/**
 * Visit the global registry of @p type until the iterator returns nonzero.
 * Objects linked only on a loadable module's private list are not visited.
 * The iterator runs while the class-registry spinlock is held: it must not
 * block, mutate that registry, or call an API that reacquires the same lock.
 * A positive iterator result stops successfully; a negative result propagates.
 */
rt_err_t rt_object_for_each(rt_uint8_t type, rt_object_iter_t iter, void *data);
/**
 * Find a global-registry object after truncating @p name to RT_NAME_MAX - 1,
 * or return NULL.  Call only from thread context; module-private objects are
 * not searched.
 */
rt_object_t rt_object_find(const char *name, rt_uint8_t type);
/** Copy an object's name to a caller buffer and guarantee bounded access. */
rt_err_t rt_object_get_name(rt_object_t object, char *name, rt_uint8_t name_size);

#ifdef RT_USING_HOOK
/**
 * Install single-listener object hooks.
 *
 * attach runs after base initialization but before registry insertion; detach
 * runs before registry removal and before the type becomes Null.  The generic
 * trytake/take/put hooks are historical trace points whose exact timing and
 * success meaning depend on each call site: trytake includes nonblocking
 * attempts, timer take precedes list insertion, and put does not guarantee that
 * the operation following the hook will succeed.  Some callbacks execute while
 * an object lock is held.  Passing NULL disables a function-pointer hook.  Each
 * callback receives a borrowed pointer and must remain bounded, avoid recursive
 * use of the same object, and not retain it beyond the object's lifetime.
 */
void rt_object_attach_sethook(void (*hook)(struct rt_object *object));
void rt_object_detach_sethook(void (*hook)(struct rt_object *object));
void rt_object_trytake_sethook(void (*hook)(struct rt_object *object));
void rt_object_take_sethook(void (*hook)(struct rt_object *object));
void rt_object_put_sethook(void (*hook)(struct rt_object *object));
#endif /* RT_USING_HOOK */
/** @} */

/**
 * @addtogroup group_clock_management
 * @{
 */

/** @name Tick and timer services
 *
 * Tick values use unsigned wraparound arithmetic.  Durations should be compared
 * through the supplied helpers rather than assuming the counter never wraps.
 * Timer callbacks normally run in interrupt context for hard timers and in the
 * timer service thread for soft timers.  With RT_USING_TIMER_ALL_SOFT, the
 * worker thread dispatches every timer regardless of its HARD flag.
 * @{ */
/** Return the current system tick maintained by the clock interrupt. */
rt_tick_t rt_tick_get(void);
/** Return elapsed ticks since @p base using wraparound-safe unsigned subtraction. */
rt_tick_t rt_tick_get_delta(rt_tick_t base);
/** Replace the system tick counter; intended for clock/platform management. */
void rt_tick_set(rt_tick_t tick);
/**
 * Advance one tick, update the current time slice, and check due timers.
 *
 * The common implementation asserts that interrupt nesting is nonzero, so a
 * clock ISR must call this only after rt_interrupt_enter().
 */
void rt_tick_increase(void);
/**
 * Advance the system clock by an explicit number of ticks.
 *
 * Like rt_tick_increase(), the common implementation requires ISR context
 * established by rt_interrupt_enter().
 */
void rt_tick_increase_tick(rt_tick_t tick);
/**
 * Convert milliseconds to ticks, rounding a non-integral positive duration up;
 * a negative input becomes the RT_WAITING_FOREVER bit pattern.
 */
rt_tick_t  rt_tick_from_millisecond(rt_int32_t ms);
/**
 * Return system uptime converted to milliseconds.  The weak default is exact
 * only when RT_TICK_PER_SECOND divides 1000; otherwise it emits a build warning
 * and returns zero unless the BSP supplies a higher-precision override.
 */
rt_tick_t rt_tick_get_millisecond(void);
#ifdef RT_USING_HOOK
/**
 * Install the hook called before tick/time-slice/timer accounting, normally in
 * ISR context.  `rt_tick_increase_tick(n)` invokes it once for the whole batch,
 * not once for every tick represented by `n`.
 */
void rt_tick_sethook(void (*hook)(void));
#endif /* RT_USING_HOOK */

/**
 * Initialize the hard-timer ordered lists before scheduler startup; this is a
 * no-op when RT_USING_TIMER_ALL_SOFT removes the hard-timer lists.
 */
void rt_system_timer_init(void);
/**
 * Initialize and start the soft-timer worker and semaphore when
 * RT_USING_TIMER_SOFT is enabled; otherwise the common function is a no-op.
 */
void rt_system_timer_thread_init(void);

/**
 * Initialize a static timer in caller-owned storage.
 *
 * @param timer Timer control block that remains valid until detached.
 * @param name Object name used for diagnostics and lookup.
 * @param timeout Callback invoked on expiration.
 * @param parameter Opaque callback argument.
 * @param time Initial relative timeout in ticks.
 * @param flag ONE_SHOT/PERIODIC and HARD/SOFT policy bits.
 */
void rt_timer_init(rt_timer_t  timer,
                   const char *name,
                   void (*timeout)(void *parameter),
                   void       *parameter,
                   rt_tick_t   time,
                   rt_uint8_t  flag);
/** Stop and unregister a static timer; does not free caller storage. */
rt_err_t rt_timer_detach(rt_timer_t timer);
#ifdef RT_USING_HEAP
/** Allocate and initialize a dynamic timer; return NULL on failure. */
rt_timer_t rt_timer_create(const char *name,
                           void (*timeout)(void *parameter),
                           void       *parameter,
                           rt_tick_t   time,
                           rt_uint8_t  flag);
/** Stop, unregister, and free a timer created by rt_timer_create(). */
rt_err_t rt_timer_delete(rt_timer_t timer);
#endif /* RT_USING_HEAP */
/** Arm or restart @p timer relative to the current tick. */
rt_err_t rt_timer_start(rt_timer_t timer);
/** Deactivate @p timer; succeeds according to the timer's current state contract. */
rt_err_t rt_timer_stop(rt_timer_t timer);
/** Execute an RT_TIMER_CTRL_* command; @p arg type depends on @p cmd. */
rt_err_t rt_timer_control(rt_timer_t timer, int cmd, void *arg);
/** Return the earliest absolute deadline among active hard and soft timers. */
rt_tick_t rt_timer_next_timeout_tick(void);
/**
 * Service timers from the tick ISR after rt_interrupt_enter().  Normally it
 * dispatches due hard timers and notifies the soft-timer worker.  With
 * RT_USING_TIMER_ALL_SOFT it only wakes that worker; it does not directly
 * dispatch a callback marked HARD.
 */
void rt_timer_check(void);
#ifdef RT_USING_HOOK
/**
 * Install callbacks immediately before and after each timer callback.
 *
 * The hooks receive the timer being dispatched and run in the same context as
 * its callback (normally ISR for hard and worker thread for soft; all callbacks
 * use the worker under RT_USING_TIMER_ALL_SOFT).  They must not block in actual
 * interrupt context or invalidate the timer under dispatch.
 */
void rt_timer_enter_sethook(void (*hook)(struct rt_timer *timer));
void rt_timer_exit_sethook(void (*hook)(struct rt_timer *timer));
#endif /* RT_USING_HOOK */
/** @} */

/**@}*/

/** @name Thread lifecycle and execution control
 *
 * A newly initialized/created thread is in INIT state and does not execute until
 * rt_thread_startup().  Smaller numeric priorities are higher.  @p tick is the
 * time slice used for round-robin scheduling among equal-priority threads.
 * Static and dynamic lifetime APIs must not be mixed.
 * @{ */
/**
 * Initialize a static TCB and caller-provided stack.
 *
 * @param thread Caller-owned TCB.
 * @param name Diagnostic object name.
 * @param entry Thread body; returning from it follows the normal exit path.
 * @param parameter Opaque argument passed to @p entry.
 * @param stack_start Base address of writable stack storage.
 * @param stack_size Stack bytes available from @p stack_start.
 * @param priority Initial scheduler priority in the configured range.
 * @param tick Initial time-slice length in scheduler ticks.
 */
rt_err_t rt_thread_init(struct rt_thread *thread,
                        const char       *name,
                        void (*entry)(void *parameter),
                        void             *parameter,
                        void             *stack_start,
                        rt_uint32_t       stack_size,
                        rt_uint8_t        priority,
                        rt_uint32_t       tick);
/**
 * Close a statically allocated thread and arrange any required deferred
 * cleanup; the TCB and stack storage remain owned by the caller.
 */
rt_err_t rt_thread_detach(rt_thread_t thread);
#ifdef RT_USING_HEAP
/** Allocate a TCB and stack, then initialize a dynamic thread in INIT state. */
rt_thread_t rt_thread_create(const char *name,
                             void (*entry)(void *parameter),
                             void       *parameter,
                             rt_uint32_t stack_size,
                             rt_uint8_t  priority,
                             rt_uint32_t tick);
/** Close a dynamic thread and arrange deferred TCB/stack reclamation. */
rt_err_t rt_thread_delete(rt_thread_t thread);
#endif /* RT_USING_HEAP */
/**
 * Low-level transition of @p thread to CLOSE state.
 *
 * This removes scheduler membership and detaches the embedded timeout timer,
 * but does not by itself unregister/free the TCB or perform the full mutex and
 * deferred-reclamation path.  Normal callers should use the matching
 * rt_thread_detach()/rt_thread_delete() lifetime API.  Closing the current
 * thread requires the caller to have prevented scheduling as documented by the
 * implementation.
 */
rt_err_t rt_thread_close(rt_thread_t thread);
/** Return the scheduler's current thread, or NULL before one has been selected. */
rt_thread_t rt_thread_self(void);
/**
 * Find a global-registry thread after truncating @p name to RT_NAME_MAX - 1.
 * The returned pointer is borrowed; call only from thread context.
 */
rt_thread_t rt_thread_find(char *name);
/** Move an INIT thread into the ready set and request scheduling as needed. */
rt_err_t rt_thread_startup(rt_thread_t thread);
/** Relinquish the current thread's remaining slice to an equal-priority peer. */
rt_err_t rt_thread_yield(void);
/**
 * Delay the current thread for up to @p tick scheduler ticks; zero is invalid.
 * The interruptible wait may return early when signal handling wakes it.
 */
rt_err_t rt_thread_delay(rt_tick_t tick);
/**
 * Delay to the next absolute point in a periodic schedule.
 *
 * @p tick stores the caller's previous release point.  When the next release
 * remains in the future it is advanced by @p inc_tick and the thread sleeps;
 * if the deadline has already been missed, it is reset to the current tick and
 * the function returns without sleeping.  This avoids cumulative drift while
 * recovering from an overrun.
 */
rt_err_t rt_thread_delay_until(rt_tick_t *tick, rt_tick_t inc_tick);
/** Convert @p ms to ticks and delay the current thread. */
rt_err_t rt_thread_mdelay(rt_int32_t ms);
/** Execute an RT_THREAD_CTRL_* request; @p arg depends on @p cmd. */
rt_err_t rt_thread_control(rt_thread_t thread, int cmd, void *arg);
/**
 * Suspend @p thread with the default uninterruptible policy.  Normal application
 * use should suspend only the current thread; asynchronously suspending another
 * thread can freeze it while it owns a lock or shared resource, and a running
 * thread on another CPU is not a valid target.
 */
rt_err_t rt_thread_suspend(rt_thread_t thread);
/** Suspend @p thread using RT_INTERRUPTIBLE/KILLABLE/UNINTERRUPTIBLE policy. */
rt_err_t rt_thread_suspend_with_flag(rt_thread_t thread, int suspend_flag);
/** Remove a suspended thread from its wait state and make it ready. */
rt_err_t rt_thread_resume(rt_thread_t thread);
#ifdef RT_USING_SMART
/**
 * Consume and clear the thread's one-shot wakeup adapter, then invoke it; when
 * no adapter is installed, fall back to rt_thread_resume().
 */
rt_err_t rt_thread_wakeup(rt_thread_t thread);
/** Install the wait-object adapter and opaque object used by rt_thread_wakeup(). */
void rt_thread_wakeup_set(struct rt_thread *thread, rt_wakeup_func_t func, void* user_data);
#endif /* RT_USING_SMART */
/** Copy a thread name into a bounded caller buffer. */
rt_err_t rt_thread_get_name(rt_thread_t thread, char *name, rt_uint8_t name_size);
#ifdef RT_USING_CPU_USAGE_TRACER
/** Return the most recently sampled utilization percentage for @p thread. */
rt_uint8_t rt_thread_get_usage(rt_thread_t thread);
#endif /* RT_USING_CPU_USAGE_TRACER */
#ifdef RT_USING_SIGNALS
/** Allocate and initialize the classic signal-handler vector for @p tid. */
void rt_thread_alloc_sig(rt_thread_t tid);
/** Release classic signal resources associated with @p tid. */
void rt_thread_free_sig(rt_thread_t tid);
/** Queue signal @p sig to @p tid and wake it when the signal policy permits. */
int  rt_thread_kill(rt_thread_t tid, int sig);
#endif /* RT_USING_SIGNALS */
#ifdef RT_USING_HOOK
/**
 * Install lifecycle tracing hooks.  The suspend hook runs only after a
 * successful suspension transition; the resume hook is reached after the
 * resume flow even when its ready operation reports an error, so it is not a
 * general success notification.
 *
 * The callbacks run synchronously while scheduler/IPC state may still be
 * protected.  They are intended for tracing; they must be bounded, nonblocking,
 * and must not recursively suspend/resume threads.
 */
void rt_thread_suspend_sethook(void (*hook)(rt_thread_t thread));
void rt_thread_resume_sethook (void (*hook)(rt_thread_t thread));

/**
 * @ingroup group_thread_management
 *
 * @brief Handler type for the multi-listener thread-initialized hook.
 *
 * @param thread Newly initialized TCB.  The thread has not necessarily been
 *               started and the pointer is borrowed from its owner.
 *
 * Handlers execute in the initializing caller's context and should be used for
 * observation/instrumentation rather than changing scheduler-visible fields.
 */
typedef void (*rt_thread_inited_hookproto_t)(rt_thread_t thread);
RT_OBJECT_HOOKLIST_DECLARE(rt_thread_inited_hookproto_t, rt_thread_inited);

#endif /* RT_USING_HOOK */
/** @} */

/** @name Idle-thread services
 *
 * There is one lowest-priority idle thread per CPU.  The common hook table is
 * traversed by the primary CPU's generic idle loop; secondary SMP idle loops
 * normally call the architecture idle routine directly.  Hooks may execute
 * with power-management or deferred-cleanup work nearby.
 * @{ */
/** Create and bind the per-CPU idle thread(s) during kernel startup. */
void rt_thread_idle_init(void);
#if defined(RT_USING_HOOK) || defined(RT_USING_IDLE_HOOK)
/**
 * @ingroup group_thread_management
 *
 * @brief Register a function called from each primary-CPU generic idle iteration.
 *
 * @param hook Function placed in the first free table slot.  The implementation
 *             does not reject NULL or duplicate registrations.  NULL returns
 *             RT_EOK but leaves the chosen slot empty and reusable; duplicate
 *             non-NULL functions can occupy multiple slots and run repeatedly.
 *
 * @return `RT_EOK`: set OK.
 *         `-RT_EFULL`: hook list is full.
 *
 * @note The callback executes in the primary CPU's idle-thread loop, must be
 *       short, must never block or suspend, and should not busy-loop because it
 *       directly affects power saving and lowest-priority maintenance work.
 *       In the common SMP idle implementation, secondary CPUs execute their
 *       architecture idle routine directly and do not traverse this hook table.
 */
rt_err_t rt_thread_idle_sethook(void (*hook)(void));
/** Remove a previously registered idle hook. */
rt_err_t rt_thread_idle_delhook(void (*hook)(void));
#endif /* defined(RT_USING_HOOK) || defined(RT_USING_IDLE_HOOK) */
/** Return the idle thread for the current CPU. */
rt_thread_t rt_thread_idle_gethandler(void);
/** Return true when @p thread is one of the system's per-CPU idle threads. */
rt_bool_t rt_thread_is_idle_thread(rt_thread_t thread);
/** @} */

/** @name Scheduler services
 *
 * Most functions in this group are kernel/BSP integration points rather than
 * application APIs.  A scheduling critical section prevents thread switches;
 * it is nestable and is not equivalent to a general data lock on SMP.
 * @{ */
/** Initialize ready queues, bitmaps, locks, and per-CPU scheduling state. */
void rt_system_scheduler_init(void);
/** Select the first ready thread and perform the non-returning first context switch. */
void rt_system_scheduler_start(void);

/** Reevaluate the highest-priority ready thread and switch if required. */
void rt_schedule(void);
/**
 * Complete a deferred SMP switch on the interrupt-return path.  The common
 * implementation exists only in the SMP scheduler; UP ports must not reference
 * this declaration unless they provide their own implementation.
 */
void rt_scheduler_do_irq_switch(void *context);

#ifdef RT_USING_OVERFLOW_CHECK
/** Validate stack sentinel/bounds for @p thread and invoke overflow policy on failure. */
void rt_scheduler_stack_check(struct rt_thread *thread);

/** Compile a stack check into context-switch paths when overflow checking is enabled. */
#define RT_SCHEDULER_STACK_CHECK(thr) rt_scheduler_stack_check(thr)

#else /* !RT_USING_OVERFLOW_CHECK */

#define RT_SCHEDULER_STACK_CHECK(thr)

#endif /* RT_USING_OVERFLOW_CHECK */

/**
 * Enter a nestable scheduler critical section and return the new nesting level;
 * an SMP call before a current thread exists returns -RT_EINVAL.
 */
rt_base_t rt_enter_critical(void);
/** Leave one scheduler critical nesting level and perform deferred scheduling if due. */
void rt_exit_critical(void);
/**
 * Leave one nesting level, using @p critical_level only to verify in a debug
 * build that it equals the current level returned by the matching enter call.
 * It does not restore an arbitrary earlier level; non-debug builds ignore it.
 */
void rt_exit_critical_safe(rt_base_t critical_level);
/** Return the current CPU's scheduler critical-section nesting depth. */
rt_uint16_t rt_critical_level(void);

#ifdef RT_USING_HOOK
/**
 * Install scheduler diagnostics hooks.
 *
 * The overflow hook is called when a stack check fails and returns a policy
 * status.  The scheduler hook observes a selected from/to pair before the
 * switch; the switch hook receives the outgoing/current thread at the point a
 * low-level context switch is requested.  All execute on a highly constrained
 * scheduling path and must be nonblocking, bounded, and independent of
 * operations that can schedule again.
 */
void rt_scheduler_stack_overflow_sethook(rt_err_t (*hook)(struct rt_thread *thread));
void rt_scheduler_sethook(void (*hook)(rt_thread_t from, rt_thread_t to));
void rt_scheduler_switch_sethook(void (*hook)(struct rt_thread *tid));
#endif /* RT_USING_HOOK */

#ifdef RT_USING_SMP
/** Architecture entry used after a secondary CPU has completed low-level bring-up. */
void rt_secondary_cpu_entry(void);
/** Handle scheduler-related inter-processor interrupts. */
void rt_scheduler_ipi_handler(int vector, void *param);
#endif /* RT_USING_SMP */
/** @} */

/**
 * @addtogroup group_signal
 * @{
 */
#ifdef RT_USING_SIGNALS
/** Block delivery of classic signal @p signo to the current thread. */
void rt_signal_mask(int signo);
/** Unblock delivery of classic signal @p signo to the current thread. */
void rt_signal_unmask(int signo);
/** Inspect pending signals and prepare an architecture return context if needed. */
void *rt_signal_check(void* context);
/** Install a classic handler and return the previous handler. */
rt_sighandler_t rt_signal_install(int signo, rt_sighandler_t handler);
/** Wait until one signal in @p set arrives or @p timeout expires. */
int rt_signal_wait(const rt_sigset_t *set, rt_siginfo_t *si, rt_int32_t timeout);
/** Initialize global classic signal support during kernel startup. */
int rt_system_signal_init(void);
#endif /* RT_USING_SIGNALS */
/**@}*/

/**
 * @addtogroup group_memory_management
 * @{
 */

/*
 * Memory API families are independent:
 * - rt_malloc() uses the single configured system-heap backend.
 * - rt_smem/rt_slab manage explicit allocator objects.
 * - rt_memheap manages one or more variable-sized regions.
 * - rt_mempool manages fixed-size blocks and can wait for a returned block.
 */
#ifdef RT_USING_MEMPOOL
/** Initialize a static fixed-block pool over [start, start + size). */
rt_err_t rt_mp_init(struct rt_mempool *mp,
                    const char        *name,
                    void              *start,
                    rt_size_t          size,
                    rt_size_t          block_size);
/** Detach a static pool after all users and waiters have finished with it. */
rt_err_t rt_mp_detach(struct rt_mempool *mp);
#ifdef RT_USING_HEAP
/** Allocate pool metadata/storage for @p block_count blocks of @p block_size. */
rt_mp_t rt_mp_create(const char *name,
                     rt_size_t   block_count,
                     rt_size_t   block_size);
/** Delete a dynamically created pool when no block remains outstanding. */
rt_err_t rt_mp_delete(rt_mp_t mp);
#endif /* RT_USING_HEAP */
/** Obtain one block, waiting @p time ticks; returns NULL on timeout/failure. */
void *rt_mp_alloc(rt_mp_t mp, rt_int32_t time);
/** Return a block to its owning pool and wake a waiter if present. */
void rt_mp_free(void *block);
#ifdef RT_USING_HOOK
/**
 * Install pool allocation/free observation hooks.
 *
 * The allocation hook is called only after a block has been obtained and
 * receives the pool and non-NULL result; allocation failures return before the
 * hook.  The free hook observes the owning pool and returned block before it is
 * reinserted.  Both run outside the pool spinlock but inherit the caller's
 * context and must not recursively allocate/free the same pool or block.
 */
void rt_mp_alloc_sethook(void (*hook)(struct rt_mempool *mp, void *block));
void rt_mp_free_sethook(void (*hook)(struct rt_mempool *mp, void *block));
#endif /* RT_USING_HOOK */

#endif /* RT_USING_MEMPOOL */

#ifdef RT_USING_HEAP
/** Initialize the configured system heap over [begin_addr, end_addr). */
void rt_system_heap_init(void *begin_addr, void *end_addr);
/** Backend-neutral system-heap initializer used by the default weak wrapper. */
void rt_system_heap_init_generic(void *begin_addr, void *end_addr);

/** Allocate at least @p size bytes, or return NULL. */
void *rt_malloc(rt_size_t size);
/** Release a pointer returned by the system allocator; NULL is accepted. */
void rt_free(void *ptr);
/** Resize an allocation while preserving the minimum of old/new payload sizes. */
void *rt_realloc(void *ptr, rt_size_t newsize);
/**
 * Allocate `count * size` bytes initialized to zero.  The current generic
 * implementation does not check multiplication overflow; callers handling
 * untrusted dimensions must validate the product before calling.
 */
void *rt_calloc(rt_size_t count, rt_size_t size);
/** Allocate @p size bytes with the requested power-of-two alignment. */
void *rt_malloc_align(rt_size_t size, rt_size_t align);
/** Release memory returned specifically by rt_malloc_align(). */
void rt_free_align(void *ptr);

/** Return total managed, currently used, and high-water bytes when pointers are non-NULL. */
void rt_memory_info(rt_size_t *total,
                    rt_size_t *used,
                    rt_size_t *max_used);

#if defined(RT_USING_SLAB) && defined(RT_USING_SLAB_AS_HEAP)
/** Allocate @p npages contiguous pages from the slab page allocator. */
void *rt_page_alloc(rt_size_t npages);
/** Return exactly @p npages starting at a page-aligned slab address. */
void rt_page_free(void *addr, rt_size_t npages);
#endif /* defined(RT_USING_SLAB) && defined(RT_USING_SLAB_AS_HEAP) */

/**
 * @ingroup group_hook
 * @{
 */

#ifdef RT_USING_HOOK
/**
 * Install system-heap tracing hooks.
 *
 * `void **ptr` exposes the live pointer variable at the hook point: malloc and
 * realloc-exit receive the result, realloc-entry receives the old pointer, and
 * free receives the target pointer.  A hook that writes through this argument
 * changes the value subsequently returned, reallocated, or freed; observational
 * hooks should therefore leave it untouched.  The generic wrapper
 * invokes these hooks outside its outer heap lock, but they still inherit the
 * allocation caller's thread/ISR context.  They must not recursively allocate,
 * use formatting paths that allocate, or perform an unsafe blocking service.
 */
void rt_malloc_sethook(void (*hook)(void **ptr, rt_size_t size));
void rt_realloc_set_entry_hook(void (*hook)(void **ptr, rt_size_t size));
void rt_realloc_set_exit_hook(void (*hook)(void **ptr, rt_size_t size));
void rt_free_sethook(void (*hook)(void **ptr));
#endif /* RT_USING_HOOK */
/**@}*/

#endif /* RT_USING_HEAP */

#ifdef RT_USING_SMALL_MEM
/**
 * @name Small-memory allocator object API
 *
 * The object uses a compact address-ordered first-fit allocator suitable for a
 * relatively small contiguous region.  Returned pointers must be released by
 * the matching rt_smem_free() implementation.
 * @{
 */
/** Initialize a small-memory allocator over @p size bytes at @p begin_addr. */
rt_smem_t rt_smem_init(const char    *name,
                     void          *begin_addr,
                     rt_size_t      size);
/** Detach the allocator object after all allocations have been released. */
rt_err_t rt_smem_detach(rt_smem_t m);
/** Allocate @p size bytes from @p m. */
void *rt_smem_alloc(rt_smem_t m, rt_size_t size);
/** Resize @p rmem in its owning small-memory allocator. */
void *rt_smem_realloc(rt_smem_t m, void *rmem, rt_size_t newsize);
/** Free a small-memory allocation; ownership metadata identifies its allocator. */
void rt_smem_free(void *rmem);
/** @} */
#endif /* RT_USING_SMALL_MEM */

#ifdef RT_USING_MEMHEAP
/**
 * @name Memheap object API
 *
 * A memheap provides variable-size allocation from an explicit memory region
 * and may be combined with other memheaps when selected as the system heap.
 * @{
 */
/** Initialize @p memheap over the caller-provided region. */
rt_err_t rt_memheap_init(struct rt_memheap *memheap,
                         const char        *name,
                         void              *start_addr,
                         rt_size_t         size);
/** Detach an empty memheap from object/system-heap management. */
rt_err_t rt_memheap_detach(struct rt_memheap *heap);
/** Allocate from a specific memheap. */
void *rt_memheap_alloc(struct rt_memheap *heap, rt_size_t size);
/** Resize an allocation owned by @p heap. */
void *rt_memheap_realloc(struct rt_memheap *heap, void *ptr, rt_size_t newsize);
/** Free a memheap allocation; the block header records the owning heap. */
void rt_memheap_free(void *ptr);
/** Query total, used, and high-water bytes for one memheap. */
void rt_memheap_info(struct rt_memheap *heap,
                     rt_size_t *total,
                     rt_size_t *used,
                     rt_size_t *max_used);
/** @} */
#endif /* RT_USING_MEMHEAP */

#ifdef RT_USING_MEMHEAP_AS_HEAP
/**
 * Internal unlocked memheap operations used behind the outer system-heap lock.
 * Applications should use rt_malloc()/rt_free()/rt_realloc() instead.
 */
void *_memheap_alloc(struct rt_memheap *heap, rt_size_t size);
void _memheap_free(void *rmem);
void *_memheap_realloc(struct rt_memheap *heap, void *rmem, rt_size_t newsize);
#endif

#ifdef RT_USING_SLAB
/**
 * @name Slab allocator object API
 *
 * The allocator combines page allocation for large requests with size-class
 * zones for small requests.  All addresses must be returned to the same slab
 * object from which they were allocated.
 * @{
 */
/** Initialize a slab allocator over a page-aligned usable region. */
rt_slab_t rt_slab_init(const char *name, void *begin_addr, rt_size_t size);
/** Detach an unused slab allocator. */
rt_err_t rt_slab_detach(rt_slab_t m);
/** Allocate contiguous pages from a specific slab object. */
void *rt_slab_page_alloc(rt_slab_t m, rt_size_t npages);
/** Return contiguous pages to a specific slab object. */
void rt_slab_page_free(rt_slab_t m, void *addr, rt_size_t npages);
/** Allocate a byte-sized request through the slab size-class/page policy. */
void *rt_slab_alloc(rt_slab_t m, rt_size_t size);
/** Resize a slab allocation while preserving payload data. */
void *rt_slab_realloc(rt_slab_t m, void *ptr, rt_size_t size);
/** Release an allocation to its slab object. */
void rt_slab_free(rt_slab_t m, void *ptr);
/** @} */
#endif /* RT_USING_SLAB */

/**@}*/

/**
 * @addtogroup group_thread_comm
 * @{
 */

/**
 * @name Internal suspend-list primitives
 *
 * These functions form the bridge between an IPC object's wait queue and the
 * scheduler.  An enqueue atomically changes a thread from running/ready to
 * suspended and optionally orders it by priority.  A dequeue stops the embedded
 * timeout timer, removes the thread, assigns its wakeup error, and makes it
 * ready.  They are kernel building blocks, not application APIs.
 * @{
 */
/** Print a suspend list for diagnostics without changing its topology. */
void rt_susp_list_print(rt_list_t *list);
/** Sentinel telling dequeue/resume helpers to preserve the thread's existing error. */
#define RT_THREAD_RESUME_RES_THR_ERR (-1)
/** Remove and ready the first waiter; return that thread or NULL for an empty list. */
struct rt_thread *rt_susp_list_dequeue(rt_list_t *susp_list, rt_err_t thread_error);
/** Ready every waiter and assign/preserve @p thread_error. */
rt_err_t rt_susp_list_resume_all(rt_list_t *susp_list, rt_err_t thread_error);
/** Resume all waiters while coordinating an already-related IPC spinlock. */
rt_err_t rt_susp_list_resume_all_irq(rt_list_t *susp_list,
                                     rt_err_t thread_error,
                                     struct rt_spinlock *lock);

/** Atomically suspend @p thread and enqueue it with wait order/policy. */
rt_err_t rt_thread_suspend_to_list(rt_thread_t thread, rt_list_t *susp_list, int ipc_flags, int suspend_flag);
/** Enqueue an already-suspended thread; caller must hold the scheduler lock. */
rt_err_t rt_susp_list_enqueue(rt_list_t *susp_list, rt_thread_t thread, int ipc_flags);
/** @} */

/**
 * @addtogroup group_semaphore Semaphore
 * @{
 */

#ifdef RT_USING_SEMAPHORE
/** Initialize a static semaphore with @p value and FIFO/priority waiter order. */
rt_err_t rt_sem_init(rt_sem_t    sem,
                     const char *name,
                     rt_uint32_t value,
                     rt_uint8_t  flag);
/** Detach a static semaphore, waking/invalidating waiters as defined by IPC reset. */
rt_err_t rt_sem_detach(rt_sem_t sem);
#ifdef RT_USING_HEAP
/** Create a dynamically allocated semaphore. */
rt_sem_t rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag);
/** Delete a dynamic semaphore and release its object storage. */
rt_err_t rt_sem_delete(rt_sem_t sem);
#endif /* RT_USING_HEAP */

/** Acquire one token, waiting for @p timeout ticks if necessary. */
rt_err_t rt_sem_take(rt_sem_t sem, rt_int32_t timeout);
/** Acquire with a wait that ordinary signals may interrupt. */
rt_err_t rt_sem_take_interruptible(rt_sem_t sem, rt_int32_t timeout);
/** Acquire with a wait that only kill-class signals may interrupt. */
rt_err_t rt_sem_take_killable(rt_sem_t sem, rt_int32_t timeout);
/** Attempt immediate acquisition without blocking. */
rt_err_t rt_sem_trytake(rt_sem_t sem);
/** Return a token or transfer it directly to a waiting thread. */
rt_err_t rt_sem_release(rt_sem_t sem);
/** Execute a generic RT_IPC_CMD_* operation on the semaphore. */
rt_err_t rt_sem_control(rt_sem_t sem, int cmd, void *arg);
#endif /* RT_USING_SEMAPHORE */

/**@}*/

/**
 * @addtogroup group_mutex Mutex
 * @{
 */

#ifdef RT_USING_MUTEX
/** Initialize a static recursive mutex; waiters are priority ordered. */
rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, rt_uint8_t flag);
/** Detach an unused static mutex. */
rt_err_t rt_mutex_detach(rt_mutex_t mutex);
#ifdef RT_USING_HEAP
/** Create a dynamically allocated recursive mutex. */
rt_mutex_t rt_mutex_create(const char *name, rt_uint8_t flag);
/** Delete an unused dynamic mutex. */
rt_err_t rt_mutex_delete(rt_mutex_t mutex);
#endif /* RT_USING_HEAP */
/** Internal exit/recovery helper that removes @p thread's ownership relation. */
void rt_mutex_drop_thread(rt_mutex_t mutex, rt_thread_t thread);
/** Set the priority ceiling and return the previous ceiling. */
rt_uint8_t rt_mutex_setprioceiling(rt_mutex_t mutex, rt_uint8_t priority);
/** Return the currently configured priority ceiling. */
rt_uint8_t rt_mutex_getprioceiling(rt_mutex_t mutex);

/** Acquire recursively or block up to @p timeout ticks with priority inheritance. */
rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t timeout);
/** Attempt immediate acquisition without blocking. */
rt_err_t rt_mutex_trytake(rt_mutex_t mutex);
/** Acquire using a wait interruptible by ordinary signals. */
rt_err_t rt_mutex_take_interruptible(rt_mutex_t mutex, rt_int32_t time);
/** Acquire using a wait interruptible only by kill-class signals. */
rt_err_t rt_mutex_take_killable(rt_mutex_t mutex, rt_int32_t time);
/** Decrement recursive hold; on final release transfer ownership or unlock. */
rt_err_t rt_mutex_release(rt_mutex_t mutex);
/** Execute a generic RT_IPC_CMD_* operation on the mutex. */
rt_err_t rt_mutex_control(rt_mutex_t mutex, int cmd, void *arg);

/** Return the current owner as a borrowed pointer; result can change concurrently. */
rt_inline rt_thread_t rt_mutex_get_owner(rt_mutex_t mutex)
{
    return mutex->owner;
}
/** Return the current recursive hold depth; result can change concurrently. */
rt_inline rt_ubase_t rt_mutex_get_hold(rt_mutex_t mutex)
{
    return mutex->hold;
}

#endif /* RT_USING_MUTEX */

/**@}*/

/**
 * @addtogroup group_event Event
 * @{
 */

#ifdef RT_USING_EVENT
/** Initialize a static 32-bit event object with FIFO/priority waiter order. */
rt_err_t rt_event_init(rt_event_t event, const char *name, rt_uint8_t flag);
/** Detach a static event object after resolving its waiters. */
rt_err_t rt_event_detach(rt_event_t event);
#ifdef RT_USING_HEAP
/** Create a dynamically allocated event object. */
rt_event_t rt_event_create(const char *name, rt_uint8_t flag);
/** Delete a dynamic event object. */
rt_err_t rt_event_delete(rt_event_t event);
#endif /* RT_USING_HEAP */

/** OR @p set into the event state and wake every receiver whose condition matches. */
rt_err_t rt_event_send(rt_event_t event, rt_uint32_t set);
/**
 * Receive an AND/OR event condition, optionally clearing matched bits.
 * @p recved receives the bits that satisfied the condition.  A zero timeout is
 * nonblocking and RT_WAITING_FOREVER waits indefinitely.
 */
rt_err_t rt_event_recv(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   opt,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved);
/** Event receive whose wait may be interrupted by ordinary signals. */
rt_err_t rt_event_recv_interruptible(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   opt,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved);
/** Event receive whose wait may be interrupted only by kill-class signals. */
rt_err_t rt_event_recv_killable(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   opt,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved);
/** Execute a generic RT_IPC_CMD_* operation on the event object. */
rt_err_t rt_event_control(rt_event_t event, int cmd, void *arg);
#endif /* RT_USING_EVENT */

/**@}*/

/**
 * @addtogroup group_mailbox MailBox
 * @{
 */

#ifdef RT_USING_MAILBOX
/**
 * Initialize a static mailbox over @p size rt_ubase_t slots in @p msgpool.
 *
 * Only the pointer-width value is copied; ownership of any referenced object
 * remains with the sender/application.  @p flag selects FIFO or priority waiter
 * order.
 */
rt_err_t rt_mb_init(rt_mailbox_t mb,
                    const char  *name,
                    void        *msgpool,
                    rt_size_t    size,
                    rt_uint8_t   flag);
/** Detach a static mailbox after resolving blocked senders/receivers. */
rt_err_t rt_mb_detach(rt_mailbox_t mb);
#ifdef RT_USING_HEAP
/** Create a mailbox and allocate storage for @p size pointer-width messages. */
rt_mailbox_t rt_mb_create(const char *name, rt_size_t size, rt_uint8_t flag);
/** Delete a dynamic mailbox and its internal message storage. */
rt_err_t rt_mb_delete(rt_mailbox_t mb);
#endif /* RT_USING_HEAP */

/** Send immediately; fail instead of waiting when the mailbox is full. */
rt_err_t rt_mb_send(rt_mailbox_t mb, rt_ubase_t value);
/**
 * Nonblocking interruptible-send alias.  Its fixed zero timeout prevents
 * suspension, so the interruptible policy has no observable effect.
 */
rt_err_t rt_mb_send_interruptible(rt_mailbox_t mb, rt_ubase_t value);
/**
 * Nonblocking killable-send alias.  Its fixed zero timeout prevents suspension,
 * so the killable policy has no observable effect.
 */
rt_err_t rt_mb_send_killable(rt_mailbox_t mb, rt_ubase_t value);
/** Send, waiting up to @p timeout ticks for a free slot. */
rt_err_t rt_mb_send_wait(rt_mailbox_t mb,
                         rt_ubase_t  value,
                         rt_int32_t   timeout);
/** Waiting send that ordinary signals may interrupt. */
rt_err_t rt_mb_send_wait_interruptible(rt_mailbox_t mb,
                         rt_ubase_t  value,
                         rt_int32_t   timeout);
/** Waiting send that only kill-class signals may interrupt. */
rt_err_t rt_mb_send_wait_killable(rt_mailbox_t mb,
                         rt_ubase_t  value,
                         rt_int32_t   timeout);
/** Insert @p value at the receive side so it is returned before normal messages. */
rt_err_t rt_mb_urgent(rt_mailbox_t mb, rt_ubase_t value);
/** Receive one value into @p value, waiting up to @p timeout ticks. */
rt_err_t rt_mb_recv(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout);
/** Receive with a wait that ordinary signals may interrupt. */
rt_err_t rt_mb_recv_interruptible(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout);
/** Receive with a wait that only kill-class signals may interrupt. */
rt_err_t rt_mb_recv_killable(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout);
/** Execute a generic RT_IPC_CMD_* operation on the mailbox. */
rt_err_t rt_mb_control(rt_mailbox_t mb, int cmd, void *arg);
#endif /* RT_USING_MAILBOX */

/**@}*/

/**
 * @addtogroup group_messagequeue Message Queue
 * @{
 */
#ifdef RT_USING_MESSAGEQUEUE

/**
 * @brief Private header prepended to every fixed-capacity message node.
 *
 * next links either the queued FIFO/priority chain or the free-node chain;
 * length records the bytes valid in the payload.  Applications should use
 * RT_MQ_BUF_SIZE() rather than constructing this layout manually.
 */
struct rt_mq_message
{
    struct rt_mq_message *next; /**< Next internal node in a queue/free chain. */
    rt_ssize_t length;          /**< Valid payload length copied into this node. */
#ifdef RT_USING_MESSAGEQUEUE_PRIORITY
    rt_int32_t prio;            /**< Message priority used for ordered insertion. */
#endif /* RT_USING_MESSAGEQUEUE_PRIORITY */
};

/**
 * Return bytes required for a static queue of @p max_msgs payloads.
 *
 * Each payload is rounded up to RT_ALIGN_SIZE before adding its private header.
 * Arguments are evaluated in the resulting expression and should not have side
 * effects.  The caller must also ensure the multiplication does not overflow
 * rt_size_t and must provide suitably aligned storage.
 */
#define RT_MQ_BUF_SIZE(msg_size, max_msgs) \
((RT_ALIGN((msg_size), RT_ALIGN_SIZE) + sizeof(struct rt_mq_message)) * (max_msgs))

/**
 * Initialize a static fixed-message queue over @p pool_size bytes.
 *
 * @p msg_size is the maximum copied payload, while pool_size determines how
 * many aligned header+payload nodes can be carved from @p msgpool.
 */
rt_err_t rt_mq_init(rt_mq_t     mq,
                    const char *name,
                    void       *msgpool,
                    rt_size_t   msg_size,
                    rt_size_t   pool_size,
                    rt_uint8_t  flag);
/** Detach a static queue after resolving blocked senders/receivers. */
rt_err_t rt_mq_detach(rt_mq_t mq);
#ifdef RT_USING_HEAP
/** Create a queue with @p max_msgs dynamically allocated message nodes. */
rt_mq_t rt_mq_create(const char *name,
                     rt_size_t   msg_size,
                     rt_size_t   max_msgs,
                     rt_uint8_t  flag);
/** Delete a dynamic message queue and its node storage. */
rt_err_t rt_mq_delete(rt_mq_t mq);
#endif /* RT_USING_HEAP */

/** Copy and enqueue one message immediately, failing when the queue is full. */
rt_err_t rt_mq_send(rt_mq_t mq, const void *buffer, rt_size_t size);
/**
 * Nonblocking interruptible-send alias.  Its zero timeout prevents suspension,
 * so the interruptible policy has no observable effect.
 */
rt_err_t rt_mq_send_interruptible(rt_mq_t mq, const void *buffer, rt_size_t size);
/**
 * Nonblocking killable-send alias.  Its zero timeout prevents suspension, so
 * the killable policy has no observable effect.
 */
rt_err_t rt_mq_send_killable(rt_mq_t mq, const void *buffer, rt_size_t size);
/** Copy and send, waiting up to @p timeout ticks for a free node. */
rt_err_t rt_mq_send_wait(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout);
/** Waiting send that ordinary signals may interrupt. */
rt_err_t rt_mq_send_wait_interruptible(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout);
/** Waiting send that only kill-class signals may interrupt. */
rt_err_t rt_mq_send_wait_killable(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout);
/** Enqueue a copied message ahead of normal FIFO messages. */
rt_err_t rt_mq_urgent(rt_mq_t mq, const void *buffer, rt_size_t size);
/** Receive into @p buffer and return copied bytes or a negative error. */
rt_ssize_t rt_mq_recv(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout);
/** Receive with a wait that ordinary signals may interrupt. */
rt_ssize_t rt_mq_recv_interruptible(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout);
/** Receive with a wait that only kill-class signals may interrupt. */
rt_ssize_t rt_mq_recv_killable(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout);
/** Execute a generic RT_IPC_CMD_* operation on the queue. */
rt_err_t rt_mq_control(rt_mq_t mq, int cmd, void *arg);

#ifdef RT_USING_MESSAGEQUEUE_PRIORITY
/** Enqueue with explicit message @p prio and configurable suspend policy. */
rt_err_t rt_mq_send_wait_prio(rt_mq_t mq,
                              const void *buffer,
                              rt_size_t size,
                              rt_int32_t prio,
                              rt_int32_t timeout,
                              int suspend_flag);
/** Receive the highest-ordered message and optionally return its priority. */
rt_ssize_t rt_mq_recv_prio(rt_mq_t mq,
                           void *buffer,
                           rt_size_t size,
                           rt_int32_t *prio,
                           rt_int32_t timeout,
                           int suspend_flag);
#endif /* RT_USING_MESSAGEQUEUE_PRIORITY */
#endif /* RT_USING_MESSAGEQUEUE */

/**@}*/

/** @name Deferred thread reclamation
 *
 * A thread cannot free the stack on which its exit path is still running.
 * Closed threads are therefore queued and reclaimed later by idle (typical UP)
 * or the system defunct thread (SMP/RT-Smart).
 * @{ */
/** Initialize the defunct queue and, when required, its cleanup thread/semaphore. */
void rt_thread_defunct_init(void);
/** Queue a fully stopped thread for deferred cleanup. */
void rt_thread_defunct_enqueue(rt_thread_t thread);
/** Remove one thread from the defunct queue, or return NULL when empty. */
rt_thread_t rt_thread_defunct_dequeue(void);
/** Execute callbacks and detach/free every reclaimable defunct thread. */
void rt_defunct_execute(void);
/** @} */

/** @name Spinlock operations
 *
 * Spinlocks protect short non-sleeping critical regions.  They do not permit
 * blocking while held.  irqsave variants additionally disable local interrupts
 * and return the exact previous interrupt level that must be supplied to the
 * matching restore call.  Lock/unlock pairs must be balanced on the same CPU.
 * @{ */
struct rt_spinlock;

/**
 * Initialize the underlying SMP hardware lock.  The common function does not
 * initialize optional debug owner/PC metadata, and the UP implementation is a
 * no-op; zero/static initialization remains important for those fields.
 */
void rt_spin_lock_init(struct rt_spinlock *lock);
/**
 * Enter scheduler-critical state and acquire the hardware lock on SMP.  On UP
 * there is no spinning or hardware lock: the call only enters a scheduler
 * critical section.  It does not save/restore raw interrupt state.
 */
void rt_spin_lock(struct rt_spinlock *lock);
/** Release a lock acquired by rt_spin_lock(). */
void rt_spin_unlock(struct rt_spinlock *lock);
/** Disable local interrupts, acquire the lock, and return prior interrupt state. */
rt_base_t rt_spin_lock_irqsave(struct rt_spinlock *lock);
/** Release the lock and restore exactly the interrupt state in @p level. */
void rt_spin_unlock_irqrestore(struct rt_spinlock *lock, rt_base_t level);
/** @} */

/**@}*/

#ifdef RT_USING_DEVICE
/**
 * @addtogroup group_device_driver
 * @{
 */

/** @name Generic device object API
 *
 * The wrappers validate state and dispatch to the driver's rt_device_ops (or
 * legacy direct callbacks).  read/write counts and positions are expressed in
 * device-class-specific units.  Callback setters install borrowed function
 * pointers and the driver may call them from interrupt context.
 * @{ */
/**
 * Find a global-registry device after RT_NAME_MAX truncation; returns a
 * borrowed pointer and must be called from thread context.
 */
rt_device_t rt_device_find(const char *name);

/** Register a caller-owned device with capability/state @p flags. */
rt_err_t rt_device_register(rt_device_t dev,
                            const char *name,
                            rt_uint16_t flags);
/** Remove a device from object discovery; caller remains responsible for storage. */
rt_err_t rt_device_unregister(rt_device_t dev);

#ifdef RT_USING_HEAP
/** Allocate a base device plus @p attach_size bytes for driver-private extension. */
rt_device_t rt_device_create(int type, int attach_size);
/**
 * Release storage allocated by rt_device_create().  The device must first be
 * unregistered and satisfy the device core's destroyed-object preconditions;
 * this is not a substitute for rt_device_unregister().
 */
void rt_device_destroy(rt_device_t device);
#endif /* RT_USING_HEAP */

/** Install the upper-layer receive-ready callback. */
rt_err_t
rt_device_set_rx_indicate(rt_device_t dev,
                          rt_err_t (*rx_ind)(rt_device_t dev, rt_size_t size));
/** Install the upper-layer asynchronous transmit-complete callback. */
rt_err_t
rt_device_set_tx_complete(rt_device_t dev,
                          rt_err_t (*tx_done)(rt_device_t dev, void *buffer));

/** Ensure the device's driver initialization operation has run. */
rt_err_t  rt_device_init (rt_device_t dev);
/**
 * Open with @p oflag, handling lazy initialization and reference counting.
 * A driver result of -RT_ENOSYS is returned unchanged but is treated as an
 * opened reference: the core sets OPEN and increments ref_count.
 */
rt_err_t  rt_device_open (rt_device_t dev, rt_uint16_t oflag);
/** Drop an open reference and call the driver close operation when appropriate. */
rt_err_t  rt_device_close(rt_device_t dev);
/**
 * Dispatch a read and return the driver's result.  Core-level closed-device or
 * missing-operation failures return zero and place the reason in errno; an
 * individual driver may use a negative return by its own contract.
 */
rt_ssize_t rt_device_read(rt_device_t dev,
                          rt_off_t    pos,
                          void       *buffer,
                          rt_size_t   size);
/**
 * Dispatch a write and return the driver's result.  Core-level closed-device
 * or missing-operation failures return zero and set errno; driver-specific
 * contracts may additionally return a negative status.
 */
rt_ssize_t rt_device_write(rt_device_t dev,
                          rt_off_t    pos,
                          const void *buffer,
                          rt_size_t   size);
/** Dispatch a generic/class-specific command whose argument type depends on @p cmd. */
rt_err_t  rt_device_control(rt_device_t dev, int cmd, void *arg);
/** @} */

/**@}*/
#endif /* RT_USING_DEVICE */

/** @name Interrupt and per-CPU integration services
 *
 * rt_interrupt_enter()/leave() are called by BSP/libcpu ISR wrappers, not by
 * application code.  They update nesting and hooks; the architecture's
 * exception-return path remains responsible for completing a deferred context
 * switch.  Every enter must be paired with one leave on the same CPU.
 * @{ */
/** Increment the current CPU's interrupt nesting count and run the enter hook. */
void rt_interrupt_enter(void);
/** Run the leave hook and decrement interrupt nesting before exception return. */
void rt_interrupt_leave(void);

/**
 * Push an exception-frame descriptor when ARCH_USING_IRQ_CTX_LIST is enabled.
 * Without that configured implementation these unconditional declarations do
 * not by themselves provide linkable services.
 */
void rt_interrupt_context_push(rt_interrupt_context_t this_ctx);
/** Pop the most recently pushed context; a matching nonempty stack is required. */
void rt_interrupt_context_pop(void);
/** Return the top frame pointer; call only while the configured list is nonempty. */
void *rt_interrupt_context_get(void);

/** Return the current CPU's bookkeeping object. */
struct rt_cpu *rt_cpu_self(void);
/**
 * Return CPU object @p index.  The caller must pass `0 <= index < RT_CPUS_NR`;
 * the SMP implementation performs no bounds check (the UP form returns NULL
 * for an index other than zero).
 */
struct rt_cpu *rt_cpu_index(int index);

#ifdef RT_USING_SMP

/** Enter the legacy all-CPU scheduling lock and return restore state. */
rt_base_t rt_cpus_lock(void);
/** Leave the all-CPU lock and restore state captured by rt_cpus_lock(). */
void rt_cpus_unlock(rt_base_t level);
/**
 * Complete post-context-switch SMP scheduler bookkeeping; under RT-Smart MMU,
 * also switch to @p thread's address space before the scheduler post hook.
 */
void rt_cpus_lock_status_restore(struct rt_thread *thread);

#ifdef RT_USING_DEBUG
    rt_base_t rt_cpu_get_id(void);
#else /* !RT_USING_DEBUG */
    #define rt_cpu_get_id rt_hw_cpu_id
#endif /* RT_USING_DEBUG */

#else /* !RT_USING_SMP */
#define rt_cpu_get_id()  (0)

#endif /* RT_USING_SMP */

/** Return the current CPU's interrupt nesting depth. */
rt_uint8_t rt_interrupt_get_nest(void);

#ifdef RT_USING_HOOK
/**
 * Install interrupt-boundary hooks.
 *
 * Enter is called after entering RT-Thread interrupt accounting; leave is
 * called while still on the interrupt exit path.  Both run in ISR context and
 * must be bounded, nonblocking, allocation-free, and safe at arbitrary nesting.
 */
void rt_interrupt_enter_sethook(void (*hook)(void));
void rt_interrupt_leave_sethook(void (*hook)(void));
#endif /* RT_USING_HOOK */

#ifdef RT_USING_COMPONENTS_INIT
/** Execute non-board INIT_PREV through INIT_APP/FS exported initializers. */
void rt_components_init(void);
/** Execute board-stage INIT_BOARD/CORE/SUBSYS/PLATFORM initializers. */
void rt_components_board_init(void);
#endif /* RT_USING_COMPONENTS_INIT */
/** @} */

/**
 * @addtogroup group_kernel_service
 * @{
 */

/** @name Console output
 *
 * When console support is absent, output macros compile away and arguments are
 * not evaluated.  Console output is diagnostic, may be serialized when
 * RT_USING_THREADSAFE_PRINTF is enabled, and must not be assumed safe from every
 * ISR or fault context unless the selected console driver explicitly is.
 * @{ */
#ifndef RT_USING_CONSOLE
/** Compile out formatted kernel console output. */
#define rt_kprintf(...)
/** Compile out unformatted kernel console output. */
#define rt_kputs(str)
#else
/** Format and write a message to the current kernel console device. */
int rt_kprintf(const char *fmt, ...);
/** Write a NUL-terminated string to the kernel console without formatting. */
void rt_kputs(const char *str);
#ifdef RT_USING_CONSOLE_OUTPUT_CTL
/** Globally enable or suppress normal console output. */
void rt_console_output_set_enabled(rt_bool_t enabled);
/** Return whether normal console output is currently enabled. */
rt_bool_t rt_console_output_get_enabled(void);
#else
#define rt_console_output_set_enabled(enabled) ((void)0)
#define rt_console_output_get_enabled()        (RT_TRUE)
#endif /* RT_USING_CONSOLE_OUTPUT_CTL */
#endif /* RT_USING_CONSOLE */
/** @} */

/** @name Backtrace services
 *
 * Architecture support determines which frames can be unwound.  Buffer APIs
 * store raw return/program-counter addresses; symbolization/formatting may be a
 * separate step.  Backtracing a running remote thread requires scheduler and
 * architecture synchronization beyond a simple borrowed TCB pointer.
 * @{ */
/** Print or otherwise report a backtrace of the current context. */
rt_err_t rt_backtrace(void);
/** Report a backtrace beginning at @p thread's saved context. */
rt_err_t rt_backtrace_thread(rt_thread_t thread);
/** Continue a thread backtrace from an explicit architecture frame. */
rt_err_t rt_backtrace_frame(rt_thread_t thread, struct rt_hw_backtrace_frame *frame);
/** Print @p buflen raw addresses already stored in @p buffer. */
rt_err_t rt_backtrace_formatted_print(rt_ubase_t *buffer, long buflen);
/** Unwind into @p buffer after skipping @p skip frames; return status per port. */
rt_err_t rt_backtrace_to_buffer(rt_thread_t thread, struct rt_hw_backtrace_frame *frame,
                                long skip, rt_ubase_t *buffer, long buflen);
/** @} */

#if defined(RT_USING_DEVICE) && defined(RT_USING_CONSOLE)
/** Select a registered device by name as console and return the previous console. */
rt_device_t rt_console_set_device(const char *name);
/** Return the currently selected console device. */
rt_device_t rt_console_get_device(void);
#ifdef RT_USING_THREADSAFE_PRINTF
    /** Return the thread currently holding serialized console output, if any. */
    rt_thread_t rt_console_current_user(void);
#else
    rt_inline void *rt_console_current_user(void) { return RT_NULL; }
#endif /* RT_USING_THREADSAFE_PRINTF */
#endif /* defined(RT_USING_DEVICE) && defined(RT_USING_CONSOLE) */

/** Return the one-based index of the most-significant set bit, or zero for zero. */
int __rt_fls(int val);
/** Return the one-based index of the least-significant set bit, or zero for zero. */
int __rt_ffs(int value);
/** Unsigned-long least-significant-set-bit helper. */
unsigned long __rt_ffsl(unsigned long value);
/** Count leading zero bits in an unsigned long according to the implementation contract. */
unsigned long __rt_clz(unsigned long value);

/** Print the RT-Thread version banner to the configured console. */
void rt_show_version(void);

#ifdef RT_DEBUGING_ASSERT
/** Optional observer called by rt_assert_handler() with expression and source location. */
extern void (*rt_assert_hook)(const char *ex, const char *func, rt_size_t line);
/** Install or clear the assertion observer. */
void rt_assert_set_hook(void (*hook)(const char *ex, const char *func, rt_size_t line));
/**
 * Assertion policy used by RT_ASSERT().  The default prints a backtrace and
 * halts (or exits an asserting module); an installed assertion hook replaces
 * that default path and may choose whether control returns.
 */
void rt_assert_handler(const char *ex, const char *func, rt_size_t line);

/**
 * Evaluate @p EX once and route failure text/location to rt_assert_handler().
 * The disabled form also evaluates EX once through RT_UNUSED but discards its
 * value.  Keep state-changing expressions out of assertions, and use the macro
 * as a braced/standalone statement because the enabled expansion is a bare `if`.
 */
#define RT_ASSERT(EX)                                                         \
if (!(EX))                                                                    \
{                                                                             \
    rt_assert_handler(#EX, __FUNCTION__, __LINE__);                           \
}
#else
#define RT_ASSERT(EX) {RT_UNUSED(EX);}
#endif /* RT_DEBUGING_ASSERT */

#ifdef RT_DEBUGING_CONTEXT
/**
 * Assert that the caller is not executing in interrupt context.
 * This is a debug-only contract check and compiles away otherwise.
 */
#define RT_DEBUG_NOT_IN_INTERRUPT                                             \
do                                                                            \
{                                                                             \
    if (rt_interrupt_get_nest() != 0)                                         \
    {                                                                         \
        rt_kprintf("Function[%s] shall not be used in ISR\n", __FUNCTION__);  \
        RT_ASSERT(0)                                                          \
    }                                                                         \
}                                                                             \
while (0)

/* "In thread context" means:
 *     1) the scheduler has been started
 *     2) not in interrupt context.
 */
#define RT_DEBUG_IN_THREAD_CONTEXT                                            \
do                                                                            \
{                                                                             \
    if (rt_thread_self() == RT_NULL)                                          \
    {                                                                         \
        rt_kprintf("Function[%s] shall not be used before scheduler start\n", \
                   __FUNCTION__);                                             \
        RT_ASSERT(0)                                                          \
    }                                                                         \
    RT_DEBUG_NOT_IN_INTERRUPT;                                                \
}                                                                             \
while (0)

#if defined(RT_USING_SMP)
/**
 * @brief Check whether disabled interrupts make scheduler unavailable.
 *
 * In SMP builds, some kernel-internal lockless wait paths may disable local
 * interrupts while still using scheduler-related operations legally. Keep this
 * IRQ-disabled context assertion for UP builds only.
 */
#define RT_DEBUG_SCHEDULER_IRQ_DISABLED() (RT_FALSE)
#else
/**
 * @brief Check whether disabled interrupts make scheduler unavailable.
 *
 * In UP builds, globally disabled interrupts prevent normal scheduling and
 * timeout progress, so blocking scheduler paths must reject this context.
 */
#define RT_DEBUG_SCHEDULER_IRQ_DISABLED() rt_hw_interrupt_is_disabled()
#endif /* defined(RT_USING_SMP) */

/* "scheduler available" means:
 *     1) the scheduler has been started.
 *     2) not in interrupt context.
 *     3) scheduler is not locked.
 *     4) interrupts are not disabled on UP.
 */
#define RT_DEBUG_SCHEDULER_AVAILABLE(need_check)                              \
do                                                                            \
{                                                                             \
    if (need_check)                                                           \
    {                                                                         \
        if ((rt_critical_level() != 0) || RT_DEBUG_SCHEDULER_IRQ_DISABLED())  \
        {                                                                     \
            rt_kprintf("Function[%s]: scheduler is not available\n",          \
                    __FUNCTION__);                                            \
            RT_ASSERT(0)                                                      \
        }                                                                     \
        RT_DEBUG_IN_THREAD_CONTEXT;                                           \
    }                                                                         \
}                                                                             \
while (0)
#else
#define RT_DEBUG_NOT_IN_INTERRUPT
#define RT_DEBUG_IN_THREAD_CONTEXT
#define RT_DEBUG_SCHEDULER_AVAILABLE(need_check)
#endif /* RT_DEBUGING_CONTEXT */

/** Return true after scheduler start when executing outside interrupt context. */
rt_inline rt_bool_t rt_in_thread_context(void)
{
    return rt_thread_self() != RT_NULL && rt_interrupt_get_nest() == 0;
}

/**
 * Test the scheduler's public availability predicate: critical nesting must be
 * zero and execution must be in thread context.  This helper does not perform
 * a separate raw-interrupt-mask test.
 */
rt_inline rt_bool_t rt_scheduler_is_available(void)
{
    return rt_critical_level() == 0 && rt_in_thread_context();
}

#ifdef RT_USING_SMP
/**
 * Return true when @p thread is bound to a CPU.
 * Passing NULL queries the current thread.  Before scheduler start a missing
 * current thread is treated as acceptable/true by the compatibility logic.
 */
rt_inline rt_bool_t rt_sched_thread_is_binding(rt_thread_t thread)
{
    if (thread == RT_NULL)
    {
        thread = rt_thread_self();
    }
    return !thread || RT_SCHED_CTX(thread).bind_cpu != RT_CPUS_NR;
}

#else
#define rt_sched_thread_is_binding(thread) (RT_TRUE)
#endif

/**@}*/

#ifdef __cplusplus
}
#endif

#endif /* __RT_THREAD_H__ */
