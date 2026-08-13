/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更日志：
 * 日期           作者         说明
 * 2024-01-18     Shell        将基础类型定义从 rtdef.h 中拆分出来
 */

/**
 * @file rttypes.h
 * @brief 基础标量、链表、原子存储和自旋锁类型。
 *
 * 此头文件是 RT-Thread 内核、组件、BSP 和 CPU 移植层共用的最底层类型约定。
 * 它有意只定义数据如何表示，而不提供内核服务；更高层的头文件会基于这里
 * 声明的类型构建对象、线程、IPC、定时器和设备等结构。
 *
 * 最终采用哪些定义取决于构建配置：
 *
 * - `RT_USING_ARCH_DATA_TYPE` 允许架构自行提供定宽整数别名。
 * - 在所选 libc 配置提供相应类型时，`RT_USING_LIBC` 使用 C 库的精确宽度和
 *   指针宽度类型。
 * - `ARCH_CPU_64BIT` 使本机基础类型宽度为 64 位；未定义时则为 32 位。
 * - `RT_USING_STDC_ATOMIC` 使用 C11 的 `_Atomic` 类型保存原子对象。
 * - `RT_USING_HW_ATOMIC` 使用普通存储，但通过架构专用的原子原语保护访问。
 * - `RT_USING_SMP` 让自旋锁采用 CPU 移植层定义的硬件锁；在单核构建中，
 *   该锁表示本地中断或临界区状态。
 *
 * 请勿在此头文件中加入对某块特定开发板的假设。这里的类型宽度和布局属于 ABI
 * 的一部分，会被汇编上下文切换代码、可加载模块、驱动程序和持久化协议结构使用。
 */

#ifndef __RT_TYPES_H__
#define __RT_TYPES_H__

/*
 * rtconfig.h 由 BSP 的 .config 生成，提供下方所用的全部功能和架构选择宏。
 * 标准头文件则提供定宽、大小、指针和可变参数类型。
 */
#include <rtconfig.h>

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#ifndef RT_USING_NANO
/*
 * Nano 配置不使用 POSIX 系统类型，以尽量减少依赖。标准版和 Smart 构建可使用
 * 这些头文件导出的 ssize_t、errno 值和平台信号类型。
 */
#include <sys/types.h>
#include <sys/errno.h>
#if defined(RT_USING_SIGNALS) || defined(RT_USING_SMART)
#include <sys/signal.h>
#endif /* defined(RT_USING_SIGNALS) || defined(RT_USING_SMART) */
#endif /* RT_USING_NANO */

