/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-01-18     Shell        Separate the basic types from rtdef.h
 */

/**
 * @file rttypes.h
 * @brief Fundamental scalar, list, atomic-storage, and spinlock types.
 *
 * This header is the lowest-level type contract shared by the RT-Thread
 * kernel, components, BSPs, and CPU ports.  It deliberately contains data
 * representation rather than kernel services: higher-level headers build
 * object, thread, IPC, timer, and device structures on the types declared
 * here.
 *
 * The selected definitions depend on the build configuration:
 *
 * - `RT_USING_ARCH_DATA_TYPE` lets an architecture provide the fixed-width
 *   integer aliases itself.
 * - `RT_USING_LIBC` uses the C library's exact-width and pointer-width types
 *   when the selected library profile provides them.
 * - `ARCH_CPU_64BIT` makes the native base types 64 bits wide; otherwise
 *   they are 32 bits wide.
 * - `RT_USING_STDC_ATOMIC` stores atomic objects with C11 `_Atomic` types.
 * - `RT_USING_HW_ATOMIC` uses ordinary storage whose accesses are protected
 *   by architecture-specific atomic primitives.
 * - `RT_USING_SMP` gives a spinlock a CPU-port-defined hardware lock; on a
 *   uniprocessor build the lock represents local interrupt/critical state.
 *
 * Keep this header free of assumptions about a particular board.  Its
 * widths and layouts form part of the ABI seen by assembly context-switch
 * code, loadable modules, drivers, and persistent protocol structures.
 */

#ifndef __RT_TYPES_H__
#define __RT_TYPES_H__

/*
 * rtconfig.h is generated from the BSP's .config and supplies all feature
 * and architecture-selection macros used below.  The standard headers
 * provide fixed-width, size, pointer, and variadic argument types.
 */
#include <rtconfig.h>

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#ifndef RT_USING_NANO
/*
 * The Nano profile avoids POSIX system types to keep the dependency surface
 * small.  Standard and Smart builds may use ssize_t, errno values, and the
 * platform signal types exposed by these headers.
 */
#include <sys/types.h>
#include <sys/errno.h>
#if defined(RT_USING_SIGNALS) || defined(RT_USING_SMART)
#include <sys/signal.h>
#endif /* defined(RT_USING_SIGNALS) || defined(RT_USING_SMART) */
#endif /* RT_USING_NANO */

