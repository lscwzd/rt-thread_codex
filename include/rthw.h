/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-18     Bernard      the first version
 * 2006-04-25     Bernard      add rt_hw_context_switch_interrupt declaration
 * 2006-09-24     Bernard      add rt_hw_context_switch_to declaration
 * 2012-12-29     Bernard      add rt_hw_exception_install declaration
 * 2017-10-17     Hichard      add some macros
 * 2018-11-17     Jesven       add rt_hw_spinlock_t
 *                             add smp support
 * 2019-05-18     Bernard      add empty definition for not enable cache case
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2023-10-16     Shell        Support a new backtrace framework
 */

#ifndef __RT_HW_H__
#define __RT_HW_H__

/**
 * @file rthw.h
 * @brief Contract between the architecture/BSP layer and the RT-Thread kernel.
 *
 * This header collects operations whose implementation depends on the CPU,
 * interrupt controller, cache hierarchy, exception model, or board console.
 * Kernel code calls these interfaces without knowing the underlying machine;
 * the selected CPU port or BSP provides the applicable implementation.
 *
 * These APIs span early-boot, thread, interrupt, and exception contexts.
 * Callers must therefore obey the context and synchronization rules documented
 * for each group. Unless explicitly stated otherwise, address validity,
 * cache-line alignment, privilege, and inter-CPU synchronization remain the
 * caller's responsibility.
 */

#include <rtdef.h>

#if defined (RT_USING_CACHE) || defined(RT_USING_SMP) || defined(RT_HW_INCLUDE_CPUPORT)
/*
 * cpuport.h supplies architecture-native types and primitives such as
 * rt_hw_spinlock_t, cache barriers, and CPU-specific helpers. It is included
 * only when a selected feature needs that architecture contract.
 */
