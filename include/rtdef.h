/*
 * Copyright (c) 2006-2025 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2007-01-10     Bernard      the first version
 * 2008-07-12     Bernard      remove all rt_int8, rt_uint32_t etc typedef
 * 2010-10-26     yi.qiu       add module support
 * 2010-11-10     Bernard      add cleanup callback function in thread exit.
 * 2011-05-09     Bernard      use builtin va_arg in GCC 4.x
 * 2012-11-16     Bernard      change RT_NULL from ((void*)0) to 0.
 * 2012-12-29     Bernard      change the RT_USING_MEMPOOL location and add
 *                             RT_USING_MEMHEAP condition.
 * 2012-12-30     Bernard      add more control command for graphic.
 * 2013-01-09     Bernard      change version number.
 * 2015-02-01     Bernard      change version number to v2.1.0
 * 2017-08-31     Bernard      change version number to v3.0.0
 * 2017-11-30     Bernard      change version number to v3.0.1
 * 2017-12-27     Bernard      change version number to v3.0.2
 * 2018-02-24     Bernard      change version number to v3.0.3
 * 2018-04-25     Bernard      change version number to v3.0.4
 * 2018-05-31     Bernard      change version number to v3.1.0
 * 2018-09-04     Bernard      change version number to v3.1.1
 * 2018-09-14     Bernard      apply Apache License v2.0 to RT-Thread Kernel
 * 2018-10-13     Bernard      change version number to v4.0.0
 * 2018-10-02     Bernard      add 64bit arch support
 * 2018-11-22     Jesven       add smp member to struct rt_thread
 *                             add struct rt_cpu
 *                             add smp relevant macros
 * 2019-01-27     Bernard      change version number to v4.0.1
 * 2019-05-17     Bernard      change version number to v4.0.2
 * 2019-12-20     Bernard      change version number to v4.0.3
 * 2020-08-10     Meco Man     add macro for struct rt_device_ops
 * 2020-10-23     Meco Man     define maximum value of ipc type
 * 2021-03-19     Meco Man     add security devices
 * 2021-05-10     armink       change version number to v4.0.4
 * 2021-11-19     Meco Man     change version number to v4.1.0
 * 2021-12-21     Meco Man     re-implement RT_UNUSED
 * 2022-01-01     Gabriel      improve hooking method
 * 2022-01-07     Gabriel      move some __on_rt_xxxxx_hook to dedicated c source files
 * 2022-01-12     Meco Man     remove RT_THREAD_BLOCK
 * 2022-04-20     Meco Man     change version number to v4.1.1
 * 2022-04-21     THEWON       add macro RT_VERSION_CHECK
 * 2022-06-29     Meco Man     add RT_USING_LIBC and standard libc headers
 * 2022-08-16     Meco Man     change version number to v5.0.0
 * 2022-09-12     Meco Man     define rt_ssize_t
 * 2022-12-20     Meco Man     add const name for rt_object
 * 2023-04-01     Chushicheng  change version number to v5.0.1
 * 2023-05-20     Bernard      add stdc atomic detection.
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2023-10-10     Chushicheng  change version number to v5.1.0
 * 2023-10-11     zmshahaha    move specific devices related and driver to components/drivers
 * 2023-11-21     Meco Man     add RT_USING_NANO macro
 * 2023-11-17     xqyjlj       add process group and session support
 * 2023-12-01     Shell        Support of dynamic device
 * 2023-12-18     xqyjlj       add rt_always_inline
 * 2023-12-22     Shell        Support hook list
 * 2024-01-18     Shell        Separate basical types to a rttypes.h
 *                             Separate the compiler portings to rtcompiler.h
 * 2024-03-30     Meco Man     update version number to v5.2.0
 * 2025-11-10     Rbb666       update version number to v5.3.0
 */

#ifndef __RT_DEF_H__
#define __RT_DEF_H__

/**
 * @file rtdef.h
 * @brief Core configuration-independent data model shared by the RT-Thread kernel.
 *
 * This header is the central description of the objects manipulated by the
 * kernel.  It contains version encoding, initialization-export metadata,
 * object class identifiers, timer and thread control blocks, IPC objects,
 * memory-manager metadata, and the base device object.  Public operations on
 * these types are declared in rtthread.h; scheduler-private fields are injected
 * through rtsched.h; fixed-width and intrusive-list types come from rttypes.h.
 *
 * Most structures in this file implement C-style inheritance: the first field
 * of a derived object is its parent object.  For example, rt_thread starts with
 * rt_object, while rt_semaphore starts with rt_ipc_object, which itself starts
 * with rt_object.  This layout lets generic object-management code safely cast
 * a derived object to its base type.
 *
 * Many fields are conditionally compiled.  Their presence, and therefore the
 * binary layout of the structures, depends on the target BSP's rtconfig.h.
 * Kernel objects must never be exchanged between binaries built with different
 * configurations.
 */

#include "rtsched.h"
#include "rttypes.h"

#include "klibc/kerrno.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup group_basic_definition
 */

/**@{*/

/*
 * RT-Thread version information.
 *
 * RT_VERSION_CHECK() maps a semantic version X.Y.Z to X * 10000 + Y * 100 + Z.
 * The monotonic integer is intended for preprocessor comparisons; it is not a
 * packed bit field and must not be decoded with shifts or masks.
 */
#define RT_VERSION_MAJOR                5               /**< Major version number (X.x.x) */
#define RT_VERSION_MINOR                3               /**< Minor version number (x.X.x) */
#define RT_VERSION_PATCH                0               /**< Patch version number (x.x.X) */

/* e.g. #if (RTTHREAD_VERSION >= RT_VERSION_CHECK(4, 1, 0) */
#define RT_VERSION_CHECK(major, minor, revise)          ((major * 10000U) + (minor * 100U) + revise)

/* RT-Thread version */
#define RTTHREAD_VERSION                RT_VERSION_CHECK(RT_VERSION_MAJOR, RT_VERSION_MINOR, RT_VERSION_PATCH)

/**@}*/

/*
 * Maximum values used by ABI-visible counters.  A libc-enabled build reuses
 * the standard integer limits; a freestanding build provides equivalent
 * constants without depending on <stdint.h> limit macros.
 */
#ifdef RT_USING_LIBC
#define RT_UINT8_MAX                    UINT8_MAX       /**< Maximum number of UINT8 */
#define RT_UINT16_MAX                   UINT16_MAX      /**< Maximum number of UINT16 */
#define RT_UINT32_MAX                   UINT32_MAX      /**< Maximum number of UINT32 */
#define RT_UINT64_MAX                   UINT64_MAX      /**< Maximum number of UINT64 */
#else
#define RT_UINT8_MAX                    0xFFU                 /**< Maximum number of UINT8 */
#define RT_UINT16_MAX                   0xFFFFU               /**< Maximum number of UINT16 */
#define RT_UINT32_MAX                   0xFFFFFFFFUL          /**< Maximum number of UINT32 */
#define RT_UINT64_MAX                   0xFFFFFFFFFFFFFFFFULL /**< Maximum number of UINT64 */
#endif /* RT_USING_LIBC */

#define RT_TICK_MAX                     RT_UINT32_MAX   /**< Maximum number of tick */

/*
 * Public bounds for IPC counters.  Most correspond to the current field width;
 * RT_MUTEX_VALUE_MAX is retained as a compatibility constant even though the
 * current mutex control block has no `value` field and uses `hold` instead.
 * Runtime APIs may impose stricter limits.
 */
#define RT_SEM_VALUE_MAX                RT_UINT16_MAX   /**< Maximum number of semaphore .value */
#define RT_MUTEX_VALUE_MAX              RT_UINT16_MAX   /**< Legacy mutex-value compatibility bound. */
#define RT_MUTEX_HOLD_MAX               RT_UINT8_MAX    /**< Maximum number of mutex .hold */
#define RT_MB_ENTRY_MAX                 RT_UINT16_MAX   /**< Maximum number of mailbox .entry */
#define RT_MQ_ENTRY_MAX                 RT_UINT16_MAX   /**< Maximum number of message queue .entry */

/* Common utilities. */

/** Explicitly mark an expression as intentionally unused without evaluating it twice. */
#define RT_UNUSED(x)                   ((void)(x))

/**
 * Compile-time assertion usable by pre-C11 compilers.
 *
 * A false expression creates an array with a negative bound and therefore a
 * compilation error.  @p name becomes part of the generated typedef so each
 * assertion in one scope must use a unique identifier.
 */
#define RT_STATIC_ASSERT(name, expn) typedef char _static_assert_##name[(expn)?1:-1]

/* Compiler Related Definitions */
#include "rtcompiler.h"

/**
 * @name Automatic initialization export
 *
 * INIT_EXPORT() places a function pointer, and optionally diagnostic metadata,
 * into a linker section whose suffix is the textual @p level.  The linker
 * script keeps and sorts the .rti_fn.* sections.  Startup code then walks the
 * resulting range in lexical level order.
 *
 * An exported initializer has signature `int fn(void)`.  Board-level entries
 * run before the scheduler starts; the remaining component levels normally run
 * from the main initialization thread.  The macros expand to nothing when
 * RT_USING_COMPONENTS_INIT is disabled, so exporting a function does not by
 * itself guarantee that it is present in a particular firmware image.
 *
 * MSVC cannot use the same ELF-style section attributes as GCC-compatible
 * compilers, so it stores an explicit level string in a common section.  With
 * RT_DEBUGING_AUTO_INIT, the function name is also retained for diagnostics.
 * @{ */