#ifdef __cplusplus
/* Give C-linkage to declarations included through this C-compatible header. */
extern "C" {
#endif

/**
 * @name RT-Thread basic scalar types
 *
 * Public RT-Thread APIs use these aliases instead of compiler-specific C
 * spellings.  Fixed-width aliases describe serialized/register-sized data;
 * base-width aliases describe naturally sized CPU words and pointers.
 * @{
 */

/*
 * Hosted 64-bit Windows and x86-64 builds can identify their native word
 * width directly from compiler predefined macros.  Embedded CPU ports may
 * define ARCH_CPU_64BIT in their generated configuration instead.
 */
#if defined(_WIN64) || defined(__x86_64__)
#ifndef ARCH_CPU_64BIT
#define ARCH_CPU_64BIT
#endif // ARCH_CPU_64BIT
#endif // defined(_WIN64) || defined(__x86_64__)

/** Boolean result type.  RT_TRUE and RT_FALSE are its canonical values. */
typedef int                             rt_bool_t;      /**< Integer Boolean result; use RT_TRUE/RT_FALSE. */

#ifndef RT_USING_ARCH_DATA_TYPE
#ifdef RT_USING_LIBC
/* Use the C library's exact-width types when a full libc is available. */
typedef int8_t                          rt_int8_t;      /**< Exactly 8-bit signed integer. */
typedef int16_t                         rt_int16_t;     /**< Exactly 16-bit signed integer. */
typedef int32_t                         rt_int32_t;     /**< Exactly 32-bit signed integer. */
typedef uint8_t                         rt_uint8_t;     /**< Exactly 8-bit unsigned integer. */
typedef uint16_t                        rt_uint16_t;    /**< Exactly 16-bit unsigned integer. */
typedef uint32_t                        rt_uint32_t;    /**< Exactly 32-bit unsigned integer. */
typedef int64_t                         rt_int64_t;     /**< Exactly 64-bit signed integer. */
typedef uint64_t                        rt_uint64_t;    /**< Exactly 64-bit unsigned integer. */
#else
/*
 * Freestanding fallback used by small targets without libc.  RT-Thread's
 * supported toolchains/ABIs are expected to give char, short, and int the
 * widths stated by the aliases below.
 */
typedef signed   char                   rt_int8_t;      /**< Port-assumed 8-bit signed integer. */
typedef signed   short                  rt_int16_t;     /**< Port-assumed 16-bit signed integer. */
typedef signed   int                    rt_int32_t;     /**< Port-assumed 32-bit signed integer. */
typedef unsigned char                   rt_uint8_t;     /**< Port-assumed 8-bit unsigned integer. */
typedef unsigned short                  rt_uint16_t;    /**< Port-assumed 16-bit unsigned integer. */
typedef unsigned int                    rt_uint32_t;    /**< Port-assumed 32-bit unsigned integer. */
#ifdef ARCH_CPU_64BIT
/* This configured 64-bit port assumes its ABI represents a 64-bit value with long. */
typedef signed long                     rt_int64_t;     /**< Port-assumed 64-bit signed integer. */
typedef unsigned long                   rt_uint64_t;    /**< Port-assumed 64-bit unsigned integer. */
#else
/* 32-bit ABIs use long long for an explicitly 64-bit scalar. */
typedef signed long long                rt_int64_t;     /**< Port-assumed 64-bit signed integer. */
typedef unsigned long long              rt_uint64_t;    /**< Port-assumed 64-bit unsigned integer. */
#endif /* ARCH_CPU_64BIT */
#endif /* RT_USING_LIBC */
#endif /* RT_USING_ARCH_DATA_TYPE */

/*
 * rt_base_t/rt_ubase_t are one native machine word wide.  They are suitable
 * for interrupt-state tokens, register values, pointer casts, bitmaps, and
 * other operations naturally handled by the CPU.  Use fixed-width types
 * instead when an externally visible representation must not vary by ABI.
 */
#ifdef ARCH_CPU_64BIT
typedef rt_int64_t                      rt_base_t;      /**< Signed native CPU-word scalar (64 bits). */
typedef rt_uint64_t                     rt_ubase_t;     /**< Unsigned native CPU-word scalar (64 bits). */
#else
typedef rt_int32_t                      rt_base_t;      /**< Signed native CPU-word scalar (32 bits). */
typedef rt_uint32_t                     rt_ubase_t;     /**< Unsigned native CPU-word scalar (32 bits). */
#endif

#if defined(RT_USING_LIBC) && !defined(RT_USING_NANO)
/* Match the hosted/POSIX ABI when libc supplies size and pointer types. */
typedef size_t                          rt_size_t;      /**< Unsigned object/buffer size compatible with libc. */
typedef ssize_t                         rt_ssize_t;     /**< Signed byte count; negative values can report errors. */
typedef intptr_t                        rt_intptr_t;    /**< Signed integer capable of round-tripping a pointer. */
typedef uintptr_t                       rt_uintptr_t;   /**< Unsigned integer capable of round-tripping a pointer. */
#else
/*
 * Freestanding equivalents preserve the important invariant that sizes and
 * pointer-integer conversions are one native word wide.
 */
typedef rt_ubase_t                      rt_size_t;      /**< Freestanding unsigned object/buffer size. */
typedef rt_base_t                       rt_ssize_t;     /**< Freestanding signed byte count/error carrier. */
typedef rt_base_t                      rt_intptr_t;    /**< Freestanding signed pointer-width integer. */
typedef rt_ubase_t                       rt_uintptr_t;   /**< Freestanding unsigned pointer-width integer. */
#endif /* defined(RT_USING_LIBC) && !defined(RT_USING_NANO) */

/*
 * Semantic aliases used by public APIs.  rt_err_t carries RT_EOK or an error
 * code whose sign is defined by the individual API (both positive and negative
 * RT_E* values occur in kernel paths); rt_tick_t wraps modulo 2^32;
 * rt_flag_t stores option/condition bit sets; rt_dev_t identifies a device;
 * and rt_off_t represents a signed seek or memory offset.
 */
typedef rt_base_t                       rt_err_t;       /**< RT_EOK or a signed API-specific RT-Thread error value. */
typedef rt_uint32_t                     rt_tick_t;      /**< Wrapping system-tick count and tick interval. */
typedef rt_base_t                       rt_flag_t;      /**< Native-width signed option/event flag set. */
typedef rt_ubase_t                      rt_dev_t;       /**< Native-width numeric device identifier. */
typedef rt_base_t                       rt_off_t;       /**< Signed file, device, or memory offset. */

/** @} */

/*
 * C11 atomic syntax is unavailable before C11.  Disable the requested C11
 * backend early so rtatomic.h can select a hardware or interrupt-masked
 * implementation instead of compiling invalid `_Atomic` declarations.
 */
#if defined(RT_USING_STDC_ATOMIC) && __STDC_VERSION__ < 201112L
#undef RT_USING_STDC_ATOMIC
#warning Not using C11 or beyond! Maybe you should change the -std option on your compiler
#endif

/**
 * @name Atomic object storage types
 *
 * These aliases describe the storage operated on by rtatomic.h.  Declaring
 * a variable with an atomic alias does not by itself make every arbitrary C
 * expression atomic: callers must use the `rt_atomic_*` API.  In C11 mode
 * the type system also enforces `_Atomic` access.  Hardware and software
 * backends keep plain integer storage and provide atomicity in their access
 * primitives.
 *
 * C++ is kept on plain storage because the C `_Atomic` syntax and
 * `<stdatomic.h>` interface are not portable C++ contracts across the
 * supported embedded toolchains.
 * @{
 */
#ifdef __cplusplus
    typedef rt_uint8_t rt_atomic8_t;   /**< C++ 8-bit storage used through port synchronization APIs. */
    typedef rt_uint16_t rt_atomic16_t; /**< C++ 16-bit storage used through port synchronization APIs. */
    typedef rt_base_t rt_atomic_t;     /**< C++ native-word storage used through port synchronization APIs. */
#else
    #if defined(RT_USING_STDC_ATOMIC)
        #include <stdatomic.h>
        typedef _Atomic(rt_uint8_t) rt_atomic8_t;   /**< Atomic object whose value type is rt_uint8_t; size/alignment are implementation-defined. */
        typedef _Atomic(rt_uint16_t) rt_atomic16_t; /**< Atomic object whose value type is rt_uint16_t; size/alignment are implementation-defined. */
        typedef _Atomic(rt_base_t) rt_atomic_t;     /**< C11 atomic native-word storage. */
    #elif defined(RT_USING_HW_ATOMIC)
        typedef rt_uint8_t rt_atomic8_t;   /**< 8-bit storage accessed by CPU-port atomic helpers. */
        typedef rt_uint16_t rt_atomic16_t; /**< 16-bit storage accessed by CPU-port atomic helpers. */
        typedef rt_base_t rt_atomic_t;     /**< Native-word storage accessed by CPU-port atomic helpers. */
    #else
        typedef rt_uint8_t rt_atomic8_t;   /**< 8-bit storage protected by software atomic helpers. */
        typedef rt_uint16_t rt_atomic16_t; /**< 16-bit storage protected by software atomic helpers. */
        typedef rt_base_t rt_atomic_t;     /**< Native-word storage protected by software atomic helpers. */
    #endif /* RT_USING_STDC_ATOMIC */
#endif /* __cplusplus */

/** @} */

/* Boolean constants used throughout C APIs; zero is false, one is true. */
#define RT_TRUE                         1               /**< boolean true  */
#define RT_FALSE                        0               /**< boolean false */

/* Portable null-pointer constant retained for C and legacy C++ toolchains. */
#define RT_NULL                         0

/**
 * @brief Intrusive doubly linked-list node.
 *
 * A list node is embedded directly in its owning object rather than
 * allocating a wrapper.  `next` and `prev` connect either neighboring
 * objects or a circular sentinel head.  Container macros recover the owner
 * address from the embedded node, so a single object may participate in
 * several lists by embedding several independent nodes.
 */
struct rt_list_node
{
    struct rt_list_node *next; /**< Next node in traversal order, or the sentinel head. */
    struct rt_list_node *prev; /**< Previous node in traversal order, or the sentinel head. */
};
typedef struct rt_list_node rt_list_t; /**< Public shorthand for an intrusive list node/head. */

/**
 * @brief Intrusive singly linked-list node.
 *
 * This form saves one pointer per node when reverse traversal and constant
 * time removal of an arbitrary node are unnecessary.  Its owner and
 * lifetime rules are the same as for `rt_list_t`.
 */
struct rt_slist_node
{
    struct rt_slist_node *next; /**< Next node, or RT_NULL at the end of a linear list. */
};
typedef struct rt_slist_node rt_slist_t; /**< Public shorthand for a singly linked node/head. */

/**
 * @brief Node/head storage for the atomic singly linked stack.
 *
 * `next` is stored as a native-width atomic integer so it can hold either
 * zero or a node pointer cast to `rt_base_t`.  rtatomic.h uses compare and
 * exchange to push and pop nodes.  The algorithm is lock-free only when the
 * selected atomic backend provides lock-free compare-and-exchange; the
 * software fallback may serialize through interrupt/CPU locking.  Objects must
 * remain allocated while another execution context could still observe their
 * address.  The implementation also does not add an ABA counter, so users must
 * arrange lifetime/reuse rules appropriate to their concurrency model.
 */
struct rt_lockless_slist_node
{
    rt_atomic_t next; /**< Atomic integer representation of the next node pointer. */
};
typedef struct rt_lockless_slist_node rt_ll_slist_t; /**< Atomic singly linked node/head type. */

/**
 * @name Spinlock representation and diagnostic metadata
 *
 * The operational lock/unlock functions are declared by the hardware/kernel
 * interface.  This section only defines their shared object layout and the
 * optional bookkeeping used to diagnose ownership and critical-section
 * nesting mistakes.
 * @{
 */
#ifdef RT_USING_SMP
/* SMP ports define the actual inter-CPU exclusion primitive in cpuport.h. */
#include <cpuport.h> /* for spinlock from arch */

/** Spinlock state used when multiple CPUs can execute concurrently. */
struct rt_spinlock
{
    rt_hw_spinlock_t lock; /**< Architecture-defined hardware atomic lock word/state. */
#ifdef RT_USING_DEBUG
    rt_uint32_t critical_level; /**< Saved critical nesting level for validation on unlock. */
#endif /* RT_USING_DEBUG */
#if defined(RT_DEBUGING_SPINLOCK)
    void *owner; /**< Thread recorded as acquiring the lock; diagnostic only. */
    void *pc;    /**< Call-site return address captured when the lock is acquired. */
#endif /* RT_DEBUGING_SPINLOCK */
};

/*
 * A CPU port may override the initializer when rt_hw_spinlock_t has a layout
 * other than the common nested scalar/aggregate zero initializer.
 */
#ifndef RT_SPINLOCK_INIT
#define RT_SPINLOCK_INIT {{0}} /* can be overridden by cpuport.h */
#endif /* RT_SPINLOCK_INIT */

#else /* !RT_USING_SMP */

/**
 * Uniprocessor spinlock state.
 *
 * There is no competing CPU, so the generic `rt_spin_lock*` implementation
 * obtains exclusion through scheduler-critical and, for irqsave operations,
 * local interrupt state.  `lock` is native-width compatibility/reserved
 * storage rather than an inter-CPU atomic lock word; a UP implementation need
 * not consult it.
 */
struct rt_spinlock
{
#ifdef RT_USING_DEBUG
    rt_uint32_t critical_level; /**< Saved critical nesting level for diagnostics. */
#endif /* RT_USING_DEBUG */
    rt_ubase_t lock; /**< UP-compatible reserved lock storage; not an inter-CPU primitive. */
};
/* Static initializer for the uniprocessor scalar representation. */
#define RT_SPINLOCK_INIT {0}
#endif /* RT_USING_SMP */

/*
 * Optional owner tracking is meaningful only in SMP builds.  The owner and
 * acquisition PC are observations for debugging; they do not provide the
 * exclusion itself and must never be used as synchronization state.
 */
#if defined(RT_DEBUGING_SPINLOCK) && defined(RT_USING_SMP)

    /* Poison value written after release so stale ownership is conspicuous. */
    #define __OWNER_MAGIC ((void *)0xdeadbeaf)

    /* GCC can cheaply expose the immediate caller; other compilers report none. */
    #if defined(__GNUC__)
    #define __GET_RETURN_ADDRESS __builtin_return_address(0)
    #else /* !__GNUC__ */
    #define __GET_RETURN_ADDRESS RT_NULL
    #endif /* __GNUC__ */

    /* Record the current thread and the call site immediately after acquisition. */
    #define _SPIN_LOCK_DEBUG_OWNER(lock)                  \
        do                                                \
        {                                                 \
            struct rt_thread *_curthr = rt_thread_self(); \
            if (_curthr != RT_NULL)                       \
            {                                             \
                (lock)->owner = _curthr;                  \
                (lock)->pc = __GET_RETURN_ADDRESS;        \
            }                                             \
        } while (0)

    /* Mark the lock unowned and clear the previous acquisition call site. */
    #define _SPIN_UNLOCK_DEBUG_OWNER(lock) \
        do                                 \
        {                                  \
            (lock)->owner = __OWNER_MAGIC; \
            (lock)->pc = RT_NULL;          \
        } while (0)

#else /* !RT_DEBUGING_SPINLOCK */

    /* Preserve expression validity and suppress unused-argument diagnostics. */
    #define _SPIN_LOCK_DEBUG_OWNER(lock)    RT_UNUSED(lock)
    #define _SPIN_UNLOCK_DEBUG_OWNER(lock)  RT_UNUSED(lock)
#endif /* RT_DEBUGING_SPINLOCK */

/*
 * Critical-level diagnostics pair the nesting level seen at lock time with
 * the value returned to the unlock path.  This helps detect mismatched lock
 * scope or an unexpected change in critical nesting while the lock is held.
 */
#ifdef RT_DEBUGING_CRITICAL
    #define _SPIN_LOCK_DEBUG_CRITICAL(lock)               \
        do                                                \
        {                                                 \
            (lock)->critical_level = rt_critical_level(); \
        } while (0)

    #define _SPIN_UNLOCK_DEBUG_CRITICAL(lock, critical) \
        do                                              \
        {                                               \
            (critical) = (lock)->critical_level;        \
        } while (0)

#else /* !RT_DEBUGING_CRITICAL */
    /* Disabled builds retain identical call syntax but produce no metadata. */
    #define _SPIN_LOCK_DEBUG_CRITICAL(lock)             RT_UNUSED(lock)
    #define _SPIN_UNLOCK_DEBUG_CRITICAL(lock, critical) do {critical = 0; RT_UNUSED(lock);} while (0)

#endif /* RT_DEBUGING_CRITICAL */

/** Execute all enabled acquisition-side diagnostic bookkeeping. */
#define RT_SPIN_LOCK_DEBUG(lock)         \
    do                                   \
    {                                    \
        _SPIN_LOCK_DEBUG_OWNER(lock);    \
        _SPIN_LOCK_DEBUG_CRITICAL(lock); \
    } while (0)

/** Execute all enabled release-side bookkeeping and recover saved nesting. */
#define RT_SPIN_UNLOCK_DEBUG(lock, critical)         \
    do                                               \
    {                                                \
        _SPIN_UNLOCK_DEBUG_OWNER(lock);              \
        _SPIN_UNLOCK_DEBUG_CRITICAL(lock, critical); \
    } while (0)

/** Public spinlock object type used by kernel data structures. */
typedef struct rt_spinlock rt_spinlock_t;

/** Define and statically initialize a spinlock object named @p x. */
#define RT_DEFINE_SPINLOCK(x)  struct rt_spinlock x = RT_SPINLOCK_INIT

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* __RT_TYPES_H__ */