#include <cpuport.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name Memory-mapped register accessors
 *
 * Each macro converts an integer address to a pointer of the requested width,
 * applies volatile semantics, and dereferences it. The result is an lvalue, so
 * both reads and writes are possible, for example
 * `HWREG32(base + offset) = value`.
 *
 * Volatile prevents the compiler from removing or coalescing the individual
 * access, but it is not a CPU memory barrier and provides no inter-CPU
 * synchronization. The caller must guarantee a valid, suitably aligned
 * address and a device that supports the chosen access width. A BSP may
 * override these macros before including this header.
 * @{
 */
#ifndef HWREG64
#define HWREG64(x)          (*((volatile rt_uint64_t *)(x)))
#endif
#ifndef HWREG32
#define HWREG32(x)          (*((volatile rt_uint32_t *)(x)))
#endif
#ifndef HWREG16
#define HWREG16(x)          (*((volatile rt_uint16_t *)(x)))
#endif
#ifndef HWREG8
#define HWREG8(x)           (*((volatile rt_uint8_t *)(x)))
#endif
/** @} */

/**
 * Default cache-line-size hint for ports or callers that choose to use this
 * macro for alignment and cache-maintenance calculations.  The current common
 * tree does not consume it automatically.  A user of the hint must override
 * it when the actual coherency granule is not 32 bytes.
 */
#ifndef RT_CPU_CACHE_LINE_SZ
#define RT_CPU_CACHE_LINE_SZ    32
#endif

/**
 * Operation selector passed to instruction/data cache maintenance APIs.
 * Values occupy independent bits, but support for a combined flush/invalidate
 * request is port-specific; callers should not assume every port accepts it.
 */
enum RT_HW_CACHE_OPS
{
    /** Write back dirty cache lines so later observers see current memory. */
    RT_HW_CACHE_FLUSH      = 0x01,
    /** Discard cached lines so subsequent reads fetch current memory. */
    RT_HW_CACHE_INVALIDATE = 0x02,
};

/**
 * @name CPU cache interfaces
 *
 * Enable, disable, and status operations are architecture services. Range
 * operations apply @p ops to the byte interval starting at @p addr and
 * extending for @p size bytes; ports commonly round the interval to complete
 * cache lines. Callers coordinating with DMA must choose the operation required
 * by the transfer direction and arrange any required memory barriers.
 *
 * When RT_USING_CACHE is disabled, maintenance calls become no-op macros and
 * status reads return zero. Arguments to a no-op macro are not evaluated.
 * @{
 */
#ifdef RT_USING_CACHE

#ifdef RT_USING_SMART
#include <cache.h>
#endif

/** Enable the instruction cache using the CPU port's required sequencing. */
void rt_hw_cpu_icache_enable(void);
/** Disable the instruction cache, performing any port-required maintenance. */
void rt_hw_cpu_icache_disable(void);
/**
 * Query instruction-cache enable state as implemented by the CPU port.
 * Some existing ports provide a placeholder that always returns zero, so this
 * must not be used as a cross-architecture proof that the cache is disabled.
 */
rt_base_t rt_hw_cpu_icache_status(void);
/** Apply @p ops to instructions cached for range [@p addr, @p addr + @p size). */
void rt_hw_cpu_icache_ops(int ops, void* addr, int size);

/** Enable the data cache using the CPU port's required sequencing. */
void rt_hw_cpu_dcache_enable(void);
/** Disable the data cache, performing any port-required writeback/maintenance. */
void rt_hw_cpu_dcache_disable(void);
/**
 * Query data-cache enable state as implemented by the CPU port.  Some ports
 * currently return zero unconditionally even when maintenance operations are
 * implemented; interpret the result only under that port's documented contract.
 */
rt_base_t rt_hw_cpu_dcache_status(void);
/** Apply @p ops to data cached for range [@p addr, @p addr + @p size). */
void rt_hw_cpu_dcache_ops(int ops, void* addr, int size);
#else

/* Cacheless build: preserve the API at zero run-time and evaluation cost. */
#define rt_hw_cpu_icache_enable(...)
#define rt_hw_cpu_icache_disable(...)
#define rt_hw_cpu_icache_ops(...)
#define rt_hw_cpu_dcache_enable(...)
#define rt_hw_cpu_dcache_disable(...)
#define rt_hw_cpu_dcache_ops(...)

#define rt_hw_cpu_icache_status(...) 0
#define rt_hw_cpu_dcache_status(...) 0

#endif
/** @} */

/**
 * @brief Reset the processor or complete platform.
 *
 * This is normally a BSP/SoC service and commonly does not return. It may be
 * called from recovery code, so an implementation should not require ordinary
 * thread scheduling to remain operational.
 */
void rt_hw_cpu_reset(void);

/**
 * @brief Put the processor or platform into its shutdown state.
 *
 * The exact behavior is board-specific: power-off, firmware handoff, or a
 * permanent low-power loop are all possible. Implementations normally do not
 * return when the hardware supports shutdown.
 */
void rt_hw_cpu_shutdown(void);

/**
 * @brief Return a textual description of the active CPU architecture.
 * @return Pointer to a persistent, read-only string owned by the CPU port.
 */
const char *rt_hw_cpu_arch(void);

/**
 * @brief Construct the initial saved context on a new thread's stack.
 *
 * The scheduler calls this while initializing a thread. The port lays out a
 * synthetic exception/context frame so the first restore starts at @p entry
 * with @p parameter. If the entry function returns, its saved return address
 * must transfer control to @p exit.
 *
 * @param entry Thread entry address, kept untyped for assembly-compatible ABI.
 * @param parameter Argument to present to the thread entry function.
 * @param stack_addr Initial stack address selected by generic thread code; the
 *                   precise top/bottom convention follows the CPU port.
 * @param exit Cleanup trampoline invoked if the entry function returns.
 * @return Saved stack pointer stored in the thread control block and consumed
 *         by the corresponding context-switch implementation.
 *
 * @note Stack alignment, register ordering, status-register contents, and
 *       privilege state form an ABI shared with the port's assembly code.
 */
rt_uint8_t *rt_hw_stack_init(void       *entry,
                             void       *parameter,
                             rt_uint8_t *stack_addr,
                             void       *exit);

#ifdef RT_USING_HW_STACK_GUARD
/**
 * @brief Program the architecture's hardware stack guard for @p thread.
 *
 * An implementation may configure an MPU region, limit register, or another
 * hardware overflow-detection mechanism. The supplied thread must already
 * have a valid stack range.
 */
void rt_hw_stack_guard_init(rt_thread_t thread);
#endif

/**
 * Interrupt service routine signature used by the generic interrupt API.
 *
 * @param vector Interrupt/vector number delivered by the controller.
 * @param param Opaque argument registered with rt_hw_interrupt_install().
 *
 * The callback runs in interrupt context. It must use interrupt-safe APIs,
 * avoid blocking, and keep execution bounded. Architecture entry/exit code is
 * responsible for RT-Thread's interrupt nesting protocol.
 */
typedef void (*rt_isr_handler_t)(int vector, void *param);

/** Descriptor maintained by an interrupt-controller implementation. */
struct rt_irq_desc
{
    rt_isr_handler_t handler; /**< Installed ISR, or the port's default handler. */
    void            *param;   /**< Opaque value passed to @ref handler. */

#ifdef RT_USING_INTERRUPT_INFO
    char             name[RT_NAME_MAX]; /**< Diagnostic vector name. */
    rt_uint32_t      counter;            /**< Aggregate dispatch count. */
#ifdef RT_USING_SMP
    /** Per-CPU dispatch counts used to diagnose interrupt affinity/load. */
    rt_ubase_t       cpu_counter[RT_CPUS_NR];
#endif
#endif
};

/**
 * @name Interrupt-controller interfaces
 *
 * The BSP initializes its vector table/controller, controls individual vector
 * delivery, and records handlers through this group. Vector numbering and
 * invalid-vector behavior are controller-specific.
 * @{
 */
/** Initialize the interrupt subsystem before drivers install their ISRs. */
void rt_hw_interrupt_init(void);

/** Prevent delivery of @p vector at the interrupt controller. */
void rt_hw_interrupt_mask(int vector);

/** Permit delivery of @p vector at the interrupt controller. */
void rt_hw_interrupt_umask(int vector);

/**
 * @brief Install @p handler and opaque @p param for @p vector.
 * @param vector Interrupt number understood by the active controller.
 * @param handler Interrupt-context callback to associate with the vector.
 * @param param Opaque callback argument; its storage must outlive registration.
 * @param name Diagnostic label used when interrupt statistics are enabled.
 * @return Previously installed handler according to the port convention,
 *         commonly RT_NULL when no user handler was present.
 * @note Installing a handler does not necessarily unmask its interrupt source.
 */
rt_isr_handler_t rt_hw_interrupt_install(int              vector,
                                         rt_isr_handler_t handler,
                                         void            *param,
                                         const char      *name);

/**
 * @brief Remove a matching interrupt registration.
 *
 * Both @p handler and @p param identify the registration in implementations
 * that support checked or shared uninstall. Before reclaiming callback data,
 * the caller must mask the source and synchronize with in-flight handlers as
 * required by the selected interrupt controller.
 *
 * @param vector Interrupt number whose registration is being removed.
 * @param handler Previously registered callback, used when the port verifies it.
 * @param param Previously registered opaque argument or shared-IRQ identity.
 */
void rt_hw_interrupt_uninstall(int              vector,
                               rt_isr_handler_t handler,
                               void            *param);
/** @} */

#ifdef RT_USING_SMP
/**
 * @brief Disable interrupts only on the calling CPU.
 * @return Previous local hardware interrupt state. Pass this exact value to
 *         rt_hw_local_irq_enable() to preserve nesting and prior mask state.
 */
rt_base_t rt_hw_local_irq_disable(void);

/** Restore the calling CPU's interrupt state saved by local_irq_disable(). */
void rt_hw_local_irq_enable(rt_base_t level);

/**
 * @brief Enter the SMP-wide CPU critical section.
 * @return Prior local interrupt state for rt_cpus_unlock().
 *
 * With a current thread, the common implementation combines the global CPU
 * spinlock (nested per thread) with local interrupt/scheduler exclusion.  In
 * early boot or another context with no current thread, it only disables local
 * interrupts and does not acquire the global spinlock.  Callers must not assume
 * cross-CPU exclusion in that special context.
 */
rt_base_t rt_cpus_lock(void);

/** Leave the SMP-wide critical section and restore saved interrupt state. */
void rt_cpus_unlock(rt_base_t level);

/* Generic kernel critical sections use the SMP-wide lock in an SMP build. */
#define rt_hw_interrupt_disable rt_cpus_lock
#define rt_hw_interrupt_enable rt_cpus_unlock
#else
/**
 * @brief Disable maskable interrupts on a uniprocessor.
 * @return Previous interrupt state, which must later be restored verbatim.
 *
 * Nested critical sections work by saving each returned state and restoring
 * them in LIFO order. A caller must not replace restoration with an unconditional
 * hardware enable, because interrupts may already have been disabled on entry.
 */
rt_base_t rt_hw_interrupt_disable(void);

/** Restore the interrupt state returned by rt_hw_interrupt_disable(). */
void rt_hw_interrupt_enable(rt_base_t level);

/* On UP, local and generic interrupt exclusion are the same operation. */
#define rt_hw_local_irq_disable rt_hw_interrupt_disable
#define rt_hw_local_irq_enable rt_hw_interrupt_enable

#endif /*RT_USING_SMP*/

/**
 * Query whether maskable interrupts are disabled on the calling CPU.
 *
 * A CPU port should override the weak generic implementation when an accurate
 * query is available.  The weak default returns RT_FALSE, so callers must not
 * treat this routine as a hardware-status guarantee on an unimplemented port.
 *
 * @return Port-reported disabled state, or RT_FALSE from the weak fallback.
 */
rt_bool_t rt_hw_interrupt_is_disabled(void);

/**
 * @name Architecture context-switch interfaces
 *
 * These routines are implemented by the CPU port, often in assembly. @p from
 * and @p to identify locations associated with saved stack pointers rather
 * than ordinary stack values; their exact representation is the ABI shared by
 * scheduler and port. The `*_to` form starts the first thread and has no
 * outgoing context. The `*_interrupt` form requests or performs a switch from
 * interrupt context using the port's immediate or deferred-switch mechanism.
 *
 * Callers must not use these as general thread APIs. Scheduler locking,
 * interrupt nesting, FPU state, address-space switching, and whether a first
 * switch returns are all architecture-sensitive.
 * @{
 */
#ifdef RT_USING_SMP
/* SMP ports receive the incoming TCB for CPU/address-space bookkeeping. */
/** Save the outgoing thread context at @p from and restore @p to in thread context. */
void rt_hw_context_switch(rt_ubase_t from, rt_ubase_t to, struct rt_thread *to_thread);
/** Restore the first runnable context at @p to; there is no outgoing thread. */
void rt_hw_context_switch_to(rt_ubase_t to, struct rt_thread *to_thread);
/**
 * Switch/defer from interrupt context described by @p context, saving @p from
 * and selecting @p to as the incoming saved stack context.
 */
void rt_hw_context_switch_interrupt(void *context, rt_ubase_t from, rt_ubase_t to, struct rt_thread *to_thread);
#else
/** Save the outgoing thread context at @p from and restore @p to in thread context. */
void rt_hw_context_switch(rt_ubase_t from, rt_ubase_t to);
/** Restore the first runnable context at @p to; there is no outgoing thread. */
void rt_hw_context_switch_to(rt_ubase_t to);
/**
 * Switch/defer in interrupt context. TCB arguments let the port inspect state
 * belonging to the outgoing and incoming threads in addition to their saved
 * stack-context locations @p from and @p to.
 */
void rt_hw_context_switch_interrupt(rt_ubase_t from, rt_ubase_t to, rt_thread_t from_thread, rt_thread_t to_thread);
#endif /*RT_USING_SMP*/
/** @} */

/**
 * @brief Minimal machine state needed to walk one stack frame.
 *
 * A port may interpret @ref fp as a frame pointer, stack cursor, or another
 * unwind cookie. Callers must treat both fields as opaque inputs to the next
 * unwind operation.
 */
struct rt_hw_backtrace_frame {
    rt_uintptr_t fp; /**< Architecture-defined frame/unwind cursor. */
    rt_uintptr_t pc; /**< Program counter represented by this frame. */
};

/**
 * @brief Obtain the first unwind frame for @p thread.
 * @param thread Target thread; support for a currently executing thread on
 *               another CPU is architecture-specific.
 * @param frame Output frame initialized on success.
 * @return RT_EOK on success, or a negative error when no frame is available.
 */
rt_err_t rt_hw_backtrace_frame_get(rt_thread_t thread, struct rt_hw_backtrace_frame *frame);

/**
 * @brief Advance @p frame to its caller frame.
 * @return RT_EOK if another frame was produced; a negative error marks the end
 *         of the trace or an invalid/unwindable stack.
 */
rt_err_t rt_hw_backtrace_frame_unwind(rt_thread_t thread, struct rt_hw_backtrace_frame *frame);

/**
 * @brief Write a NUL-terminated string to the BSP's lowest-level console.
 *
 * Kernel formatted output ultimately uses this backend. It may be reached
 * before full device initialization or from diagnostics. Whether it is safe in
 * an ISR and whether concurrent output is serialized are BSP-specific.
 */
void rt_hw_console_output(const char *str);

/**
 * @brief Display memory beginning at machine address @p addr.
 * @param size Requested display length/count; its unit and rounding are defined
 *             by the architecture implementation.
 * @note Intended for low-level diagnostics. The caller is responsible for
 *       address validity, permissions, alignment, and possible access faults.
 */
void rt_hw_show_memory(rt_uint32_t addr, rt_size_t size);

/**
 * @brief Install the architecture exception callback hook.
 *
 * On ports that implement exception-hook dispatch, this registers
 * @p exception_handle for an architecture-specific saved exception context.
 * Whether the callback is retained or invoked, the context representation,
 * interpretation of its rt_err_t result, and RT_NULL behavior are all
 * CPU-port-specific; some ports provide only a compatibility stub.  A callback
 * that is actually dispatched runs in exception context, must not block, and
 * must not retain a pointer to a transient frame.
 */
void rt_hw_exception_install(rt_err_t (*exception_handle)(void *context));

/**
 * @brief Request an approximately @p us microsecond hardware delay.
 *
 * A BSP implementation normally provides a calibrated busy wait suitable for
 * short hardware timing and early boot.  The weak generic fallback does not
 * delay: it logs an unsupported-operation warning and returns.  Code that
 * requires timing correctness must therefore ensure the active BSP overrides
 * this symbol; accuracy and maximum practical interval are BSP-specific.
 */
void rt_hw_us_delay(rt_uint32_t us);

/**
 * @return Logical ID of the calling CPU in the range expected by RT-Thread.
 *         A uniprocessor port normally returns zero.
 */
int rt_hw_cpu_id(void);

#if defined(RT_USING_SMP) || defined(RT_USING_AMP)
/**
 * @brief Send an inter-processor interrupt to selected CPUs.
 * @param ipi_vector Architecture/controller-specific IPI vector number.
 * @param cpu_mask Bit mask of destination logical CPUs; bit N selects CPU N.
 *
 * This operation raises the interrupt only. The associated handler and memory
 * ordering for data published before the IPI belong to the SMP/AMP subsystem
 * and CPU port.
 */
void rt_hw_ipi_send(int ipi_vector, unsigned int cpu_mask);
#endif

#ifdef RT_USING_SMP

/** Initialize a hardware spin lock to its unlocked state. */
void rt_hw_spin_lock_init(rt_hw_spinlock_t *lock);

/**
 * @brief Acquire @p lock, spinning until ownership is obtained.
 *
 * Interrupt-state handling is not implied by this primitive; callers must use
 * the IRQ/scheduler locking protocol required for the protected data. Spin
 * locks are not sleeping locks and must protect only bounded critical sections.
 */
void rt_hw_spin_lock(rt_hw_spinlock_t *lock);

/** Release @p lock and publish protected writes according to the port ABI. */
void rt_hw_spin_unlock(rt_hw_spinlock_t *lock);

/** Global hardware lock used by the generic SMP-wide CPU lock implementation. */
extern rt_hw_spinlock_t _cpus_lock;

/* Constant initializer for locks that exist before run-time initialization. */
#define __RT_HW_SPIN_LOCK_INITIALIZER(lockname) {0}

/** Produce a typed unlocked initializer expression for an SMP hardware lock. */
#define __RT_HW_SPIN_LOCK_UNLOCKED(lockname)    \
    (rt_hw_spinlock_t) __RT_HW_SPIN_LOCK_INITIALIZER(lockname)

/** Define named hardware spin lock @p x with static unlocked initialization. */
#define RT_DEFINE_HW_SPINLOCK(x)  rt_hw_spinlock_t x = __RT_HW_SPIN_LOCK_UNLOCKED(x)

/**
 * @brief Start configured secondary CPUs during SMP initialization.
 *
 * The BSP performs the platform-specific release-from-reset or firmware call
 * for configured secondary CPUs.  Individual bring-up attempts can fail or a
 * platform may leave a CPU offline; only successfully started CPUs enter the
 * RT-Thread secondary-CPU path.
 */
void rt_hw_secondary_cpu_up(void);

/**
 * @brief Execute the architecture idle operation on a secondary CPU.
 *
 * This is normally called from the secondary idle path and is commonly a
 * wait-for-interrupt instruction plus required platform bookkeeping.
 */
void rt_hw_secondary_cpu_idle_exec(void);

#else /* !RT_USING_SMP */

/*
 * UP compatibility form: storage holds the interrupt state saved on lock.
 * It is not an inter-CPU lock, and the same variable must be supplied to the
 * matching unlock so the exact prior interrupt state can be restored.
 */
#define RT_DEFINE_HW_SPINLOCK(x)    rt_ubase_t x

/** Save interrupt state into @p lock and exclude local interrupt concurrency. */
#define rt_hw_spin_lock(lock)     *(lock) = rt_hw_interrupt_disable()
/** Restore the interrupt state previously stored by rt_hw_spin_lock(). */
#define rt_hw_spin_unlock(lock)   rt_hw_interrupt_enable(*(lock))


#endif /* RT_USING_SMP */

#ifndef RT_USING_CACHE
    /*
     * This configuration branch supplies compatibility no-ops so generic code
     * can compile without RT_USING_CACHE.  Cache configuration is not, by
     * itself, proof that the CPU needs no ordering barrier.  A port that needs
     * real ordering must arrange the appropriate architecture definitions and
     * feature configuration instead of relying on these fallbacks.
     */
    /** Compatibility no-op in this configuration; not a hardware ordering guarantee. */
    #define rt_hw_isb()
    /** Compatibility no-op in this configuration; not a hardware ordering guarantee. */
    #define rt_hw_dmb()
    /** Compatibility no-op in this configuration; not a hardware ordering guarantee. */
    #define rt_hw_dsb()
#endif /* RT_USING_CACHE */

#ifdef __cplusplus
}
#endif

#endif