#ifdef RT_USING_COMPONENTS_INIT
/** Prototype required for every automatically exported initializer. */
typedef int (*init_fn_t)(void);
#ifdef _MSC_VER
#pragma section("rti_fn$f",read)
    #ifdef RT_DEBUGING_AUTO_INIT
        struct rt_init_desc
        {
            const char* level;       /**< Textual ordering key, for example ".rti_fn.3". */
            const init_fn_t fn;      /**< Initializer to invoke. */
            const char* fn_name;     /**< Function name retained for startup diagnostics. */
        };
        #define INIT_EXPORT(fn, level)                                  \
                                const char __rti_level_##fn[] = ".rti_fn." level;       \
                                const char __rti_##fn##_name[] = #fn;                   \
                                __declspec(allocate("rti_fn$f"))                        \
                                rt_used const struct rt_init_desc __rt_init_msc_##fn =  \
                                {__rti_level_##fn, fn, __rti_##fn##_name};
    #else
        struct rt_init_desc
        {
            const char* level;       /**< Textual ordering key used by the MSVC startup walker. */
            const init_fn_t fn;      /**< Initializer to invoke. */
        };
        #define INIT_EXPORT(fn, level)                                  \
                                const char __rti_level_##fn[] = ".rti_fn." level;       \
                                __declspec(allocate("rti_fn$f"))                        \
                                rt_used const struct rt_init_desc __rt_init_msc_##fn =  \
                                {__rti_level_##fn, fn };
    #endif /* RT_DEBUGING_AUTO_INIT */
#else
    #ifdef RT_DEBUGING_AUTO_INIT
        struct rt_init_desc
        {
            const char* fn_name;     /**< Function name retained for startup diagnostics. */
            const init_fn_t fn;      /**< Initializer placed in this descriptor's linker section. */
        };
        #define INIT_EXPORT(fn, level)                                                       \
            const char __rti_##fn##_name[] = #fn;                                            \
            rt_used const struct rt_init_desc __rt_init_desc_##fn rt_section(".rti_fn." level) = \
            { __rti_##fn##_name, fn};
    #else
        #define INIT_EXPORT(fn, level)                                                       \
            rt_used const init_fn_t __rt_init_##fn rt_section(".rti_fn." level) = fn
    #endif /* RT_DEBUGING_AUTO_INIT */
#endif /* _MSC_VER */
#else
#define INIT_EXPORT(fn, level)
#endif /* RT_USING_COMPONENTS_INIT */

/* Board-stage routines are called by rt_components_board_init(). */
#define INIT_BOARD_EXPORT(fn)           INIT_EXPORT(fn, "1")

/* Core facilities: CPU, memory, interrupt controller, and fundamental buses. */
#define INIT_CORE_EXPORT(fn)            INIT_EXPORT(fn, "1.0")
/* Subsystems required by later drivers: system timer, clocks, and pin control. */
#define INIT_SUBSYS_EXPORT(fn)          INIT_EXPORT(fn, "1.1")
/* Platform-specific services and other late board-stage code. */
#define INIT_PLATFORM_EXPORT(fn)        INIT_EXPORT(fn, "1.2")

/* The following levels normally run in main_thread_entry(). */
/* Pure-software preparation that does not require initialized devices. */
#define INIT_PREV_EXPORT(fn)            INIT_EXPORT(fn, "2")
/* Device registration and hardware-driver initialization. */
#define INIT_DEVICE_EXPORT(fn)          INIT_EXPORT(fn, "3")
/* Middleware such as DFS and protocol stacks. */
#define INIT_COMPONENT_EXPORT(fn)       INIT_EXPORT(fn, "4")
/* Runtime environment setup, for example mounting storage. */
#define INIT_ENV_EXPORT(fn)             INIT_EXPORT(fn, "5")
/* Application services that depend on the environment. */
#define INIT_APP_EXPORT(fn)             INIT_EXPORT(fn, "6")

/* Initialization that specifically requires a mounted file system. */
#define INIT_FS_EXPORT(fn)              INIT_EXPORT(fn, "6.0")
/*
 * Per-secondary-CPU initialization walked by rt_dm_secondary_cpu_init(); the
 * BSP/architecture secondary-startup path decides when to invoke that walker.
 */
#define INIT_SECONDARY_CPU_EXPORT(fn)   INIT_EXPORT(fn, "7")
/** @} */

#if !defined(RT_USING_FINSH)
/* define these to empty, even if not include finsh.h file */
#define FINSH_FUNCTION_EXPORT(name, desc)
#define FINSH_FUNCTION_EXPORT_ALIAS(name, alias, desc)

#define MSH_CMD_EXPORT(command, desc)
#define MSH_CMD_EXPORT_ALIAS(command, alias, desc)
#elif !defined(FINSH_USING_SYMTAB)
#define FINSH_FUNCTION_EXPORT_CMD(name, cmd, desc)
#endif

/** Number of event bits carried by an rt_event object. */
#define RT_EVENT_LENGTH                 32

/*
 * Default page geometry shared by the slab allocator and virtual-memory/MMU
 * components.  The mask and shift assume 4096 bytes == 1 << 12.
 */
#define RT_MM_PAGE_SIZE                 4096
#define RT_MM_PAGE_MASK                 (RT_MM_PAGE_SIZE - 1)
#define RT_MM_PAGE_BITS                 12

/*
 * Allocation indirection used by kernel object creation.  A port or protected
 * build may override these macros before including this file to route kernel
 * metadata to a dedicated allocator.  The default uses the system heap API.
 */
#ifndef RT_KERNEL_MALLOC
#define RT_KERNEL_MALLOC(sz)            rt_malloc(sz)
#endif /* RT_KERNEL_MALLOC */

#ifndef RT_KERNEL_FREE
#define RT_KERNEL_FREE(ptr)             rt_free(ptr)
#endif /* RT_KERNEL_FREE */

#ifndef RT_KERNEL_REALLOC
#define RT_KERNEL_REALLOC(ptr, size)    rt_realloc(ptr, size)
#endif /* RT_KERNEL_REALLOC */

/**
 * @ingroup group_basic_definition
 *
 * @def RT_IS_ALIGN(addr, align)
 * Return true(1) or false(0).
 *     RT_IS_ALIGN(128, 4) is judging whether 128 aligns with 4.
 *     The result is 1, which means 128 aligns with 4.
 * @note If the address is NULL, false(0) will be returned
 * @note @p align must be a nonzero power of two.  @p addr may be evaluated
 *       twice (the second test can be short-circuited), so pass a
 *       side-effect-free integer/pointer-width expression.
 */
#define RT_IS_ALIGN(addr, align) ((!(addr & (align - 1))) && (addr != RT_NULL))

/**
 * @ingroup group_basic_definition
 *
 * @def RT_ALIGN(size, align)
 * Return the most contiguous size aligned at specified width. RT_ALIGN(13, 4)
 * would return 16.
 * @note align Must be an integer power of 2 or the result will be incorrect
 * @note @p align is expanded more than once; do not pass an expression with
 *       increments, function calls, or other side effects.  Addition can wrap
 *       if @p size is too close to the maximum representable value.
 */
#define RT_ALIGN(size, align)           (((size) + (align) - 1) & ~((align) - 1))

/**
 * @ingroup group_basic_definition
 *
 * @def RT_ALIGN_DOWN(size, align)
 * Return the down number of aligned at specified width. RT_ALIGN_DOWN(13, 4)
 * would return 12.
 * @note align Must be an integer power of 2 or the result will be incorrect
 * @note Use side-effect-free arguments; no run-time validation is performed.
 */
#define RT_ALIGN_DOWN(size, align)      ((size) & ~((align) - 1))

/**
 * @addtogroup group_object_management
 * @{
 */

/* Kernel object flags occupy the rt_object::flag byte. */
#define RT_OBJECT_FLAG_MODULE           0x80            /**< is module object. */

/**
 * @brief Common header embedded at offset zero in every managed kernel object.
 *
 * The object manager normally links an initialized instance into the class
 * container selected by @ref rt_object_class_type.  Under RT_USING_MODULE, an
 * object created by the current loadable module is linked to that module's
 * private object list instead.  The generic rt_object_init() path sets
 * RT_Object_Class_Static in the high bit of `type` and pairs with detach;
 * rt_object_allocate() leaves it clear and pairs with delete.  This attribute
 * identifies the object initialization/lifetime path, not necessarily the
 * backing storage's physical origin, because class wrappers can define their
 * own allocation/destroy sequence.  Code that changes `type` or `list` directly
 * can corrupt registry or lifetime state and must use object/class APIs.
 */
struct rt_object
{
#if RT_NAME_MAX > 0
    char        name[RT_NAME_MAX];                       /**< NUL-terminated name stored inline; long input is truncated. */
#else
    const char *name;                                    /**< Borrowed name pointer; storage must outlive the object. */
#endif /* RT_NAME_MAX > 0 */
    rt_uint8_t  type;                                    /**< Class value plus the static-object ownership bit. */
    rt_uint8_t  flag;                                    /**< Class-specific flags; the module bit is globally reserved. */

#ifdef RT_USING_MODULE
    void      * module_id;                               /**< Owning loadable module, used to reclaim module resources. */
#endif /* RT_USING_MODULE */

#ifdef RT_USING_SMART
    rt_atomic_t lwp_ref_count;                           /**< Atomic references held on behalf of RT-Smart LWPs. */
#endif /* RT_USING_SMART */

    rt_list_t   list;                                    /**< Node in a class-global or module-private object list. */
};
typedef struct rt_object *rt_object_t;                   /**< Type for kernel objects. */

/**
 * @brief Visitor callback used by rt_object_for_each().
 *
 * @param object Current object in the selected class container.
 * @param data Opaque caller context passed through by rt_object_for_each().
 * @return RT_EOK to continue; a positive value to stop successfully; or a
 *         negative RT-Thread error to stop and report failure.
 *
 * rt_object_for_each() invokes the callback while holding the selected class
 * registry spinlock.  The callback must stay bounded, must not block, and must
 * not call an object API that takes the same registry lock or changes the
 * current object's registry membership.
 */
typedef rt_err_t (*rt_object_iter_t)(rt_object_t object, void *data);

/**
 * @brief Runtime class tag stored in rt_object::type.
 *
 * The low seven bits select the logical object class.  The value
 * RT_Object_Class_Static is an ownership attribute ORed into the class value,
 * not an independent object class; callers should mask or use the object APIs
 * when testing a class.  Some enumerators are meaningful only when their
 * corresponding feature is compiled in.
 *
 * The object type can be one of the following with specific macros enabled:
 *  - Thread
 *  - Semaphore
 *  - Mutex
 *  - Event
 *  - MailBox
 *  - MessageQueue
 *  - MemHeap
 *  - MemPool
 *  - Device
 *  - Timer
 *  - Module, Memory, Channel, ProcessGroup, Session, or Custom
 *  - Unknown
 *  - Static ownership attribute (not an independent logical class)
 */
enum rt_object_class_type
{
    RT_Object_Class_Null          = 0x00,      /**< The object is not used. */
    RT_Object_Class_Thread        = 0x01,      /**< The object is a thread. */
    RT_Object_Class_Semaphore     = 0x02,      /**< The object is a semaphore. */
    RT_Object_Class_Mutex         = 0x03,      /**< The object is a mutex. */
    RT_Object_Class_Event         = 0x04,      /**< The object is a event. */
    RT_Object_Class_MailBox       = 0x05,      /**< The object is a mail box. */
    RT_Object_Class_MessageQueue  = 0x06,      /**< The object is a message queue. */
    RT_Object_Class_MemHeap       = 0x07,      /**< The object is a memory heap. */
    RT_Object_Class_MemPool       = 0x08,      /**< The object is a memory pool. */
    RT_Object_Class_Device        = 0x09,      /**< The object is a device. */
    RT_Object_Class_Timer         = 0x0a,      /**< The object is a timer. */
    RT_Object_Class_Module        = 0x0b,      /**< The object is a module. */
    RT_Object_Class_Memory        = 0x0c,      /**< The object is a memory. */
    RT_Object_Class_Channel       = 0x0d,      /**< The object is a channel */
    RT_Object_Class_ProcessGroup  = 0x0e,      /**< The object is a process group */
    RT_Object_Class_Session       = 0x0f,      /**< The object is a session */
    RT_Object_Class_Custom        = 0x10,      /**< The object is a custom object */
    RT_Object_Class_Unknown       = 0x11,      /**< The object is unknown. */
    RT_Object_Class_Static        = 0x80       /**< The object is a static object. */
};

/**
 * @brief Per-class registry used internally by the generic object manager.
 *
 * There is one descriptor for each enabled class.  object_list is the sentinel
 * of globally registered instances (objects owned by a loadable module may be
 * kept on its private list instead), object_size is the allocation size for
 * dynamic objects, and spinlock serializes global-registry mutation/traversal.
 */
struct rt_object_information
{
    enum rt_object_class_type type;                     /**< Class represented by this registry. */
    rt_list_t                 object_list;              /**< Sentinel of globally registered objects of this class. */
    rt_size_t                 object_size;              /**< Bytes allocated by rt_object_allocate(). */
    struct rt_spinlock        spinlock;                 /**< Protects object_list and registry operations. */
};

/**
 * @brief Invoke a single function-pointer hook when hook support is enabled.
 *
 * @param func Hook variable, not a function name that is guaranteed to exist.
 * @param argv Parenthesized argument tuple, for example `(thread)`.
 *
 * The double macro layer allows @p func to be expanded before dispatch.
 * RT_HOOK_USING_FUNC_PTR selects the legacy single-listener implementation;
 * otherwise the call is compiled out and a hook-list point may be used.  Hook
 * code executes synchronously in the caller's context, which may be an ISR or
 * a scheduler-locked region, and must obey that call site's restrictions.
 */
#ifndef RT_USING_HOOK
#define RT_OBJECT_HOOK_CALL(func, argv)

#else

/**
 * @brief Add hook point in the routines
 * @note Usage:
 * void foo(void *arg) {
 *     do_something();
 *
 *     RT_OBJECT_HOOK_CALL(foo_hook, (arg));
 *
 *     do_other_things();
 * }
 */
#define _RT_OBJECT_HOOK_CALL(func, argv) __ON_HOOK_ARGS(func, argv)
#define RT_OBJECT_HOOK_CALL(func, argv)  _RT_OBJECT_HOOK_CALL(func, argv)

    #ifdef RT_HOOK_USING_FUNC_PTR
        #define __ON_HOOK_ARGS(__hook, argv)        do {if ((__hook) != RT_NULL) __hook argv; } while (0)
    #else
        #define __ON_HOOK_ARGS(__hook, argv)
    #endif /* RT_HOOK_USING_FUNC_PTR */
#endif /* RT_USING_HOOK */

#ifdef RT_USING_HOOKLIST

/**
 * @brief Declare the types and registration API for a multi-listener hook.
 *
 * The generated node contains the typed handler and an intrusive list node.
 * Callers own the node storage and must keep it alive while registered.  The
 * generated `name_nested` counter prevents registration/removal while a hook
 * traversal is in progress.  `handler` is the callback to invoke and
 * `list_node` links this caller-owned record into the hook point's listener
 * list; applications must treat both as registered-state metadata.
 *
 * @note Usage:
 * This is typically used in your header. In foo.h using this like:
 *
 * ```foo.h
 *     typedef void (*bar_hook_proto_t)(arguments...);
 *     RT_OBJECT_HOOKLIST_DECLARE(bar_hook_proto_t, bar_myhook);
 * ```
 */
#define RT_OBJECT_HOOKLIST_DECLARE(handler_type, name) \
    typedef struct name##_hooklistnode                 \
    {                                                  \
        handler_type handler;                          \
        rt_list_t list_node;                           \
    } *name##_hooklistnode_t;                          \
    extern volatile rt_ubase_t name##_nested;          \
    void name##_sethook(name##_hooklistnode_t node);   \
    void name##_rmhook(name##_hooklistnode_t node)

/**
 * @brief Define and statically initialize one caller-owned hook-list node.
 *
 * @note Usage
 * You can add a hook like this.
 *
 * ```addhook.c
 * void myhook(arguments...) { do_something(); }
 * RT_OBJECT_HOOKLIST_DEFINE_NODE(bar_myhook, myhook_node, myhook);
 *
 * void addhook(void)
 * {
 *      bar_myhook_sethook(&myhook_node);
 * }
 * ```
 *
 * BTW, you can also find examples codes under
 * `examples/utest/testcases/kernel/hooklist_tc.c`.
 */
#define RT_OBJECT_HOOKLIST_DEFINE_NODE(hookname, nodename, hooker_handler) \
    struct hookname##_hooklistnode nodename = {                            \
        .handler = hooker_handler,                                         \
        .list_node = RT_LIST_OBJECT_INIT(nodename.list_node),              \
    };

/**
 * @brief Define a hook list, its lock, and its registration functions.
 *
 * Add this macro exactly once, in the source file that owns the hook point.
 * Registration and removal use irqsave locking.  They wait until all active
 * traversals complete before changing the list, so these operations are not
 * suitable for ISR context.  A hook handler must never add or remove a node
 * from the same hook list: it would wait on its own traversal and deadlock.
 * Registering the same/already-linked node twice is also invalid because the
 * generated helpers do not perform duplicate-membership checks.
 */
#define RT_OBJECT_HOOKLIST_DEFINE(name)                                      \
    static rt_list_t name##_hooklist = RT_LIST_OBJECT_INIT(name##_hooklist); \
    static struct rt_spinlock name##lock = RT_SPINLOCK_INIT;                 \
    volatile rt_ubase_t name##_nested = 0;                                   \
    void name##_sethook(name##_hooklistnode_t node)                          \
    {                                                                        \
        rt_ubase_t level = rt_spin_lock_irqsave(&name##lock);                \
        while (name##_nested)                                                \
        {                                                                    \
            rt_spin_unlock_irqrestore(&name##lock, level);                   \
            level = rt_spin_lock_irqsave(&name##lock);                       \
        }                                                                    \
        rt_list_insert_before(&name##_hooklist, &node->list_node);           \
        rt_spin_unlock_irqrestore(&name##lock, level);                       \
    }                                                                        \
    void name##_rmhook(name##_hooklistnode_t node)                           \
    {                                                                        \
        rt_ubase_t level = rt_spin_lock_irqsave(&name##lock);                \
        while (name##_nested)                                                \
        {                                                                    \
            rt_spin_unlock_irqrestore(&name##lock, level);                   \
            level = rt_spin_lock_irqsave(&name##lock);                       \
        }                                                                    \
        rt_list_remove(&node->list_node);                                    \
        rt_spin_unlock_irqrestore(&name##lock, level);                       \
    }

/**
 * @brief Invoke every registered listener in list order.
 *
 * The nested counter protects list topology, but handlers are deliberately
 * called without holding the list spinlock.  This avoids executing arbitrary
 * hook code with interrupts disabled.  It also means handlers run in the
 * original call site's context and must provide their own protection for data
 * they share with other threads or CPUs.
 *
 * @note Usage:
 * void foo() {
 *     do_something();
 *
 *     RT_OBJECT_HOOKLIST_CALL(foo);
 *
 *     do_other_things();
 * }
 */
#define _RT_OBJECT_HOOKLIST_CALL(nodetype, nested, list, lock, argv)  \
    do                                                                \
    {                                                                 \
        nodetype iter, next;                                          \
        rt_ubase_t level = rt_spin_lock_irqsave(&lock);               \
        nested += 1;                                                  \
        rt_spin_unlock_irqrestore(&lock, level);                      \
        if (!rt_list_isempty(&list))                                  \
        {                                                             \
            rt_list_for_each_entry_safe(iter, next, &list, list_node) \
            {                                                         \
                iter->handler argv;                                   \
            }                                                         \
        }                                                             \
        level = rt_spin_lock_irqsave(&lock);                          \
        nested -= 1;                                                  \
        rt_spin_unlock_irqrestore(&lock, level);                      \
    } while (0)
#define RT_OBJECT_HOOKLIST_CALL(name, argv)                        \
    _RT_OBJECT_HOOKLIST_CALL(name##_hooklistnode_t, name##_nested, \
                             name##_hooklist, name##lock, argv)

#else

#define RT_OBJECT_HOOKLIST_DECLARE(handler_type, name)
#define RT_OBJECT_HOOKLIST_DEFINE_NODE(hookname, nodename, hooker_handler)
#define RT_OBJECT_HOOKLIST_DEFINE(name)
#define RT_OBJECT_HOOKLIST_CALL(name, argv)
#endif /* RT_USING_HOOKLIST */

/** @} group_object_management */

/**
 * @addtogroup group_clock_management
 */

/**@{*/

/**
 * Timer flag layout stored in rt_timer::parent.flag.
 *
 * The active bit is maintained by the timer subsystem.  Periodicity and
 * execution-context bits describe timer policy.  Normally a hard timer callback
 * runs from the tick/interrupt-side timer check and therefore must not block,
 * while a soft timer runs in the timer service thread.  Under
 * RT_USING_TIMER_ALL_SOFT all timers, including those carrying the zero-valued
 * HARD flag, are placed on the soft list and dispatched by that worker thread.
 * RT_TIMER_FLAG_THREAD_TIMER identifies the per-thread timeout timer whose
 * state is coordinated directly with the scheduler.
 */
#define RT_TIMER_FLAG_DEACTIVATED       0x0             /**< timer is deactive */
#define RT_TIMER_FLAG_ACTIVATED         0x1             /**< timer is active */
#define RT_TIMER_FLAG_ONE_SHOT          0x0             /**< one shot timer */
#define RT_TIMER_FLAG_PERIODIC          0x2             /**< periodic timer */

#define RT_TIMER_FLAG_HARD_TIMER        0x0             /**< hard timer,the timer's callback function will be called in tick isr. */
#define RT_TIMER_FLAG_SOFT_TIMER        0x4             /**< soft timer,the timer's callback function will be called in timer thread. */
#define RT_TIMER_FLAG_THREAD_TIMER \
    (0x8 | RT_TIMER_FLAG_HARD_TIMER)                    /**< thread timer that cooperates with scheduler directly */

#define RT_TIMER_CTRL_SET_TIME          0x0             /**< set timer control command */
#define RT_TIMER_CTRL_GET_TIME          0x1             /**< get timer control command */
#define RT_TIMER_CTRL_SET_ONESHOT       0x2             /**< change timer to one shot */
#define RT_TIMER_CTRL_SET_PERIODIC      0x3             /**< change timer to periodic */
#define RT_TIMER_CTRL_GET_STATE         0x4             /**< get timer run state active or deactive*/
#define RT_TIMER_CTRL_GET_REMAIN_TIME   0x5             /**< get the remaining hang time */
#define RT_TIMER_CTRL_GET_FUNC          0x6             /**< get timer timeout func  */
#define RT_TIMER_CTRL_SET_FUNC          0x7             /**< set timer timeout func  */
#define RT_TIMER_CTRL_GET_PARM          0x8             /**< get timer parameter  */
#define RT_TIMER_CTRL_SET_PARM          0x9             /**< set timer parameter  */

#ifndef RT_TIMER_SKIP_LIST_LEVEL
/** Number of ordered-list levels embedded in each timer object. */
#define RT_TIMER_SKIP_LIST_LEVEL          1
#endif

/* Mask controlling promotion among timer skip-list levels; normally 1 or 3. */
#ifndef RT_TIMER_SKIP_LIST_MASK
#define RT_TIMER_SKIP_LIST_MASK         0x3             /**< Timer skips the list mask */
#endif

/**
 * @brief Timer expiration callback.
 *
 * @param parameter Opaque value supplied when the timer is initialized.
 *
 * Normally the HARD/SOFT flag selects interrupt or timer-worker context;
 * RT_USING_TIMER_ALL_SOFT overrides that choice and dispatches every callback
 * in the worker thread.  While a timer callback is being dispatched it must not
 * detach, delete, free, or otherwise invalidate that timer: the dispatcher
 * invokes the exit hook and inspects timer state after the callback returns.
 * Stopping or reconfiguring a still-live timer is permitted by its API.
 */
typedef void (*rt_timer_func_t)(void *parameter);

/**
 * @brief Kernel timer control block.
 *
 * Timers are ordered by absolute timeout_tick in one or more intrusive lists.
 * init_tick stores the relative interval requested by the caller; timeout_tick
 * is recomputed whenever the timer is started.  For periodic timers the same
 * interval is used when scheduling the next expiration.
 */
struct rt_timer
{
    struct rt_object parent;                            /**< inherit from rt_object */

    rt_list_t        row[RT_TIMER_SKIP_LIST_LEVEL];    /**< Nodes for each level of the ordered timer skip list. */

    rt_timer_func_t  timeout_func;                      /**< timeout function */
    void             *parameter;                        /**< timeout function's parameter */

    rt_tick_t        init_tick;                         /**< Relative delay/period in system ticks. */
    rt_tick_t        timeout_tick;                      /**< Absolute tick at which the current activation expires. */
};
typedef struct rt_timer *rt_timer_t;

/**@}*/

/**
 * @addtogroup group_signal
 */
/**@{*/

#ifdef RT_USING_SIGNALS
/** Maximum number of classic kernel-thread signals represented by rt_sigset_t. */
#define RT_SIG_MAX          32
/** Bit set of pending or masked classic signals. */
typedef unsigned long rt_sigset_t;
/** Signal information type reused from the configured C/POSIX environment. */
typedef siginfo_t rt_siginfo_t;
/** Classic one-argument signal handler executed for @p signo. */
typedef void (*rt_sighandler_t)(int signo);
#endif /* RT_USING_SIGNALS */
/**@}*/

/**
 * @addtogroup group_thread_management
 * @{
 */

/*
 * Thread
 */

/*
 * Thread state encoding stored in the scheduler context's stat byte.
 * The low three bits hold the lifecycle/suspend state; upper bits carry
 * orthogonal yield and signal state.  Compare the low state through
 * RT_THREAD_STAT_MASK rather than comparing the complete byte directly.
 */
#define RT_THREAD_INIT                       0x00                /**< Initialized status */
#define RT_THREAD_CLOSE                      0x01                /**< Closed status */
#define RT_THREAD_READY                      0x02                /**< Ready status */
#define RT_THREAD_RUNNING                    0x03                /**< Running status */

/*
 * User-facing suspend policy accepted by rt_thread_suspend_with_flag().
 * It controls which signal classes may wake an RT-Smart thread while it is
 * blocked; the values are converted to the encoded states below.
 */
enum
{
    RT_INTERRUPTIBLE = 0, /**< Ordinary signals may interrupt the wait. */
    RT_KILLABLE,          /**< Only kill-class signals may interrupt the wait. */
    RT_UNINTERRUPTIBLE,   /**< Signals do not interrupt the wait. */
};

#define RT_THREAD_SUSPEND_MASK               0x04
#define RT_SIGNAL_COMMON_WAKEUP_MASK         0x02
#define RT_SIGNAL_KILL_WAKEUP_MASK           0x01

#define RT_THREAD_SUSPEND_INTERRUPTIBLE      (RT_THREAD_SUSPEND_MASK)                                                             /**< Suspend interruptable 0x4 */
#define RT_THREAD_SUSPEND                    RT_THREAD_SUSPEND_INTERRUPTIBLE
#define RT_THREAD_SUSPEND_KILLABLE           (RT_THREAD_SUSPEND_MASK | RT_SIGNAL_COMMON_WAKEUP_MASK)                              /**< Suspend with killable 0x6 */
#define RT_THREAD_SUSPEND_UNINTERRUPTIBLE    (RT_THREAD_SUSPEND_MASK | RT_SIGNAL_COMMON_WAKEUP_MASK | RT_SIGNAL_KILL_WAKEUP_MASK) /**< Suspend with uninterruptable 0x7 */
#define RT_THREAD_STAT_MASK                  0x07

#define RT_THREAD_STAT_YIELD            0x08                /**< indicate whether remaining_tick has been reloaded since last schedule */
#define RT_THREAD_STAT_YIELD_MASK       RT_THREAD_STAT_YIELD

#define RT_THREAD_STAT_SIGNAL           0x10                /**< task hold signals */
#define RT_THREAD_STAT_SIGNAL_READY     (RT_THREAD_STAT_SIGNAL | RT_THREAD_READY)
#define RT_THREAD_STAT_SIGNAL_WAIT      0x20                /**< task is waiting for signals */
#define RT_THREAD_STAT_SIGNAL_PENDING   0x40                /**< signals is held and it has not been procressed */
#define RT_THREAD_STAT_SIGNAL_MASK      0xf0

/**
 * thread control command definitions
 */
#define RT_THREAD_CTRL_STARTUP          0x00                /**< Startup thread. */
#define RT_THREAD_CTRL_CLOSE            0x01                /**< Close thread. */
#define RT_THREAD_CTRL_CHANGE_PRIORITY  0x02                /**< Change thread priority. */
#define RT_THREAD_CTRL_INFO             0x03                /**< Get thread information. */
#define RT_THREAD_CTRL_BIND_CPU         0x04                /**< Set thread bind cpu. */
#define RT_THREAD_CTRL_RESET_PRIORITY   0x05                /**< Reset thread priority. */

/**
 * @brief Accumulated CPU execution-time categories.
 *
 * Values are architecture-defined accounting units, normally scheduler ticks.
 * They are cumulative counters rather than percentages; per-thread recent
 * percentages are derived from snapshots when CPU usage tracing is enabled.
 */
struct rt_cpu_usage_stats
{
    rt_ubase_t user;       /**< Time spent executing unprivileged/user code. */
    rt_ubase_t system;     /**< Time spent executing privileged kernel code. */
    rt_ubase_t irq;        /**< Reserved IRQ/exception slot; common tick accounting currently leaves it unchanged. */
    rt_ubase_t idle;       /**< Time spent in the per-CPU idle thread. */
};
typedef struct rt_cpu_usage_stats *rt_cpu_usage_stats_t;

#ifdef RT_USING_SMP

#define RT_CPU_DETACHED                 RT_CPUS_NR          /**< The thread not running on cpu. */
#define RT_CPU_MASK                     ((1 << RT_CPUS_NR) - 1) /**< All CPUs mask bit. */

#ifndef RT_SCHEDULE_IPI
/** Inter-processor interrupt used to request rescheduling on another CPU. */
#define RT_SCHEDULE_IPI                 0
#endif /* RT_SCHEDULE_IPI */

#ifndef RT_STOP_IPI
/** Inter-processor interrupt used by the architecture's CPU-stop protocol. */
#define RT_STOP_IPI                     1
#endif /* RT_STOP_IPI */

#ifndef RT_SMP_CALL_IPI
/** Inter-processor interrupt used to execute a function on remote CPUs. */
#define RT_SMP_CALL_IPI                 2
#endif

#define RT_MAX_IPI                      3

#define _SCHEDULER_CONTEXT(fileds) fileds

/**
 * @brief Per-CPU scheduler and interrupt bookkeeping for an SMP build.
 *
 * Scheduler fields are private to their owning CPU.  The local core accesses
 * them while in an RT-Thread critical section; unsynchronized remote access is
 * undefined.  Threads without a CPU binding can also reside in the scheduler's
 * global ready queue, which is maintained outside this structure.
 */
struct rt_cpu
{
    /**
     * protected by:
     *   - other cores: accessing from other coress is undefined behaviour
     *   - local core: rt_enter_critical()/rt_exit_critical()
     */
    _SCHEDULER_CONTEXT(
        struct rt_thread        *current_thread;       /**< Thread currently executing on this CPU. */

        rt_uint8_t              irq_switch_flag:1;     /**< Defer a requested switch until interrupt return. */
        rt_uint8_t              sched_lock_flag:1;     /**< CPU currently owns scheduler serialization. */
#ifndef ARCH_USING_HW_THREAD_SELF
        rt_uint8_t              critical_switch_flag:1; /**< A switch was postponed by a critical section. */
#endif /* ARCH_USING_HW_THREAD_SELF */

        rt_uint8_t              current_priority;      /**< Effective priority recorded for this CPU's running thread. */
        rt_list_t               priority_table[RT_THREAD_PRIORITY_MAX]; /**< Per-priority ready-list sentinels. */
    #if RT_THREAD_PRIORITY_MAX > 32
        rt_uint32_t             priority_group;        /**< Top-level bitmap identifying nonempty ready groups. */
        rt_uint8_t              ready_table[32];       /**< Second-level bitmap for priorities in each group. */
    #else
        rt_uint32_t             priority_group;        /**< One bit per nonempty per-CPU priority queue. */
    #endif /* RT_THREAD_PRIORITY_MAX > 32 */

        rt_atomic_t             tick;                   /**< Tick count observed on this CPU. */
    );

    struct rt_thread            *idle_thread;           /**< Lowest-priority idle thread bound to this CPU. */
    rt_atomic_t                 irq_nest;               /**< Current interrupt nesting depth on this CPU. */

#ifdef RT_USING_SMART
    struct rt_spinlock          spinlock;               /**< RT-Smart-specific protection for per-CPU state. */
#endif /* RT_USING_SMART */
#ifdef RT_USING_CPU_USAGE_TRACER
    struct rt_cpu_usage_stats   cpu_stat;               /**< Cumulative CPU usage accounting. */
#endif /* RT_USING_CPU_USAGE_TRACER */
#ifdef ARCH_USING_IRQ_CTX_LIST
    rt_slist_t                  irq_ctx_head;           /**< Stack/list of nested architecture IRQ contexts. */
#endif /* ARCH_USING_IRQ_CTX_LIST */
};

#else /* !RT_USING_SMP */
struct rt_cpu
{
    struct rt_thread            *current_thread;        /**< Currently executing thread in a UP build. */
    struct rt_thread            *idle_thread;           /**< System idle thread. */

#ifdef RT_USING_CPU_USAGE_TRACER
    struct rt_cpu_usage_stats   cpu_stat;               /**< Cumulative CPU usage accounting. */
#endif /* RT_USING_CPU_USAGE_TRACER */
#ifdef ARCH_USING_IRQ_CTX_LIST
    rt_slist_t                  irq_ctx_head;           /**< Nested architecture interrupt contexts. */
#endif /* ARCH_USING_IRQ_CTX_LIST */
};

#endif /* RT_USING_SMP */

typedef struct rt_cpu *rt_cpu_t;
/* Read-only compatibility spelling: applications obtain, but cannot assign, the current thread. */
#define rt_current_thread rt_thread_self()

struct rt_thread;

/**
 * @brief Architecture interrupt/exception context descriptor.
 *
 * Ports that enable ARCH_USING_IRQ_CTX_LIST push these descriptors so nested
 * exceptions can be inspected (for example by diagnostics or backtrace code).
 */

typedef struct rt_interrupt_context {
    void *context;      /**< Pointer to an architecture-defined saved register frame. */
    rt_slist_t node;    /**< Intrusive node in the current CPU's nested IRQ-context list. */
} *rt_interrupt_context_t;

#ifdef RT_USING_SMART
/**
 * RT-Smart wait-object wakeup adapter.
 *
 * A blocking subsystem installs a callback that knows how to detach @p thread
 * from its private wait object.  The callback returns an RT-Thread status and is
 * used when asynchronous process events need to wake a blocked user thread.
 */
typedef rt_err_t (*rt_wakeup_func_t)(void *object, struct rt_thread *thread);

/** Callback plus opaque wait-object data associated with one blocked thread. */
struct rt_wakeup
{
    rt_wakeup_func_t func; /**< Subsystem-specific operation that performs the wakeup. */
    void *user_data;       /**< Wait object passed as the callback's first argument. */
};

/* RT-Smart supports 64 process-level signal numbers. */
#define _LWP_NSIG       64

#ifdef ARCH_CPU_64BIT
#define _LWP_NSIG_BPW   64
#else
#define _LWP_NSIG_BPW   32
#endif

#define _LWP_NSIG_WORDS (RT_ALIGN(_LWP_NSIG, _LWP_NSIG_BPW) / _LWP_NSIG_BPW)

/** Traditional one-argument userspace signal handler. */
typedef void (*lwp_sighandler_t)(int);
/** SA_SIGINFO-style userspace handler receiving extended signal context. */
typedef void (*lwp_sigaction_t)(int signo, siginfo_t *info, void *context);

/** Fixed-size signal bitmap split into native-word chunks. */
typedef struct
{
    unsigned long sig[_LWP_NSIG_WORDS]; /**< Bit N-1 represents signal number N. */
} lwp_sigset_t;

#if _LWP_NSIG <= 64
#define lwp_sigmask(signo)      ((lwp_sigset_t){.sig = {[0] = ((long)(1u << ((signo)-1)))}})
#define lwp_sigset_init(mask)   ((lwp_sigset_t){.sig = {[0] = (long)(mask)}})
#endif /* _LWP_NSIG <= 64 */

/** Per-signal action installed by an RT-Smart process. */
struct lwp_sigaction
{
    union
    {
        void (*_sa_handler)(int);                    /**< Traditional sa_handler callback. */
        void (*_sa_sigaction)(int, siginfo_t *, void *); /**< Extended SA_SIGINFO callback. */
    } __sa_handler;
    lwp_sigset_t sa_mask;                            /**< Extra signals blocked during the callback. */
    int sa_flags;                                    /**< POSIX-style SA_* behavior flags. */
    void (*sa_restorer)(void);                       /**< Optional userspace signal-return trampoline. */
};

/** Optional signal-specific payload stored separately from common metadata. */
typedef struct lwp_siginfo_ext
{
    union
    {
        /* for SIGCHLD */
        struct
        {
            int status;                              /**< Child exit status or stop/continue code. */
            clock_t utime;                           /**< User CPU time consumed by the child. */
            clock_t stime;                           /**< System CPU time consumed by the child. */
        } sigchld;
    };
} *lwp_siginfo_ext_t;

/** One queued RT-Smart signal occurrence. */
typedef struct lwp_siginfo
{
    rt_list_t node;                                  /**< Node in lwp_sigqueue::siginfo_list. */

    struct
    {
        int signo;                                   /**< Signal number. */
        int code;                                    /**< Origin/cause code analogous to si_code. */

        int from_tid;                                /**< Sending thread ID, when known. */
        pid_t from_pid;                              /**< Sending process ID, when known. */
    } ksiginfo;

    struct lwp_siginfo_ext *ext;                     /**< Optional signal-specific extension payload. */
} *lwp_siginfo_t;

/** Pending signal queue and a bitmap used for fast pending checks. */
typedef struct lwp_sigqueue
{
    rt_list_t siginfo_list;                          /**< Ordered list of queued signal occurrences. */
    lwp_sigset_t sigset_pending;                     /**< Union of signal numbers currently pending. */
} *lwp_sigqueue_t;

/** Signal state private to one RT-Smart thread. */
struct lwp_thread_signal {
    lwp_sigset_t sigset_mask;                        /**< Signals blocked by this thread. */
    struct lwp_sigqueue sig_queue;                   /**< Signals pending specifically for this thread. */
};

/** Architecture-neutral pointers describing a suspended userspace context. */
struct rt_user_context
{
    void *sp;                                        /**< Saved userspace stack pointer. */
    void *pc;                                        /**< Saved userspace program counter. */
    void *flag;                                      /**< Architecture status/flags value. */

    void *ctx;                                       /**< Kernel-side context marker; NULL denotes user mode. */
};
#endif /* RT_USING_SMART */

/**
 * Thread cleanup callback invoked during deferred thread reclamation.
 *
 * It runs after the thread has stopped executing.  The callback may release
 * caller-owned resources but must not assume it runs on the exiting thread's
 * stack; depending on configuration it runs from idle or the system defunct
 * thread.
 */
typedef void (*rt_thread_cleanup_t)(struct rt_thread *tid);

/**
 * @brief Thread Control Block (TCB).
 *
 * A thread is both a managed kernel object and a schedulable execution context.
 * The architecture port owns the layout below `sp`; the scheduler owns the
 * fields expanded by RT_SCHED_THREAD_CTX; IPC and timer code coordinate through
 * the embedded thread_timer.  Most fields are kernel-private and must be read
 * or changed through the public thread APIs.
 */
struct rt_thread
{
    struct rt_object            parent;                 /**< Base object; must remain the first field. */

    /* Architecture context, initial entry, and owned stack extent. */
    void                        *sp;                    /**< Saved kernel stack pointer used by context switching. */
    void                        *entry;                 /**< Thread entry routine, stored generically for ABI portability. */
    void                        *parameter;             /**< Opaque argument passed to the entry routine. */
    void                        *stack_addr;            /**< Lowest/base address of the allocated stack region. */
    rt_uint32_t                 stack_size;             /**< Stack region size in bytes. */

    rt_err_t                    error;                  /**< Last per-thread kernel error; also conveys wakeup/timeout status. */

#ifdef RT_USING_SMP
    rt_atomic_t                 cpus_lock_nest;         /**< Nesting count for the legacy all-CPU scheduler lock. */
#endif

    /* Priority, ready/wait-list membership, state, time slice, and CPU affinity. */
    RT_SCHED_THREAD_CTX
    struct rt_timer             thread_timer;           /**< One-shot timeout timer reused by sleeps and blocking IPC. */
    rt_thread_cleanup_t         cleanup;                /**< Optional callback executed during deferred reclamation. */

#ifdef RT_USING_MUTEX
    /* Mutex ownership graph used by priority inheritance and exit cleanup. */
    rt_list_t                   taken_object_list;      /**< Mutexes currently owned by this thread. */
    rt_object_t                 pending_object;         /**< Mutex object this thread is currently waiting to acquire. */
#endif /* RT_USING_MUTEX */

#ifdef RT_USING_EVENT
    /* Requested event condition retained while the thread is blocked. */
    rt_uint32_t                 event_set;              /**< Event bits requested by rt_event_recv(). */
    rt_uint8_t                  event_info;             /**< AND/OR/CLEAR matching options for the pending receive. */
#endif /* RT_USING_EVENT */

#ifdef RT_USING_SIGNALS
    rt_sigset_t                 sig_pending;            /**< Bitmap of classic signals awaiting delivery. */
    rt_sigset_t                 sig_mask;               /**< Bitmap of classic signals enabled/unmasked for delivery. */

#ifndef RT_USING_SMP
    void                        *sig_ret;               /**< Saved stack pointer used to return from a signal handler. */
#endif /* RT_USING_SMP */
    rt_sighandler_t             *sig_vectors;           /**< Per-signal handler vector allocated for the thread. */
    void                        *si_list;               /**< Private queued signal-information list. */
#endif /* RT_USING_SIGNALS */

#ifdef RT_USING_PTHREADS
    void                        *pthread_data;          /**< POSIX-thread adaptation data, pointer-sized on all ABIs. */
#endif /* RT_USING_PTHREADS */

    /* light weight process if present */
#ifdef RT_USING_SMART
    void                        *msg_ret;               /**< Saved return value/message used by RT-Smart IPC paths. */

    void                        *lwp;                   /**< Owning lightweight-process object. */
    /* Userspace entry and dual-stack information. */
    void                        *user_entry;            /**< Initial userspace program counter. */
    void                        *user_stack;            /**< Base/address of the userspace stack mapping. */
    rt_uint32_t                 user_stack_size;        /**< Userspace stack extent in bytes. */
    rt_uint32_t                 *kernel_sp;             /**< Kernel stack pointer saved across user transitions. */
    rt_list_t                   sibling;                /**< Node in the owning process's thread list. */

    struct lwp_thread_signal    signal;                 /**< Mask and queued signals private to this user thread. */
    struct rt_user_context      user_ctx;               /**< Saved architecture-neutral userspace context. */
    struct rt_wakeup            wakeup_handle;          /**< Adapter for removing this thread from an RT-Smart wait. */
    rt_atomic_t                 exit_request;           /**< Asynchronous request for this thread to terminate. */
    int                         tid;                    /**< Process-visible thread identifier. */
    int                         tid_ref_count;          /**< References keeping the TID mapping alive. */
    void                        *susp_recycler;         /**< Recycler waiting for this suspended thread to finish. */
    void                        *robust_list;           /**< Userspace robust/PI-lock list; validate every access carefully. */

#ifndef ARCH_MM_MMU
    lwp_sighandler_t            signal_handler[32];    /**< Per-signal handlers for no-MMU RT-Smart targets. */
#else
    int                         step_exec;              /**< Debugger single-step execution request/state. */
    int                         debug_attach_req;       /**< Pending debugger attach request. */
    int                         debug_ret_user;         /**< Debugger should return control to userspace. */
    int                         debug_suspend;          /**< Thread is suspended by the debugger. */
    struct rt_hw_exp_stack      *regs;                  /**< Architecture exception frame for ptrace/debugging. */
    void                        *thread_idr;             /**< Saved architecture thread-ID/TLS register value. */
    int                         *clear_child_tid;       /**< Userspace address cleared and futex-woken on exit. */
#endif /* ARCH_MM_MMU */
#endif /* RT_USING_SMART */

#ifdef RT_USING_CPU_USAGE_TRACER
    rt_ubase_t                  user_time;              /**< Accumulated execution units in userspace. */
    rt_ubase_t                  system_time;            /**< Accumulated execution units in kernel space. */
    rt_ubase_t                  total_time_prev;        /**< Previous total-time snapshot used for deltas. */
    rt_uint8_t                  cpu_usage;              /**< Most recently calculated CPU utilization percentage. */
#endif /* RT_USING_CPU_USAGE_TRACER */

#ifdef RT_USING_MEM_PROTECTION
    void *mem_regions;                                 /**< Architecture-defined memory-protection region set. */
#ifdef RT_USING_HW_STACK_GUARD
    void *stack_buf;                                   /**< Stack allocation metadata retained for guard setup. */
#endif /* RT_USING_HW_STACK_GUARD */
#endif /* RT_USING_MEM_PROTECTION */

    struct rt_spinlock          spinlock;               /**< Protects exit-time mutex cleanup and selected RT-Smart recycler snapshots. */
    rt_ubase_t                  user_data;              /**< Application-owned scalar/pointer-sized extension slot. */
};
typedef struct rt_thread *rt_thread_t;

#ifdef RT_USING_SMART
/** True when an RT-Smart thread's saved state represents userspace execution. */
#define LWP_IS_USER_MODE(t) ((t)->user_ctx.ctx == RT_NULL)
#else
#define LWP_IS_USER_MODE(t) (0)
#endif /* RT_USING_SMART */

/** @} group_thread_management */

/**
 * @addtogroup group_thread_comm
 */

/**@{*/

/**
 * IPC wait ordering and generic control commands.
 *
 * FIFO preserves arrival order.  PRIO orders waiters by effective scheduling
 * priority so that a numerically smaller (higher-priority) thread can be woken
 * first.  RT_WAITING_NO makes a take/receive operation non-blocking, while
 * RT_WAITING_FOREVER suppresses installation of a timeout timer.
 */
#define RT_IPC_FLAG_FIFO                0x00            /**< FIFOed IPC. @ref group_thread_comm. */
#define RT_IPC_FLAG_PRIO                0x01            /**< PRIOed IPC. @ref group_thread_comm. */

#define RT_IPC_CMD_UNKNOWN              0x00            /**< unknown IPC command */
#define RT_IPC_CMD_RESET                0x01            /**< reset IPC object */
#define RT_IPC_CMD_GET_STATE            0x02            /**< get the state of IPC object */
#define RT_IPC_CMD_SET_VLIMIT           0x03            /**< set max limit value of IPC value */

#define RT_WAITING_FOREVER              -1              /**< Block forever until get resource. */
#define RT_WAITING_NO                   0               /**< Non-block. */

/**
 * @brief Common base of semaphore, mutex, event, mailbox, and message queue.
 *
 * The parent object's flag stores the IPC wait-order policy.  suspend_thread is
 * the receiver/acquirer wait queue.  Mailboxes and message queues additionally
 * carry a sender wait queue for the full-buffer case.
 */
struct rt_ipc_object
{
    struct rt_object parent;                            /**< inherit from rt_object */

    rt_list_t suspend_thread;                           /**< Threads blocked waiting to acquire/receive this resource. */
};

/**
 * @addtogroup group_semaphore Semaphore
 * @{
 */

#ifdef RT_USING_SEMAPHORE
/**
 * @brief Counting semaphore control block.
 *
 * value is the immediately available token count and never exceeds max_value.
 * spinlock makes the counter update and waiter transfer atomic with respect to
 * interrupts and other CPUs.
 */
struct rt_semaphore
{
    struct rt_ipc_object parent;                        /**< inherit from ipc_object */

    rt_uint16_t          value;                         /**< Tokens currently available without blocking. */
    rt_uint16_t          max_value;                     /**< Saturation/validation limit for value. */
    struct rt_spinlock   spinlock;                      /**< Protects value and the inherited wait queue. */
};
typedef struct rt_semaphore *rt_sem_t;
#endif /* RT_USING_SEMAPHORE */

/**@}*/

/**
 * @addtogroup group_mutex Mutex
 * @{
 */

#ifdef RT_USING_MUTEX
/**
 * @brief Recursive mutex with priority-inversion mitigation.
 *
 * owner may acquire the mutex repeatedly; hold counts the nesting depth.  The
 * mutex is also linked into owner->taken_object_list through taken_list.  The
 * priority fields retain the configured ceiling and the best priority among
 * waiters so the implementation can propagate and later restore effective
 * priorities.  During normal operation a mutex may be released only by owner.
 * Kernel cleanup is the deliberate exception: it may unwind a mutex whose
 * recorded owner has already entered RT_THREAD_CLOSE state.
 */
struct rt_mutex
{
    struct rt_ipc_object parent;                        /**< inherit from ipc_object */

    rt_uint8_t           ceiling_priority;              /**< Configured priority ceiling; numerically lower means higher. */
    rt_uint8_t           priority;                      /**< Highest effective priority represented by pending waiters. */
    rt_uint8_t           hold;                          /**< Recursive acquisition depth held by owner. */
    rt_uint8_t           reserved;                      /**< Padding/reserved byte; callers must not use it. */

    struct rt_thread    *owner;                         /**< Thread that currently owns the mutex, or NULL. */
    rt_list_t            taken_list;                    /**< Node in owner->taken_object_list. */
    struct rt_spinlock   spinlock;                      /**< Protects ownership, hold count, priority, and waiters. */
};
typedef struct rt_mutex *rt_mutex_t;
#endif /* RT_USING_MUTEX */

/**@}*/

/**
 * @addtogroup group_event Event
 * @{
 */

#ifdef RT_USING_EVENT
/**
 * Event receive-option flags.  Exactly one of AND/OR describes matching;
 * CLEAR consumes the matched bits atomically when the receive succeeds.
 */
#define RT_EVENT_FLAG_AND               0x01            /**< logic and */
#define RT_EVENT_FLAG_OR                0x02            /**< logic or */
#define RT_EVENT_FLAG_CLEAR             0x04            /**< clear flag */

/**
 * @brief Event-bit synchronization object.
 *
 * Each blocked receiver stores its requested mask and options in its TCB.
 * Sending bits ORs them into set and scans waiters for matching AND/OR
 * conditions.  Event bits represent state, not queued occurrences; repeatedly
 * sending an already-set bit does not accumulate a count.
 */
struct rt_event
{
    struct rt_ipc_object parent;                        /**< inherit from ipc_object */

    rt_uint32_t          set;                           /**< Current 32-bit event state. */
    struct rt_spinlock   spinlock;                      /**< Protects set and receiver wakeup selection. */
};
typedef struct rt_event *rt_event_t;
#endif /* RT_USING_EVENT */

/**@}*/

/**
 * @addtogroup group_mailbox MailBox
 * @{
 */

#ifdef RT_USING_MAILBOX
/**
 * @brief Ring buffer of pointer-width messages.
 *
 * A mailbox copies one rt_ubase_t value per message; it does not copy data
 * referenced by that value.  entry is the current occupancy, while in_offset
 * and out_offset wrap modulo size.  Receivers wait on the inherited queue and
 * senders blocked by a full ring wait on suspend_sender_thread.
 */
struct rt_mailbox
{
    struct rt_ipc_object parent;                        /**< inherit from ipc_object */

    rt_ubase_t          *msg_pool;                      /**< Array of slots: caller-owned for init, heap-owned for create. */

    rt_uint16_t          size;                          /**< Total number of slots in msg_pool. */

    rt_uint16_t          entry;                         /**< Number of currently queued messages. */
    rt_uint16_t          in_offset;                     /**< Ring index at which the next normal send writes. */
    rt_uint16_t          out_offset;                    /**< Ring index from which the next receive reads. */

    rt_list_t            suspend_sender_thread;         /**< Threads blocked because the ring is full. */
    struct rt_spinlock   spinlock;                      /**< Protects ring indexes, occupancy, and both wait queues. */
};
typedef struct rt_mailbox *rt_mailbox_t;
#endif /* RT_USING_MAILBOX */

/**@}*/

/**
 * @addtogroup group_messagequeue Message Queue
 * @{
 */

#ifdef RT_USING_MESSAGEQUEUE
/**
 * @brief Queue of fixed-capacity, copy-by-value messages.
 *
 * msg_pool is divided into max_msgs internal nodes, each large enough for an
 * implementation header plus an aligned msg_size payload.  The three private
 * pointers form the queued-message chain and the free-node pool.  The queued
 * chain is FIFO for ordinary sends, but priority-send/priority-receive support
 * may order nodes by message priority.  Unlike a mailbox, a send copies up to
 * msg_size bytes into a queue-owned node.
 */
struct rt_messagequeue
{
    struct rt_ipc_object parent;                        /**< inherit from ipc_object */

    void                *msg_pool;                      /**< Node storage: caller-owned for init, heap-owned for create. */

    rt_uint16_t          msg_size;                      /**< Maximum payload bytes stored in each node. */
    rt_uint16_t          max_msgs;                      /**< Total node count and maximum queue depth. */

    rt_uint16_t          entry;                         /**< Number of messages currently queued. */

    void                *msg_queue_head;                /**< First queued internal message node. */
    void                *msg_queue_tail;                /**< Last queued internal message node. */
    void                *msg_queue_free;                /**< Head of the internal free-node chain. */

    rt_list_t            suspend_sender_thread;         /**< Senders blocked because no free node is available. */
    struct rt_spinlock   spinlock;                      /**< Protects node chains, entry, and wait queues. */
};
typedef struct rt_messagequeue *rt_mq_t;
#endif /* RT_USING_MESSAGEQUEUE */

/**@}*/

/**@}*/

/**
 * @addtogroup group_memory_management
 */

/**@{*/

#ifdef RT_USING_HEAP
/**
 * @brief Common statistics object for system-heap backends.
 *
 * Small-memory and slab allocators expose the same public rt_mem_t handle by
 * embedding this descriptor in their private implementation object.  Values
 * report allocator-managed payload/accounting bytes and need not equal raw BSP
 * region boundaries after alignment and metadata overhead are applied.
 */
struct rt_memory
{
    struct rt_object        parent;                 /**< Base object; must remain the first field. */
    const char *            algorithm;              /**< Human-readable allocator/backend name. */
    rt_ubase_t              address;                /**< Aligned start address of the managed region. */
    rt_size_t               total;                  /**< Total bytes managed by this allocator. */
    rt_size_t               used;                   /**< Current accounted allocation in bytes. */
    rt_size_t               max;                    /**< High-water mark of used since initialization. */
};
typedef struct rt_memory *rt_mem_t;
#endif /* RT_USING_HEAP */

/*
 * memory management
 * heap & partition
 */

#ifdef RT_USING_SMALL_MEM
typedef rt_mem_t rt_smem_t;
#endif /* RT_USING_SMALL_MEM */

#ifdef RT_USING_SLAB
typedef rt_mem_t rt_slab_t;
#endif /* RT_USING_SLAB */

#ifdef RT_USING_MEMHEAP
/**
 * @brief Boundary tag and list links stored before a memheap allocation.
 *
 * Every physical block participates in the address-ordered next/prev chain;
 * only free blocks participate in next_free/prev_free.  pool_ptr identifies the
 * owning heap when multiple memheaps feed the system allocator.  magic encodes
 * allocation state and is checked to detect invalid or repeated frees.
 */
struct rt_memheap_item
{
    rt_uint32_t             magic;                      /**< Integrity/allocation-state marker. */
    struct rt_memheap      *pool_ptr;                   /**< Heap that owns this block. */

    struct rt_memheap_item *next;                       /**< Next physical block by address. */
    struct rt_memheap_item *prev;                       /**< Previous physical block by address. */

    struct rt_memheap_item *next_free;                  /**< Next free block in allocator search order. */
    struct rt_memheap_item *prev_free;                  /**< Previous free block in allocator search order. */
#ifdef RT_USING_MEMTRACE
    rt_uint8_t              owner_thread_name[4];       /**< Truncated allocating-thread name for diagnostics. */
#endif /* RT_USING_MEMTRACE */
};

/**
 * @brief Variable-size allocator over one caller-provided memory region.
 *
 * block_list points to the first physical boundary-tag block; free_header is
 * the embedded free-list sentinel and free_list normally points to that
 * sentinel as the search anchor.  The embedded semaphore normally
 * serializes allocation.  When locked is true an outer system-heap lock already
 * provides serialization, avoiding recursive locking and early-startup
 * dependence on the semaphore.
 */
struct rt_memheap
{
    struct rt_object        parent;                     /**< inherit from rt_object */

    void                   *start_addr;                 /**< Caller-supplied start; allocator assumes required alignment. */

    rt_size_t               pool_size;                  /**< Supplied size rounded down; includes allocator boundary headers. */
    rt_size_t               available_size;            /**< Current free bytes tracked by the allocator. */
    rt_size_t               max_used_size;              /**< High-water mark of allocated bytes. */

    struct rt_memheap_item *block_list;                 /**< Sentinel/entry for the physical block chain. */

    struct rt_memheap_item *free_list;                  /**< Free-list sentinel/search anchor (normally &free_header). */
    struct rt_memheap_item  free_header;                /**< Embedded sentinel for the free-block chain. */

    struct rt_semaphore     lock;                       /**< Internal allocator mutex-like semaphore. */
    rt_bool_t               locked;                     /**< True when synchronization is supplied externally. */
};
#endif /* RT_USING_MEMHEAP */

#ifdef RT_USING_MEMPOOL
/**
 * @brief Fixed-size block pool with optional blocking allocation.
 *
 * Free blocks reuse their first pointer-sized bytes to link block_list.  A take
 * can suspend when block_free_count is zero; rt_mp_free() returns a block and
 * wakes one waiter.  The pool neither constructs nor destroys objects stored in
 * blocks, and callers must return each block to its original pool exactly once.
 */
struct rt_mempool
{
    struct rt_object    parent;                            /**< inherit from rt_object */

    void                *start_address;                    /**< Backing storage: caller-owned for init, heap-owned for create. */
    rt_size_t           size;                             /**< Total bytes supplied for the pool. */

    rt_size_t           block_size;                       /**< Aligned bytes in each allocatable block. */
    rt_uint8_t          *block_list;                       /**< Head of the intrusive free-block chain. */

    rt_size_t           block_total_count;                /**< Number of blocks carved from the region. */
    rt_size_t           block_free_count;                 /**< Number of blocks currently available. */

    rt_list_t           suspend_thread;                   /**< Threads blocked waiting for a free block. */
    struct rt_spinlock  spinlock;                         /**< Protects free chain, counters, and waiters. */
};
typedef struct rt_mempool *rt_mp_t;
#endif /* RT_USING_MEMPOOL */

/**@}*/

#ifdef RT_USING_DEVICE
/**
 * @addtogroup group_device_driver
 */

/**@{*/

/**
 * @brief Coarse class used for discovery and class-specific control ranges.
 *
 * This value identifies the public role of a device, not the concrete driver or
 * bus used to reach it.  A class driver may embed rt_device in a larger object
 * and keep protocol-specific state after the base object.
 */
enum rt_device_class_type
{
    RT_Device_Class_Char = 0,                           /**< character device */
    RT_Device_Class_Block,                              /**< block device */
    RT_Device_Class_NetIf,                              /**< net interface */
    RT_Device_Class_MTD,                                /**< memory device */
    RT_Device_Class_CAN,                                /**< CAN device */
    RT_Device_Class_RTC,                                /**< RTC device */
    RT_Device_Class_Sound,                              /**< Sound device */
    RT_Device_Class_Graphic,                            /**< Graphic device */
    RT_Device_Class_I2CBUS,                             /**< I2C bus device */
    RT_Device_Class_USBDevice,                          /**< USB slave device */
    RT_Device_Class_USBHost,                            /**< USB host bus */
    RT_Device_Class_USBOTG,                             /**< USB OTG bus */
    RT_Device_Class_SPIBUS,                             /**< SPI bus device */
    RT_Device_Class_SPIDevice,                          /**< SPI device */
    RT_Device_Class_SDIO,                               /**< SDIO bus device */
    RT_Device_Class_PM,                                 /**< PM pseudo device */
    RT_Device_Class_Pipe,                               /**< Pipe device */
    RT_Device_Class_Portal,                             /**< Portal device */
    RT_Device_Class_Timer,                              /**< Timer device */
    RT_Device_Class_Miscellaneous,                      /**< Miscellaneous device */
    RT_Device_Class_Sensor,                             /**< Sensor device */
    RT_Device_Class_Touch,                              /**< Touch device */
    RT_Device_Class_PHY,                                /**< PHY device */
    RT_Device_Class_Security,                           /**< Security device */
    RT_Device_Class_WLAN,                               /**< WLAN device */
    RT_Device_Class_Pin,                                /**< Pin device */
    RT_Device_Class_ADC,                                /**< ADC device */
    RT_Device_Class_DAC,                                /**< DAC device */
    RT_Device_Class_WDT,                                /**< WDT device */
    RT_Device_Class_PWM,                                /**< PWM device */
    RT_Device_Class_Bus,                                /**< Bus device */
    RT_Device_Class_Unknown                             /**< unknown device */
};

/**
 * Device capability and runtime-state flags stored in rt_device::flag.
 *
 * The low access bits describe supported directions, middle bits describe
 * lifecycle/capabilities, and high bits select interrupt or DMA transfer modes.
 * These are registration-time/device-state flags and are distinct from the
 * per-open request recorded in rt_device::open_flag.
 */
#define RT_DEVICE_FLAG_DEACTIVATE       0x000           /**< device is not not initialized */

#define RT_DEVICE_FLAG_RDONLY           0x001           /**< read only */
#define RT_DEVICE_FLAG_WRONLY           0x002           /**< write only */
#define RT_DEVICE_FLAG_RDWR             0x003           /**< read and write */

#define RT_DEVICE_FLAG_REMOVABLE        0x004           /**< removable device */
#define RT_DEVICE_FLAG_STANDALONE       0x008           /**< standalone device */
#define RT_DEVICE_FLAG_ACTIVATED        0x010           /**< device is activated */
#define RT_DEVICE_FLAG_SUSPENDED        0x020           /**< device is suspended */
#define RT_DEVICE_FLAG_STREAM           0x040           /**< stream mode */
#define RT_DEVICE_FLAG_DYNAMIC          0x080           /**< device is determined when open() */

#define RT_DEVICE_FLAG_INT_RX           0x100           /**< INT mode on Rx */
#define RT_DEVICE_FLAG_DMA_RX           0x200           /**< DMA mode on Rx */
#define RT_DEVICE_FLAG_INT_TX           0x400           /**< INT mode on Tx */
#define RT_DEVICE_FLAG_DMA_TX           0x800           /**< DMA mode on Tx */

#define RT_DEVICE_OFLAG_CLOSE           0x000           /**< device is closed */
#define RT_DEVICE_OFLAG_RDONLY          0x001           /**< read only access */
#define RT_DEVICE_OFLAG_WRONLY          0x002           /**< write only access */
#define RT_DEVICE_OFLAG_RDWR            0x003           /**< read and write */
#define RT_DEVICE_OFLAG_OPEN            0x008           /**< device is opened */
#define RT_DEVICE_OFLAG_MASK            0xf0f           /**< mask of open flag */

/**
 * general device commands
 * 0x01 - 0x1F general device control commands
 * 0x20 - 0x3F udevice control commands
 * 0x40 -      special device control commands
 */
#define RT_DEVICE_CTRL_RESUME           0x01            /**< resume device */
#define RT_DEVICE_CTRL_SUSPEND          0x02            /**< suspend device */
#define RT_DEVICE_CTRL_CONFIG           0x03            /**< configure device */
#define RT_DEVICE_CTRL_CLOSE            0x04            /**< close device */
#define RT_DEVICE_CTRL_NOTIFY_SET       0x05            /**< set notify func */
#define RT_DEVICE_CTRL_SET_INT          0x06            /**< set interrupt */
#define RT_DEVICE_CTRL_CLR_INT          0x07            /**< clear interrupt */
#define RT_DEVICE_CTRL_GET_INT          0x08            /**< get interrupt status */
#define RT_DEVICE_CTRL_CONSOLE_OFLAG    0x09            /**< get console open flag */
#define RT_DEVICE_CTRL_MASK             0x1f            /**< mask for contrl commands */

/**
 * Build the base of a class-specific command namespace.  Class drivers can add
 * small command offsets to this value without colliding with generic commands
 * or commands of another device class.
 */
#define RT_DEVICE_CTRL_BASE(Type)        ((RT_Device_Class_##Type + 1) * 0x100)

typedef struct rt_driver *rt_driver_t;
typedef struct rt_device *rt_device_t;

#ifdef RT_USING_DEVICE_OPS
/**
 * @brief Uniform operations implemented by a concrete or class device driver.
 *
 * The core wrappers in components/drivers/core/device.c handle object lookup,
 * lazy initialization, state checks, and reference counts before dispatching
 * through this table.  A NULL optional operation is reported according to the
 * wrapper's contract.  Driver read/write callbacks normally return a
 * nonnegative transferred-unit count; an individual device class may document
 * an additional negative-error convention.  The core wrappers themselves use
 * zero plus errno for a closed device or missing operation.  The unit represented
 * by pos and size is device-class specific (bytes for streams, often blocks for
 * block devices).
 */
struct rt_device_ops
{
    /* Common device interface implemented by the driver. */
    rt_err_t  (*init)   (rt_device_t dev); /**< Put hardware/software state into an initialized state. */
    rt_err_t  (*open)   (rt_device_t dev, rt_uint16_t oflag); /**< Apply one open request's mode flags. */
    rt_err_t  (*close)  (rt_device_t dev); /**< Release/disable resources when the last user closes. */
    rt_ssize_t (*read)  (rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size); /**< Transfer data from device to buffer. */
    rt_ssize_t (*write) (rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size); /**< Transfer data from buffer to device. */
    rt_err_t  (*control)(rt_device_t dev, int cmd, void *args); /**< Execute generic or class-specific control command. */
};
#endif /* RT_USING_DEVICE_OPS */

/**
 * @brief Poll/select-compatible wait queue associated with a device or channel.
 *
 * waiting_list contains framework-defined wait nodes.  flag is the waitqueue's
 * internal CLEAN/WAKEUP state, not a device readiness-event bitmask; wakeup keys
 * are delivered separately to node callbacks.  spinlock makes state updates and
 * waiter notification atomic with interrupt-side producers.
 */
struct rt_wqueue
{
    rt_uint32_t flag;                  /**< Internal RT_WQ_FLAG_CLEAN/WAKEUP state. */
    rt_list_t waiting_list;            /**< Tasks or poll requests waiting for readiness. */
    struct rt_spinlock spinlock;       /**< Protects flag and waiting_list. */
};
typedef struct rt_wqueue rt_wqueue_t;

#ifdef RT_USING_DM
struct rt_driver;
struct rt_bus;
#endif /* RT_USING_DM */

/**
 * @brief Base object shared by every RT-Thread device instance.
 *
 * A class/concrete driver embeds this structure at offset zero, registers it by
 * name, and supplies operations plus user_data.  The device core owns type,
 * lifecycle flags, open reference accounting, and dispatch.  With
 * RT_USING_DM, the same object also participates in bus/driver matching; Device
 * Model extends rather than replaces the rt_device API.
 */
struct rt_device
{
    struct rt_object          parent;                   /**< inherit from rt_object */

#ifdef RT_USING_DM
    struct rt_bus *bus;                                 /**< Bus on which this device is registered. */
    rt_list_t node;                                     /**< Node in the bus's device collection. */
    struct rt_driver *drv;                              /**< Driver successfully bound to this device. */
#ifdef RT_USING_OFW
    void *ofw_node;                                     /**< Open Firmware/device-tree node describing this instance. */
#endif /* RT_USING_OFW */
    void *power_domain_unit;                            /**< Device Model power-domain attachment, if any. */
#ifdef RT_USING_DVFS
    void *dvfs_scaling;                                 /**< Per-device dynamic voltage/frequency scaling state. */
#endif
#ifdef RT_USING_DMA
    const void *dma_ops;                                /**< DMA mapping/operation set selected for this device. */
#endif
#endif /* RT_USING_DM */

    enum rt_device_class_type type;                     /**< Public device class. */
    rt_uint16_t               flag;                     /**< Capabilities and current activation/suspend state. */
    rt_uint16_t               open_flag;                /**< Effective mode and transfer flags of current opens. */

    rt_uint8_t                ref_count;                /**< Open references, including a core-accepted -RT_ENOSYS open result. */
#ifdef RT_USING_DM
    rt_uint8_t                master_id;                /**< Device Model master/owner identifier, range 0..255. */
#endif
    rt_uint8_t                device_id;                /**< Driver- or framework-assigned instance ID, range 0..255. */

    /*
     * Optional asynchronous notifications installed by an upper layer.
     * A lower driver may invoke them from its ISR/DMA completion path, so the
     * callback must follow that driver's context rules and the registrant must
     * keep both function and referenced state alive until in-flight callbacks
     * have been quiesced before replacement/unregistration.
     */
    rt_err_t (*rx_indicate)(rt_device_t dev, rt_size_t size); /**< Notify that size units can be read; may run in ISR context. */
    rt_err_t (*tx_complete)(rt_device_t dev, void *buffer);   /**< Notify completion of an asynchronous transmit buffer. */

#ifdef RT_USING_DEVICE_OPS
    const struct rt_device_ops *ops;                    /**< Immutable operation table supplied by the driver. */
#else
    /* Legacy ABI stores the same common operations directly in each object. */
    rt_err_t  (*init)   (rt_device_t dev); /**< Initialize the device. */
    rt_err_t  (*open)   (rt_device_t dev, rt_uint16_t oflag); /**< Apply requested open mode. */
    rt_err_t  (*close)  (rt_device_t dev); /**< Close/release the device. */
    rt_ssize_t (*read)  (rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size); /**< Read device-specific units. */
    rt_ssize_t (*write) (rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size); /**< Write device-specific units. */
    rt_err_t  (*control)(rt_device_t dev, int cmd, void *args); /**< Execute a control command. */
#endif /* RT_USING_DEVICE_OPS */

#ifdef RT_USING_POSIX_DEVIO
    const struct dfs_file_ops *fops;                    /**< POSIX/DFS file operations exposed by this device. */
    struct rt_wqueue wait_queue;                        /**< poll/select waiters for this device. */
#endif /* RT_USING_POSIX_DEVIO */

    rt_err_t (*readlink)
        (rt_device_t dev, char *buf, int len);          /**< Return the devfs symbolic-link target exposed by this device. */

    void                     *user_data;                /**< Opaque class/concrete-driver private state. */
};

/**
 * @brief Pair used to register a device-specific notification callback.
 */
struct rt_device_notify
{
    void (*notify)(rt_device_t dev);                    /**< Driver-triggered callback; execution context is driver-specific. */
    struct rt_device *dev;                              /**< Device associated with the notification. */
};

#ifdef RT_USING_SMART
/**
 * @brief RT-Smart synchronous message/reply channel.
 *
 * A channel is an IPC object that coordinates sender messages, blocked sender
 * threads, one reply target, and pollable reader readiness.  slock protects all
 * queue and state transitions; ref controls lifetime while users retain the
 * channel.
 */
struct rt_channel
{
    struct rt_ipc_object parent;                        /**< Base IPC object and generic wait queue. */
    struct rt_thread *reply;                            /**< Sending thread currently waiting to receive a reply. */
    struct rt_spinlock slock;                           /**< Protects channel state and all private queues. */
    rt_list_t wait_msg;                                 /**< Pending sender-message descriptors. */
    rt_list_t wait_thread;                              /**< Sender threads blocked awaiting receive/reply. */
    rt_wqueue_t reader_queue;                           /**< poll/select queue for readable channel state. */
    rt_uint8_t  stat;                                   /**< Implementation-defined channel lifecycle/status bits. */
    rt_ubase_t  ref;                                    /**< Channel reference count. */
};
typedef struct rt_channel *rt_channel_t;
#endif /* RT_USING_SMART */

/**@}*/
#endif /* RT_USING_DEVICE */

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* RT-Thread definitions for C++ */
namespace rtthread {

enum TICK_WAIT {
    WAIT_NONE = 0,       /**< Perform a non-blocking operation. */
    WAIT_FOREVER = -1,   /**< Block without installing a finite timeout. */
};

}

#endif /* __cplusplus */

#endif /* __RT_DEF_H__ */