#ifdef __cplusplus
/* 使经由此 C 兼容头文件引入的声明采用 C 链接方式。 */
extern "C" {
#endif

/**
 * @name RT-Thread 基础标量类型
 *
 * RT-Thread 的公共 API 使用这些别名，而非编译器相关的 C 类型写法。定宽别名
 * 用于序列化数据或寄存器宽度的数据；基础宽度别名表示 CPU 自然处理的字和指针。
 * @{
 */

/*
 * 在托管式 64 位 Windows 和 x86-64 构建中，可直接通过编译器预定义宏识别本机
 * 字宽。嵌入式 CPU 移植层则可在生成的配置中定义 ARCH_CPU_64BIT。
 */
#if defined(_WIN64) || defined(__x86_64__)
#ifndef ARCH_CPU_64BIT
#define ARCH_CPU_64BIT
#endif // ARCH_CPU_64BIT
#endif // defined(_WIN64) || defined(__x86_64__)

/** 布尔结果类型。RT_TRUE 和 RT_FALSE 是它的规范取值。 */
typedef int                             rt_bool_t;      /**< 整型布尔结果；请使用 RT_TRUE/RT_FALSE。 */

#ifndef RT_USING_ARCH_DATA_TYPE
#ifdef RT_USING_LIBC
/* 完整 libc 可用时，使用 C 库提供的精确宽度类型。 */
typedef int8_t                          rt_int8_t;      /**< 精确为 8 位的有符号整数。 */
typedef int16_t                         rt_int16_t;     /**< 精确为 16 位的有符号整数。 */
typedef int32_t                         rt_int32_t;     /**< 精确为 32 位的有符号整数。 */
typedef uint8_t                         rt_uint8_t;     /**< 精确为 8 位的无符号整数。 */
typedef uint16_t                        rt_uint16_t;    /**< 精确为 16 位的无符号整数。 */
typedef uint32_t                        rt_uint32_t;    /**< 精确为 32 位的无符号整数。 */
typedef int64_t                         rt_int64_t;     /**< 精确为 64 位的有符号整数。 */
typedef uint64_t                        rt_uint64_t;    /**< 精确为 64 位的无符号整数。 */
#else
/*
 * 此后备定义供没有 libc 的小型目标平台使用。RT-Thread 支持的工具链和 ABI
 * 应保证 char、short 和 int 具有下方别名所声明的宽度。
 */
typedef signed   char                   rt_int8_t;      /**< 由移植层 ABI 假定为 8 位的有符号整数。 */
typedef signed   short                  rt_int16_t;     /**< 由移植层 ABI 假定为 16 位的有符号整数。 */
typedef signed   int                    rt_int32_t;     /**< 由移植层 ABI 假定为 32 位的有符号整数。 */
typedef unsigned char                   rt_uint8_t;     /**< 由移植层 ABI 假定为 8 位的无符号整数。 */
typedef unsigned short                  rt_uint16_t;    /**< 由移植层 ABI 假定为 16 位的无符号整数。 */
typedef unsigned int                    rt_uint32_t;    /**< 由移植层 ABI 假定为 32 位的无符号整数。 */
#ifdef ARCH_CPU_64BIT
/* 此已配置的 64 位移植层假定其 ABI 使用 long 表示 64 位值。 */
typedef signed long                     rt_int64_t;     /**< 由移植层 ABI 假定为 64 位的有符号整数。 */
typedef unsigned long                   rt_uint64_t;    /**< 由移植层 ABI 假定为 64 位的无符号整数。 */
#else
/* 32 位 ABI 使用 long long 表示明确为 64 位的标量。 */
typedef signed long long                rt_int64_t;     /**< 由移植层 ABI 假定为 64 位的有符号整数。 */
typedef unsigned long long              rt_uint64_t;    /**< 由移植层 ABI 假定为 64 位的无符号整数。 */
#endif /* ARCH_CPU_64BIT */
#endif /* RT_USING_LIBC */
#endif /* RT_USING_ARCH_DATA_TYPE */

/*
 * rt_base_t/rt_ubase_t 的宽度等于一个本机机器字。它们适合保存中断状态令牌、
 * 寄存器值、指针转换结果、位图及其他 CPU 能自然处理的数据。若对外可见的数据
 * 表示不能随 ABI 改变，请改用定宽类型。
 */
#ifdef ARCH_CPU_64BIT
typedef rt_int64_t                      rt_base_t;      /**< 有符号本机 CPU 字标量（64 位）。 */
typedef rt_uint64_t                     rt_ubase_t;     /**< 无符号本机 CPU 字标量（64 位）。 */
#else
typedef rt_int32_t                      rt_base_t;      /**< 有符号本机 CPU 字标量（32 位）。 */
typedef rt_uint32_t                     rt_ubase_t;     /**< 无符号本机 CPU 字标量（32 位）。 */
#endif

#if defined(RT_USING_LIBC) && !defined(RT_USING_NANO)
/* libc 提供大小和指针类型时，应与托管式/POSIX ABI 保持一致。 */
typedef size_t                          rt_size_t;      /**< 与 libc 兼容的无符号对象或缓冲区大小。 */
typedef ssize_t                         rt_ssize_t;     /**< 有符号字节计数；负值可用于报告错误。 */
typedef intptr_t                        rt_intptr_t;    /**< 可无损往返转换指针的有符号整数。 */
typedef uintptr_t                       rt_uintptr_t;   /**< 可无损往返转换指针的无符号整数。 */
#else
/*
 * 裸机环境中的对应类型保持一项重要约束：大小值和指针整数转换的宽度均为一个
 * 本机机器字。
 */
typedef rt_ubase_t                      rt_size_t;      /**< 裸机环境中的无符号对象或缓冲区大小。 */
typedef rt_base_t                       rt_ssize_t;     /**< 裸机环境中的有符号字节计数或错误载体。 */
typedef rt_base_t                      rt_intptr_t;    /**< 裸机环境中的有符号指针宽度整数。 */
typedef rt_ubase_t                       rt_uintptr_t;   /**< 裸机环境中的无符号指针宽度整数。 */
#endif /* defined(RT_USING_LIBC) && !defined(RT_USING_NANO) */

/*
 * 公共 API 使用的语义别名。rt_err_t 保存 RT_EOK 或由具体 API 定义正负号的错误码
 * （内核路径中同时存在正值和负值的 RT_E*）；rt_tick_t 按 2^32 取模回绕；
 * rt_flag_t 保存选项或条件位集合；rt_dev_t 标识设备；rt_off_t 表示有符号的
 * 文件定位偏移或内存偏移。
 */
typedef rt_base_t                       rt_err_t;       /**< RT_EOK 或 API 专用的有符号 RT-Thread 错误值。 */
typedef rt_uint32_t                     rt_tick_t;      /**< 会回绕的系统节拍计数或节拍间隔。 */
typedef rt_base_t                       rt_flag_t;      /**< 本机字宽的有符号选项或事件标志集合。 */
typedef rt_ubase_t                      rt_dev_t;       /**< 本机字宽的数值设备标识符。 */
typedef rt_base_t                       rt_off_t;       /**< 有符号的文件、设备或内存偏移。 */

/** @} */

/*
 * C11 之前的语言标准不支持原子语法。此处及早禁用请求的 C11 后端，使 rtatomic.h
 * 能改选硬件实现或中断屏蔽实现，而不会编译无效的 `_Atomic` 声明。
 */
#if defined(RT_USING_STDC_ATOMIC) && __STDC_VERSION__ < 201112L
#undef RT_USING_STDC_ATOMIC
#warning Not using C11 or beyond! Maybe you should change the -std option on your compiler
#endif

/**
 * @name 原子对象存储类型
 *
 * 这些别名描述 rtatomic.h 操作的存储对象。仅将变量声明为原子别名，并不会使
 * 任意 C 表达式自动成为原子操作：调用方必须使用 `rt_atomic_*` API。在 C11
 * 模式下，类型系统还会强制使用 `_Atomic` 方式访问。硬件和软件后端保留普通
 * 整数存储，并在各自的访问原语中保证原子性。
 *
 * C++ 中仍使用普通存储，因为受支持的嵌入式工具链并不能保证 C 的 `_Atomic`
 * 语法和 `<stdatomic.h>` 接口可移植地适用于 C++。
 * @{
 */
#ifdef __cplusplus
    typedef rt_uint8_t rt_atomic8_t;   /**< C++ 中通过移植层同步 API 访问的 8 位存储。 */
    typedef rt_uint16_t rt_atomic16_t; /**< C++ 中通过移植层同步 API 访问的 16 位存储。 */
    typedef rt_base_t rt_atomic_t;     /**< C++ 中通过移植层同步 API 访问的本机字宽存储。 */
#else
    #if defined(RT_USING_STDC_ATOMIC)
        #include <stdatomic.h>
        typedef _Atomic(rt_uint8_t) rt_atomic8_t;   /**< 值类型为 rt_uint8_t 的原子对象；大小和对齐由实现定义。 */
        typedef _Atomic(rt_uint16_t) rt_atomic16_t; /**< 值类型为 rt_uint16_t 的原子对象；大小和对齐由实现定义。 */
        typedef _Atomic(rt_base_t) rt_atomic_t;     /**< C11 本机字宽原子存储。 */
    #elif defined(RT_USING_HW_ATOMIC)
        typedef rt_uint8_t rt_atomic8_t;   /**< 通过 CPU 移植层原子辅助函数访问的 8 位存储。 */
        typedef rt_uint16_t rt_atomic16_t; /**< 通过 CPU 移植层原子辅助函数访问的 16 位存储。 */
        typedef rt_base_t rt_atomic_t;     /**< 通过 CPU 移植层原子辅助函数访问的本机字宽存储。 */
    #else
        typedef rt_uint8_t rt_atomic8_t;   /**< 由软件原子辅助函数保护的 8 位存储。 */
        typedef rt_uint16_t rt_atomic16_t; /**< 由软件原子辅助函数保护的 16 位存储。 */
        typedef rt_base_t rt_atomic_t;     /**< 由软件原子辅助函数保护的本机字宽存储。 */
    #endif /* RT_USING_STDC_ATOMIC */
#endif /* __cplusplus */

/** @} */

/* C API 通用的布尔常量：零表示假，一表示真。 */
#define RT_TRUE                         1               /**< 布尔真值。  */
#define RT_FALSE                        0               /**< 布尔假值。 */

/* 供 C 和旧版 C++ 工具链使用的可移植空指针常量。 */
#define RT_NULL                         0

/**
 * @brief 侵入式双向链表节点。
 *
 * 链表节点直接嵌入所属对象，无需额外分配包装结构。`next` 和 `prev` 连接相邻
 * 对象或循环链表的哨兵头节点。容器宏可由嵌入节点还原所属对象地址，因此一个对象
 * 可嵌入多个彼此独立的节点，同时加入多个链表。
 */
struct rt_list_node
{
    struct rt_list_node *next; /**< 遍历顺序中的下一节点，或哨兵头节点。 */
    struct rt_list_node *prev; /**< 遍历顺序中的上一节点，或哨兵头节点。 */
};
typedef struct rt_list_node rt_list_t; /**< 侵入式链表节点或头节点的公共简称。 */

/**
 * @brief 侵入式单向链表节点。
 *
 * 当无需反向遍历或在常数时间删除任意节点时，此形式可使每个节点少占用一个指针。
 * 它的所属对象和生命周期规则与 `rt_list_t` 相同。
 */
struct rt_slist_node
{
    struct rt_slist_node *next; /**< 下一节点；线性链表末尾为 RT_NULL。 */
};
typedef struct rt_slist_node rt_slist_t; /**< 单向链表节点或头节点的公共简称。 */

/**
 * @brief 原子单向链栈的节点或头节点存储。
 *
 * `next` 以本机字宽原子整数保存，因此可存放零或转换为 `rt_base_t` 的节点指针。
 * rtatomic.h 通过比较并交换操作压入和弹出节点。只有所选原子后端提供无锁的比较并
 * 交换时，该算法才是无锁的；软件后备实现可能借助中断或 CPU 锁将操作串行化。只要
 * 其他执行上下文仍可能观察到对象地址，对象就必须保持已分配状态。实现也没有加入
 * ABA 计数器，因此使用者必须按其并发模型制定合适的对象生命周期和复用规则。
 */
struct rt_lockless_slist_node
{
    rt_atomic_t next; /**< 下一节点指针的原子整数表示。 */
};
typedef struct rt_lockless_slist_node rt_ll_slist_t; /**< 原子单向链表节点或头节点类型。 */

/**
 * @name 自旋锁表示及诊断元数据
 *
 * 硬件/内核接口会声明实际执行加锁和解锁的函数。本节仅定义这些函数共用的对象布局，
 * 以及用于诊断锁所有权和临界区嵌套错误的可选记录信息。
 * @{
 */
#ifdef RT_USING_SMP
/* SMP 移植层在 cpuport.h 中定义实际的 CPU 间互斥原语。 */
#include <cpuport.h> /* 提供体系结构自旋锁定义。 */

/** 可由多个 CPU 并发执行时使用的自旋锁状态。 */
struct rt_spinlock
{
    rt_hw_spinlock_t lock; /**< 由体系结构定义的硬件原子锁字或锁状态。 */
#ifdef RT_USING_DEBUG
    rt_uint32_t critical_level; /**< 保存的临界区嵌套层级，解锁时用于校验。 */
#endif /* RT_USING_DEBUG */
#if defined(RT_DEBUGING_SPINLOCK)
    void *owner; /**< 记录为获取该锁的线程；仅用于诊断。 */
    void *pc;    /**< 获取该锁时捕获的调用点返回地址。 */
#endif /* RT_DEBUGING_SPINLOCK */
};

/*
 * 如果 rt_hw_spinlock_t 的布局不能使用常见的嵌套标量或聚合零初始化器，CPU
 * 移植层可覆盖此初始化器。
 */
#ifndef RT_SPINLOCK_INIT
#define RT_SPINLOCK_INIT {{0}} /* 可由 cpuport.h 覆盖。 */
#endif /* RT_SPINLOCK_INIT */

#else /* !RT_USING_SMP */

/**
 * 单处理器自旋锁状态。
 *
 * 不存在竞争的 CPU，因此通用 `rt_spin_lock*` 实现通过调度器临界区获取互斥；
 * 对 irqsave 操作则通过本地中断状态获取互斥。`lock` 是本机字宽的兼容或保留存储，
 * 而不是 CPU 间原子锁字；UP 实现无需访问它。
 */
struct rt_spinlock
{
#ifdef RT_USING_DEBUG
    rt_uint32_t critical_level; /**< 为诊断保存的临界区嵌套层级。 */
#endif /* RT_USING_DEBUG */
    rt_ubase_t lock; /**< 与 UP 兼容的保留锁存储；不是 CPU 间互斥原语。 */
};
/* 单处理器标量表示形式的静态初始化器。 */
#define RT_SPINLOCK_INIT {0}
#endif /* RT_USING_SMP */

/*
 * 可选的所有者跟踪仅在 SMP 构建中有意义。所有者和获取锁时的 PC 仅用于调试观察，
 * 不提供互斥功能，绝不能作为同步状态使用。
 */
#if defined(RT_DEBUGING_SPINLOCK) && defined(RT_USING_SMP)

    /* 解锁后写入的毒值，使残留的所有者信息更容易暴露。 */
    #define __OWNER_MAGIC ((void *)0xdeadbeaf)

    /* GCC 可低成本取得直接调用者；其他编译器则不报告调用地址。 */
    #if defined(__GNUC__)
    #define __GET_RETURN_ADDRESS __builtin_return_address(0)
    #else /* !__GNUC__ */
    #define __GET_RETURN_ADDRESS RT_NULL
    #endif /* __GNUC__ */

    /* 获取锁后立即记录当前线程及调用点。 */
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

    /* 标记该锁为无所有者，并清除上次获取锁的调用点。 */
    #define _SPIN_UNLOCK_DEBUG_OWNER(lock) \
        do                                 \
        {                                  \
            (lock)->owner = __OWNER_MAGIC; \
            (lock)->pc = RT_NULL;          \
        } while (0)

#else /* !RT_DEBUGING_SPINLOCK */

    /* 保持调用表达式有效，并抑制未使用参数的诊断。 */
    #define _SPIN_LOCK_DEBUG_OWNER(lock)    RT_UNUSED(lock)
    #define _SPIN_UNLOCK_DEBUG_OWNER(lock)  RT_UNUSED(lock)
#endif /* RT_DEBUGING_SPINLOCK */

/*
 * 临界层级诊断会配对记录加锁时观察到的嵌套层级，以及传回解锁路径的层级。这有助于
 * 检测锁作用域不匹配，或持锁期间临界区嵌套层级发生意外变化的问题。
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
    /* 未启用时仍保留相同的调用语法，但不生成元数据。 */
    #define _SPIN_LOCK_DEBUG_CRITICAL(lock)             RT_UNUSED(lock)
    #define _SPIN_UNLOCK_DEBUG_CRITICAL(lock, critical) do {critical = 0; RT_UNUSED(lock);} while (0)

#endif /* RT_DEBUGING_CRITICAL */

/** 执行所有已启用的加锁侧诊断记录操作。 */
#define RT_SPIN_LOCK_DEBUG(lock)         \
    do                                   \
    {                                    \
        _SPIN_LOCK_DEBUG_OWNER(lock);    \
        _SPIN_LOCK_DEBUG_CRITICAL(lock); \
    } while (0)

/** 执行所有已启用的解锁侧记录操作，并恢复保存的嵌套层级。 */
#define RT_SPIN_UNLOCK_DEBUG(lock, critical)         \
    do                                               \
    {                                                \
        _SPIN_UNLOCK_DEBUG_OWNER(lock);              \
        _SPIN_UNLOCK_DEBUG_CRITICAL(lock, critical); \
    } while (0)

/** 内核数据结构使用的公共自旋锁对象类型。 */
typedef struct rt_spinlock rt_spinlock_t;

/** 定义并静态初始化一个名为 @p x 的自旋锁对象。 */
#define RT_DEFINE_SPINLOCK(x)  struct rt_spinlock x = RT_SPINLOCK_INIT

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* __RT_TYPES_H__ */
