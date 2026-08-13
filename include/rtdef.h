/*
 * Copyright (c) 2006-2025 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2007-01-10     Bernard      初始版本
 * 2008-07-12     Bernard      移除所有 rt_int8、rt_uint32_t 等 typedef
 * 2010-10-26     yi.qiu       增加模块支持
 * 2010-11-10     Bernard      在线程退出时增加清理回调函数。
 * 2011-05-09     Bernard      在 GCC 4.x 中使用内建 va_arg
 * 2012-11-16     Bernard      将 RT_NULL 从 ((void*)0) 改为 0。
 * 2012-12-29     Bernard      调整 RT_USING_MEMPOOL 的位置，并增加
 *                             RT_USING_MEMHEAP 条件。
 * 2012-12-30     Bernard      为图形设备增加更多控制命令。
 * 2013-01-09     Bernard      修改版本号。
 * 2015-02-01     Bernard      将版本号改为 v2.1.0
 * 2017-08-31     Bernard      将版本号改为 v3.0.0
 * 2017-11-30     Bernard      将版本号改为 v3.0.1
 * 2017-12-27     Bernard      将版本号改为 v3.0.2
 * 2018-02-24     Bernard      将版本号改为 v3.0.3
 * 2018-04-25     Bernard      将版本号改为 v3.0.4
 * 2018-05-31     Bernard      将版本号改为 v3.1.0
 * 2018-09-04     Bernard      将版本号改为 v3.1.1
 * 2018-09-14     Bernard      为 RT-Thread Kernel 应用 Apache License v2.0
 * 2018-10-13     Bernard      将版本号改为 v4.0.0
 * 2018-10-02     Bernard      增加 64 位架构支持
 * 2018-11-22     Jesven       为 struct rt_thread 增加 smp 成员
 *                             增加 struct rt_cpu
 *                             增加 smp 相关宏
 * 2019-01-27     Bernard      将版本号改为 v4.0.1
 * 2019-05-17     Bernard      将版本号改为 v4.0.2
 * 2019-12-20     Bernard      将版本号改为 v4.0.3
 * 2020-08-10     Meco Man     为 struct rt_device_ops 增加宏
 * 2020-10-23     Meco Man     定义 IPC 类型的最大值
 * 2021-03-19     Meco Man     增加安全设备
 * 2021-05-10     armink       将版本号改为 v4.0.4
 * 2021-11-19     Meco Man     将版本号改为 v4.1.0
 * 2021-12-21     Meco Man     重新实现 RT_UNUSED
 * 2022-01-01     Gabriel      改进钩子机制
 * 2022-01-07     Gabriel      将部分 __on_rt_xxxxx_hook 移至专用 C 源文件
 * 2022-01-12     Meco Man     移除 RT_THREAD_BLOCK
 * 2022-04-20     Meco Man     将版本号改为 v4.1.1
 * 2022-04-21     THEWON       增加宏 RT_VERSION_CHECK
 * 2022-06-29     Meco Man     增加 RT_USING_LIBC 和标准 libc 头文件
 * 2022-08-16     Meco Man     将版本号改为 v5.0.0
 * 2022-09-12     Meco Man     定义 rt_ssize_t
 * 2022-12-20     Meco Man     为 rt_object 增加 const 名称
 * 2023-04-01     Chushicheng  将版本号改为 v5.0.1
 * 2023-05-20     Bernard      增加 stdc 原子操作检测。
 * 2023-09-15     xqyjlj       优化 rt_hw_interrupt_disable/enable
 * 2023-10-10     Chushicheng  将版本号改为 v5.1.0
 * 2023-10-11     zmshahaha    将特定设备相关代码和驱动移至 components/drivers
 * 2023-11-21     Meco Man     增加 RT_USING_NANO 宏
 * 2023-11-17     xqyjlj       增加进程组和会话支持
 * 2023-12-01     Shell        支持动态设备
 * 2023-12-18     xqyjlj       增加 rt_always_inline
 * 2023-12-22     Shell        支持钩子列表
 * 2024-01-18     Shell        将基础类型拆分到 rttypes.h
 *                             将编译器移植代码拆分到 rtcompiler.h
 * 2024-03-30     Meco Man     将版本号更新为 v5.2.0
 * 2025-11-10     Rbb666       将版本号更新为 v5.3.0
 */

#ifndef __RT_DEF_H__
#define __RT_DEF_H__

/**
 * @file rtdef.h
 * @brief RT-Thread 内核共享的、与具体配置无关的核心数据模型。
 *
 * 本头文件集中描述内核所操作的对象，包含版本编码、初始化导出元数据、
 * 对象类别标识符、定时器和线程控制块、IPC 对象、内存管理器元数据以及
 * 设备基类对象。这些类型的公开操作在 rtthread.h 中声明；调度器私有字段
 * 由 rtsched.h 注入；定宽整数和侵入式链表类型来自 rttypes.h。
 *
 * 本文件中的多数结构体采用 C 风格继承：派生对象的第一个字段是其父对象。
 * 例如，rt_thread 以 rt_object 开头；rt_semaphore 以 rt_ipc_object 开头，
 * 而 rt_ipc_object 自身又以 rt_object 开头。该布局使通用对象管理代码能够
 * 安全地将派生对象转换为其基类型。
 *
 * 许多字段受条件编译控制。字段是否存在以及结构体的二进制布局取决于目标 BSP
 * 的 rtconfig.h。绝不能在由不同配置构建的二进制文件之间交换内核对象。
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
 * RT-Thread 版本信息。
 *
 * RT_VERSION_CHECK() 将语义版本 X.Y.Z 映射为 X * 10000 + Y * 100 + Z。
 * 该单调递增整数用于预处理器比较；它不是打包位域，不能用移位或掩码解码。
 */
#define RT_VERSION_MAJOR                5               /**< 主版本号 (X.x.x) */
#define RT_VERSION_MINOR                3               /**< 次版本号 (x.X.x) */
#define RT_VERSION_PATCH                0               /**< 修订版本号 (x.x.X) */

/* 例如#if (RTTHREAD_VERSION >= RT_VERSION_CHECK(4, 1, 0) */
#define RT_VERSION_CHECK(major, minor, revise)          ((major * 10000U) + (minor * 100U) + revise)

/* RT-Thread 版本。 */
#define RTTHREAD_VERSION                RT_VERSION_CHECK(RT_VERSION_MAJOR, RT_VERSION_MINOR, RT_VERSION_PATCH)

/**@}*/

/*
 * ABI 可见计数器使用的最大值。启用 libc 的构建复用标准整数上限；
 * 独立运行的构建提供等价常量，而不依赖 <stdint.h> 的上限宏。
 */
#ifdef RT_USING_LIBC
#define RT_UINT8_MAX                    UINT8_MAX       /**< UINT8 可表示的最大值 */
#define RT_UINT16_MAX                   UINT16_MAX      /**< UINT16 可表示的最大值 */
#define RT_UINT32_MAX                   UINT32_MAX      /**< UINT32 可表示的最大值 */
#define RT_UINT64_MAX                   UINT64_MAX      /**< UINT64 可表示的最大值 */
#else
#define RT_UINT8_MAX                    0xFFU                 /**< UINT8 可表示的最大值 */
#define RT_UINT16_MAX                   0xFFFFU               /**< UINT16 可表示的最大值 */
#define RT_UINT32_MAX                   0xFFFFFFFFUL          /**< UINT32 可表示的最大值 */
#define RT_UINT64_MAX                   0xFFFFFFFFFFFFFFFFULL /**< UINT64 可表示的最大值 */
#endif /* RT_USING_LIBC */

#define RT_TICK_MAX                     RT_UINT32_MAX   /**< tick 可表示的最大值 */

/*
 * IPC 计数器的公开上限。多数上限对应当前字段宽度；尽管当前互斥量控制块
 * 没有 `value` 字段而使用 `hold`，仍保留 RT_MUTEX_VALUE_MAX 以保持兼容。
 * 运行时 API 可能施加更严格的限制。
 */
#define RT_SEM_VALUE_MAX                RT_UINT16_MAX   /**< 信号量 .value 的最大值 */
#define RT_MUTEX_VALUE_MAX              RT_UINT16_MAX   /**< 旧版互斥量 value 的兼容性上限。 */
#define RT_MUTEX_HOLD_MAX               RT_UINT8_MAX    /**< 互斥量 .hold 的最大值 */
#define RT_MB_ENTRY_MAX                 RT_UINT16_MAX   /**< 邮箱 .entry 的最大值 */
#define RT_MQ_ENTRY_MAX                 RT_UINT16_MAX   /**< 消息队列 .entry 的最大值 */

/* 通用工具。 */

/** 显式标记表达式有意未使用，且不会对其求值两次。 */
#define RT_UNUSED(x)                   ((void)(x))

/**
 * 可供 C11 之前的编译器使用的编译期断言。
 *
 * 假表达式会创建长度为负数的数组，从而导致编译错误。@p name 会成为生成的
 * typedef 的一部分，因此同一作用域内的每个断言必须使用唯一标识符。
 */
#define RT_STATIC_ASSERT(name, expn) typedef char _static_assert_##name[(expn)?1:-1]

/* 与编译器相关的定义。 */
#include "rtcompiler.h"

/**
 * @name 自动初始化导出
 *
 * INIT_EXPORT() 将函数指针以及可选的诊断元数据放入链接器段；该段后缀为
 * 文本形式的 @p level。链接脚本保留并排序 .rti_fn.* 段；启动代码随后按
 * 级别的字典顺序遍历得到的范围。
 *
 * 导出的初始化函数签名为 `int fn(void)`。板级条目在调度器启动前运行；其余
 * 组件级别通常由主初始化线程运行。禁用 RT_USING_COMPONENTS_INIT 时，这些宏
 * 展开为空，因此导出函数本身并不能保证它出现在某个固件镜像中。
 *
 * MSVC 无法使用与 GCC 兼容编译器相同的 ELF 风格段属性，因此它在公共段中保存
 * 显式级别字符串。启用 RT_DEBUGING_AUTO_INIT 时，还会保留函数名以便诊断。
 * @{ */
#ifdef RT_USING_COMPONENTS_INIT
/** 每个自动导出的初始化函数都必须符合的原型。 */
typedef int (*init_fn_t)(void);
#ifdef _MSC_VER
#pragma section("rti_fn$f",read)
    #ifdef RT_DEBUGING_AUTO_INIT
        struct rt_init_desc
        {
            const char* level;       /**< 文本排序键，例如 ".rti_fn.3"。 */
            const init_fn_t fn;      /**< 要调用的初始化函数。 */
            const char* fn_name;     /**< 为启动诊断保留的函数名。 */
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
            const char* level;       /**< MSVC 启动遍历器使用的文本排序键。 */
            const init_fn_t fn;      /**< 要调用的初始化函数。 */
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
            const char* fn_name;     /**< 为启动诊断保留的函数名。 */
            const init_fn_t fn;      /**< 放入此描述符链接器段的初始化函数。 */
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

/* 板级阶段例程由 rt_components_board_init() 调用。 */
#define INIT_BOARD_EXPORT(fn)           INIT_EXPORT(fn, "1")

/* 核心设施：CPU、内存、中断控制器和基础总线。 */
#define INIT_CORE_EXPORT(fn)            INIT_EXPORT(fn, "1.0")
/* 后续驱动所需的子系统：系统定时器、时钟和引脚控制。 */
#define INIT_SUBSYS_EXPORT(fn)          INIT_EXPORT(fn, "1.1")
/* 平台专用服务及其他较晚执行的板级阶段代码。 */
#define INIT_PLATFORM_EXPORT(fn)        INIT_EXPORT(fn, "1.2")

/* 以下级别通常在 main_thread_entry() 中运行。 */
/* 不依赖已初始化设备的纯软件准备工作。 */
#define INIT_PREV_EXPORT(fn)            INIT_EXPORT(fn, "2")
/* 设备注册和硬件驱动初始化。 */
#define INIT_DEVICE_EXPORT(fn)          INIT_EXPORT(fn, "3")
/* DFS、协议栈等中间件。 */
#define INIT_COMPONENT_EXPORT(fn)       INIT_EXPORT(fn, "4")
/* 运行环境设置，例如挂载存储设备。 */
#define INIT_ENV_EXPORT(fn)             INIT_EXPORT(fn, "5")
/* 依赖运行环境的应用服务。 */
#define INIT_APP_EXPORT(fn)             INIT_EXPORT(fn, "6")

/* 明确要求文件系统已挂载的初始化。 */
#define INIT_FS_EXPORT(fn)              INIT_EXPORT(fn, "6.0")
/*
 * rt_dm_secondary_cpu_init() 遍历的每个次级 CPU 初始化项；BSP/架构的次级
 * CPU 启动路径决定何时调用该遍历器。
 */
#define INIT_SECONDARY_CPU_EXPORT(fn)   INIT_EXPORT(fn, "7")
/** @} */

#if !defined(RT_USING_FINSH)
/* 即使未包含 finsh.h 文件，也将这些宏定义为空。 */
#define FINSH_FUNCTION_EXPORT(name, desc)
#define FINSH_FUNCTION_EXPORT_ALIAS(name, alias, desc)

#define MSH_CMD_EXPORT(command, desc)
#define MSH_CMD_EXPORT_ALIAS(command, alias, desc)
#elif !defined(FINSH_USING_SYMTAB)
#define FINSH_FUNCTION_EXPORT_CMD(name, cmd, desc)
#endif

/** 一个 rt_event 对象包含的事件位数量。 */
#define RT_EVENT_LENGTH                 32

/*
 * slab 分配器和虚拟内存/MMU 组件共用的默认页几何参数。掩码和移位假定
 * 4096 字节等于 1 << 12。
 */
#define RT_MM_PAGE_SIZE                 4096
#define RT_MM_PAGE_MASK                 (RT_MM_PAGE_SIZE - 1)
#define RT_MM_PAGE_BITS                 12

/*
 * 内核对象创建时使用的分配间接层。移植层或受保护构建可在包含本文件前重定义
 * 这些宏，将内核元数据分配到专用分配器；默认使用系统堆 API。
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
 * 返回真(1)或假(0)。
 *     RT_IS_ALIGN(128, 4) 用于判断 128 是否按 4 对齐。
 *     结果为 1，表示 128 按 4 对齐。
 * @note 地址为 NULL 时返回假(0)。
 * @note @p align must be a nonzero power of two.  @p addr may be evaluated
 *       两次（第二次检查可能短路），因此应传入无副作用的整数或指针宽度表达式。
 */
#define RT_IS_ALIGN(addr, align) ((!(addr & (align - 1))) && (addr != RT_NULL))

/**
 * @ingroup group_basic_definition
 *
 * @def RT_ALIGN(size, align)
 * 返回不小于指定大小、且按指定宽度对齐的最小连续大小。RT_ALIGN(13, 4)
 * 返回 16。
 * @note align 必须是 2 的整数次幂，否则结果不正确。
 * @note @p align is expanded more than once; do not pass an expression with
 *       自增、函数调用或其他副作用。若 @p size 过于接近可表示最大值，加法可能溢出回绕。
 */
#define RT_ALIGN(size, align)           (((size) + (align) - 1) & ~((align) - 1))

/**
 * @ingroup group_basic_definition
 *
 * @def RT_ALIGN_DOWN(size, align)
 * 返回不大于指定大小、且按指定宽度向下对齐的值。RT_ALIGN_DOWN(13, 4)
 * 返回 12。
 * @note align 必须是 2 的整数次幂，否则结果不正确。
 * @note 请使用无副作用参数；本宏不执行运行时校验。
 */
#define RT_ALIGN_DOWN(size, align)      ((size) & ~((align) - 1))

/**
 * @addtogroup group_object_management
 * @{
 */

/* 内核对象标志位存放在 rt_object::flag 字节中。 */
#define RT_OBJECT_FLAG_MODULE           0x80            /**< 表示模块对象。 */

/**
 * @brief 嵌入每个受管理内核对象起始位置的通用头部。
 *
 * 对象管理器通常将已初始化实例链接到由 @ref rt_object_class_type 选择的类别
 * 容器。启用 RT_USING_MODULE 时，当前可加载模块创建的对象改为链接到该模块的私有
 * 对象列表。通用 rt_object_init() 路径会在 `type` 的高位设置
 * RT_Object_Class_Static，并与 detach 配对；rt_object_allocate() 保持该位清零，
 * 并与 delete 配对。此属性标识对象的初始化和生命周期路径，而不必然表示底层存储的
 * 物理来源，因为类别包装器可定义自己的分配和销毁顺序。直接修改 `type` 或 `list`
 * 会破坏注册表或生命周期状态，必须使用对象/类别 API。
 */
struct rt_object
{
#if RT_NAME_MAX > 0
    char        name[RT_NAME_MAX];                       /**< 行内存储的 NUL 结尾名称；过长输入会被截断。 */
#else
    const char *name;                                    /**< 借用的名称指针；其存储期必须长于对象。 */
#endif /* RT_NAME_MAX > 0 */
    rt_uint8_t  type;                                    /**< 类别值加上静态对象所有权位。 */
    rt_uint8_t  flag;                                    /**< 类别专用标志；模块位为全局保留位。 */

#ifdef RT_USING_MODULE
    void      * module_id;                               /**< 所属可加载模块，用于回收模块资源。 */
#endif /* RT_USING_MODULE */

#ifdef RT_USING_SMART
    rt_atomic_t lwp_ref_count;                           /**< 代表 RT-Smart LWP 持有的原子引用数。 */
#endif /* RT_USING_SMART */

    rt_list_t   list;                                    /**< 类别全局或模块私有对象链表中的节点。 */
};
typedef struct rt_object *rt_object_t;                   /**< 内核对象类型。 */

/**
 * @brief rt_object_for_each() 使用的访问回调函数。
 *
 * @param object 所选类别容器中的当前对象。
 * @param data 由 rt_object_for_each() 直接传递的不透明调用方上下文。
 * @return 返回 RT_EOK 继续；返回正值表示成功停止；返回负 RT-Thread 错误表示停止并报告失败。
 *
 * rt_object_for_each() 持有所选类别注册表自旋锁时调用该回调。回调必须在有限时间内
 * 完成，不得阻塞，也不得调用会获取同一注册表锁或改变当前对象注册表成员关系的对象 API。
 */
typedef rt_err_t (*rt_object_iter_t)(rt_object_t object, void *data);

/**
 * @brief 存储在 rt_object::type 中的运行时类别标记。
 *
 * 低七位选择逻辑对象类别。值 RT_Object_Class_Static 是与类别值按位或的所有权属性，
 * 而非独立对象类别；测试类别时调用者应进行掩码处理或使用对象 API。部分枚举值仅在
 * 编译了对应功能时才有意义。
 *
 * 启用相应宏时，对象类型可以是以下之一：
 *  - 线程、信号量、互斥量、事件、邮箱、消息队列、内存堆、内存池、设备或定时器
 *  - 模块、内存、通道、进程组、会话或自定义对象
 *  - 未知对象
 *  - 静态所有权属性（不是独立逻辑类别）
 */
enum rt_object_class_type
{
    RT_Object_Class_Null          = 0x00,      /**< 对象未被使用。 */
    RT_Object_Class_Thread        = 0x01,      /**< 对象是线程。 */
    RT_Object_Class_Semaphore     = 0x02,      /**< 对象是信号量。 */
    RT_Object_Class_Mutex         = 0x03,      /**< 对象是互斥量。 */
    RT_Object_Class_Event         = 0x04,      /**< 对象是事件。 */
    RT_Object_Class_MailBox       = 0x05,      /**< 对象是邮箱。 */
    RT_Object_Class_MessageQueue  = 0x06,      /**< 对象是消息队列。 */
    RT_Object_Class_MemHeap       = 0x07,      /**< 对象是内存堆。 */
    RT_Object_Class_MemPool       = 0x08,      /**< 对象是内存池。 */
    RT_Object_Class_Device        = 0x09,      /**< 对象是设备。 */
    RT_Object_Class_Timer         = 0x0a,      /**< 对象是定时器。 */
    RT_Object_Class_Module        = 0x0b,      /**< 对象是模块。 */
    RT_Object_Class_Memory        = 0x0c,      /**< 对象是内存。 */
    RT_Object_Class_Channel       = 0x0d,      /**< 对象是通道。 */
    RT_Object_Class_ProcessGroup  = 0x0e,      /**< 对象是进程组。 */
    RT_Object_Class_Session       = 0x0f,      /**< 对象是会话。 */
    RT_Object_Class_Custom        = 0x10,      /**< 对象是自定义对象。 */
    RT_Object_Class_Unknown       = 0x11,      /**< 对象类别未知。 */
    RT_Object_Class_Static        = 0x80       /**< 对象是静态对象。 */
};

/**
 * @brief 通用对象管理器内部使用的每类别注册表。
 *
 * 每个已启用类别各有一个描述符。object_list 是全局已注册实例的哨兵节点（可加载
 * 模块拥有的对象可能保存在其私有链表中）；object_size 是动态对象的分配大小；
 * spinlock 使全局注册表的修改和遍历串行化。
 */
struct rt_object_information
{
    enum rt_object_class_type type;                     /**< 此注册表表示的类别。 */
    rt_list_t                 object_list;              /**< 此类别全局注册对象的哨兵节点。 */
    rt_size_t                 object_size;              /**< rt_object_allocate() 分配的字节数。 */
    struct rt_spinlock        spinlock;                 /**< 保护 object_list 和注册表操作。 */
};

/**
 * @brief 启用钩子支持时调用单个函数指针钩子。
 *
 * @param func 钩子变量，不是保证存在的函数名。
 * @param argv 括号包围的参数元组，例如 `(thread)`。
 *
 * 双层宏使 @p func 能在分派前展开。
 * RT_HOOK_USING_FUNC_PTR selects the legacy single-listener implementation;
 * otherwise the call is compiled out and a hook-list point may be used.  Hook
 * 钩子代码在调用方上下文中同步执行，该上下文可能是 ISR 或调度器锁定区域，必须遵守
 * 该调用点的限制。
 */
#ifndef RT_USING_HOOK
#define RT_OBJECT_HOOK_CALL(func, argv)

#else

/**
 * @brief 在例程中添加钩子点。
 * @note 用法：
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
 * @brief 为多监听器钩子声明类型和注册 API。
 *
 * The generated node contains the typed handler and an intrusive list node.
 * Callers own the node storage and must keep it alive while registered.  The
 * generated `name_nested` counter prevents registration/removal while a hook
 * traversal is in progress.  `handler` is the callback to invoke and
 * `list_node` links this caller-owned record into the hook point's listener
 * list; applications must treat both as registered-state metadata.
 *
 * @note 用法：
 * 此宏通常在头文件中使用。在 foo.h 中可按如下方式使用：
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
 * @brief 定义并静态初始化一个由调用方拥有的钩子列表节点。
 *
 * @note 用法
 * 可以按如下方式添加钩子。
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
 * 此外，也可在以下路径找到示例代码：
 * `examples/utest/testcases/kernel/hooklist_tc.c`.
 */
#define RT_OBJECT_HOOKLIST_DEFINE_NODE(hookname, nodename, hooker_handler) \
    struct hookname##_hooklistnode nodename = {                            \
        .handler = hooker_handler,                                         \
        .list_node = RT_LIST_OBJECT_INIT(nodename.list_node),              \
    };

/**
 * @brief 定义钩子列表、其锁及其注册函数。
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
 * @brief 按链表顺序调用每个已注册监听器。
 *
 * The nested counter protects list topology, but handlers are deliberately
 * called without holding the list spinlock.  This avoids executing arbitrary
 * hook code with interrupts disabled.  It also means handlers run in the
 * original call site's context and must provide their own protection for data
 * they share with other threads or CPUs.
 *
 * @note 用法：
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
 * 存储在 rt_timer::parent.flag 中的定时器标志布局。
 *
 * 活动位由定时器子系统维护。周期性和
 * 执行上下文位描述定时器策略。通常硬定时器回调在 tick/中断侧的定时器检查中运行，
 * 因此不得阻塞；软定时器在定时器服务线程中运行。启用 RT_USING_TIMER_ALL_SOFT 时，
 * 所有定时器（包括携带值为零的 HARD 标志者）都放入软定时器链表，并由该工作线程
 * 分派。RT_TIMER_FLAG_THREAD_TIMER 标识状态直接与调度器协调的每线程超时定时器。
 */
#define RT_TIMER_FLAG_DEACTIVATED       0x0             /**< 定时器未激活 */
#define RT_TIMER_FLAG_ACTIVATED         0x1             /**< 定时器已激活 */
#define RT_TIMER_FLAG_ONE_SHOT          0x0             /**< 单次定时器 */
#define RT_TIMER_FLAG_PERIODIC          0x2             /**< 周期定时器 */

#define RT_TIMER_FLAG_HARD_TIMER        0x0             /**< 硬定时器；其回调函数在 tick ISR 中调用。 */
#define RT_TIMER_FLAG_SOFT_TIMER        0x4             /**< 软定时器；其回调函数在定时器线程中调用。 */
#define RT_TIMER_FLAG_THREAD_TIMER \
    (0x8 | RT_TIMER_FLAG_HARD_TIMER)                    /**< 直接与调度器协作的线程定时器 */

#define RT_TIMER_CTRL_SET_TIME          0x0             /**< 设置定时器控制命令 */
#define RT_TIMER_CTRL_GET_TIME          0x1             /**< 获取定时器控制命令 */
#define RT_TIMER_CTRL_SET_ONESHOT       0x2             /**< 将定时器改为单次模式 */
#define RT_TIMER_CTRL_SET_PERIODIC      0x3             /**< 将定时器改为周期模式 */
#define RT_TIMER_CTRL_GET_STATE         0x4             /**< 获取定时器激活或未激活状态 */
#define RT_TIMER_CTRL_GET_REMAIN_TIME   0x5             /**< 获取剩余等待时间 */
#define RT_TIMER_CTRL_GET_FUNC          0x6             /**< 获取定时器超时函数 */
#define RT_TIMER_CTRL_SET_FUNC          0x7             /**< 设置定时器超时函数 */
#define RT_TIMER_CTRL_GET_PARM          0x8             /**< 获取定时器参数 */
#define RT_TIMER_CTRL_SET_PARM          0x9             /**< 设置定时器参数 */

#ifndef RT_TIMER_SKIP_LIST_LEVEL
/** 每个定时器对象内嵌的有序链表层数。 */
#define RT_TIMER_SKIP_LIST_LEVEL          1
#endif

/* 控制定时器跳表层级提升的掩码；通常为 1 或 3。 */
#ifndef RT_TIMER_SKIP_LIST_MASK
#define RT_TIMER_SKIP_LIST_MASK         0x3             /**< 定时器跳表掩码 */
#endif

/**
 * @brief 定时器到期回调函数。
 *
 * @param parameter 初始化定时器时提供的不透明值。
 *
 * 通常 HARD/SOFT 标志选择中断或定时器工作线程上下文；
 * RT_USING_TIMER_ALL_SOFT overrides that choice and dispatches every callback
 * 在工作线程中分派所有回调。分派定时器回调期间不得分离、删除、释放或以其他方式使该
 * 定时器失效：分派器会在回调返回后调用退出钩子并检查定时器状态。其 API 允许停止或
 * 重新配置仍然有效的定时器。
 */
typedef void (*rt_timer_func_t)(void *parameter);

/**
 * @brief 内核定时器控制块。
 *
 * 定时器按绝对 timeout_tick 排列在一个或多个侵入式链表中。init_tick 保存调用方
 * 请求的相对间隔；每次启动定时器都会重新计算 timeout_tick。周期定时器在安排下次
 * 到期时使用相同间隔。
 */
struct rt_timer
{
    struct rt_object parent;                            /**< 继承自 rt_object */

    rt_list_t        row[RT_TIMER_SKIP_LIST_LEVEL];    /**< 有序定时器跳表各层的节点。 */

    rt_timer_func_t  timeout_func;                      /**< 超时函数 */
    void             *parameter;                        /**< 超时函数的参数 */

    rt_tick_t        init_tick;                         /**< 以系统 tick 为单位的相对延迟或周期。 */
    rt_tick_t        timeout_tick;                      /**< 当前激活将在该绝对 tick 到期。 */
};
typedef struct rt_timer *rt_timer_t;

/**@}*/

/**
 * @addtogroup group_signal
 */
/**@{*/

#ifdef RT_USING_SIGNALS
/** rt_sigset_t 可表示的传统内核线程信号最大数量。 */
#define RT_SIG_MAX          32
/** 待处理或被屏蔽的传统信号位集合。 */
typedef unsigned long rt_sigset_t;
/** 复用已配置 C/POSIX 环境中的信号信息类型。 */
typedef siginfo_t rt_siginfo_t;
/** 为 @p signo 执行的传统单参数信号处理函数。 */
typedef void (*rt_sighandler_t)(int signo);
#endif /* RT_USING_SIGNALS */
/**@}*/

/**
 * @addtogroup group_thread_management
 * @{
 */

/*
 * 线程。
 */

/*
 * 存储在调度器上下文 stat 字节中的线程状态编码。
 * 低三位保存生命周期/挂起状态；高位保存相互独立的让出和信号状态。应通过
 * RT_THREAD_STAT_MASK 比较低位状态，而非直接比较完整字节。
 */
#define RT_THREAD_INIT                       0x00                /**< 初始化状态 */
#define RT_THREAD_CLOSE                      0x01                /**< 关闭状态 */
#define RT_THREAD_READY                      0x02                /**< 就绪状态 */
#define RT_THREAD_RUNNING                    0x03                /**< 运行状态 */

/*
 * rt_thread_suspend_with_flag() 接受的面向用户的挂起策略。
 * 它控制 RT-Smart 线程阻塞期间哪些信号类别可以唤醒它；这些值会转换为下方的编码状态。
 */
enum
{
    RT_INTERRUPTIBLE = 0, /**< 普通信号可以中断等待。 */
    RT_KILLABLE,          /**< 仅终止类信号可以中断等待。 */
    RT_UNINTERRUPTIBLE,   /**< 信号不能中断等待。 */
};

#define RT_THREAD_SUSPEND_MASK               0x04
#define RT_SIGNAL_COMMON_WAKEUP_MASK         0x02
#define RT_SIGNAL_KILL_WAKEUP_MASK           0x01

#define RT_THREAD_SUSPEND_INTERRUPTIBLE      (RT_THREAD_SUSPEND_MASK)                                                             /**< 挂起可中断0x4 */
#define RT_THREAD_SUSPEND                    RT_THREAD_SUSPEND_INTERRUPTIBLE
#define RT_THREAD_SUSPEND_KILLABLE           (RT_THREAD_SUSPEND_MASK | RT_SIGNAL_COMMON_WAKEUP_MASK)                              /**< 挂起并可杀死 0x6 */
#define RT_THREAD_SUSPEND_UNINTERRUPTIBLE    (RT_THREAD_SUSPEND_MASK | RT_SIGNAL_COMMON_WAKEUP_MASK | RT_SIGNAL_KILL_WAKEUP_MASK) /**< 以不间断的 0x7 挂起 */
#define RT_THREAD_STAT_MASK                  0x07

#define RT_THREAD_STAT_YIELD            0x08                /**< 表示 remaining_tick 自上次调度以来是否已重新加载 */
#define RT_THREAD_STAT_YIELD_MASK       RT_THREAD_STAT_YIELD

#define RT_THREAD_STAT_SIGNAL           0x10                /**< 任务保持信号 */
#define RT_THREAD_STAT_SIGNAL_READY     (RT_THREAD_STAT_SIGNAL | RT_THREAD_READY)
#define RT_THREAD_STAT_SIGNAL_WAIT      0x20                /**< 任务正在等待信号 */
#define RT_THREAD_STAT_SIGNAL_PENDING   0x40                /**< 信号已被持有且尚未被处理 */
#define RT_THREAD_STAT_SIGNAL_MASK      0xf0

/**
 * 线程控制命令定义
 */
#define RT_THREAD_CTRL_STARTUP          0x00                /**< 启动线程。 */
#define RT_THREAD_CTRL_CLOSE            0x01                /**< 关闭线程。 */
#define RT_THREAD_CTRL_CHANGE_PRIORITY  0x02                /**< 更改线程优先级。 */
#define RT_THREAD_CTRL_INFO             0x03                /**< 获取线程信息。 */
#define RT_THREAD_CTRL_BIND_CPU         0x04                /**< 设置线程绑定cpu。 */
#define RT_THREAD_CTRL_RESET_PRIORITY   0x05                /**< 重置线程优先级。 */

/**
 * @brief 累计 CPU 执行时间类别。
 *
 * 值是架构定义的会计单位，通常是调度程序刻度。它们是累积计数器而不是百分比；当启用 CPU 使用情况跟踪时，每个线程的最近百分比源自快照。
 */
struct rt_cpu_usage_stats
{
    rt_ubase_t user;       /**< 执行非特权/用户代码所花费的时间。 */
    rt_ubase_t system;     /**< 执行特权内核代码所花费的时间。 */
    rt_ubase_t irq;        /**< 保留IRQ/异常槽；通用刻度会计目前保持不变。 */
    rt_ubase_t idle;       /**< 每个 CPU 空闲线程花费的时间。 */
};
typedef struct rt_cpu_usage_stats *rt_cpu_usage_stats_t;

#ifdef RT_USING_SMP

#define RT_CPU_DETACHED                 RT_CPUS_NR          /**< 线程未在 cpu 上运行。 */
#define RT_CPU_MASK                     ((1 << RT_CPUS_NR) - 1) /**< 所有 CPU 掩码位。 */

#ifndef RT_SCHEDULE_IPI
/** 用于请求在另一个 CPU 上重新调度的处理器间中断。 */
#define RT_SCHEDULE_IPI                 0
#endif /* RT_SCHEDULE_IPI */

#ifndef RT_STOP_IPI
/** 架构的 CPU-stop 协议使用的处理器间中断。 */
#define RT_STOP_IPI                     1
#endif /* RT_STOP_IPI */

#ifndef RT_SMP_CALL_IPI
/** 用于在远程 CPU 上执行函数的处理器间中断。 */
#define RT_SMP_CALL_IPI                 2
#endif

#define RT_MAX_IPI                      3

#define _SCHEDULER_CONTEXT(fileds) fileds

/**
 * @brief 用于 SMP 构建的 Per-CPU 调度程序和中断簿记。
 *
 * 调度程序字段对其所属的 CPU 而言是私有的。  本地核心在 RT-Thread 临界区中访问它们；未定义不同步的远程访问。  没有 CPU 绑定的线程也可以驻留在调度程序的全局就绪队列中，该队列在此结构之外进行维护。
 */
struct rt_cpu
{
    /**
 * 受以下保护：
 * - 其他核心：从其他核心访问是未定义的行为
 * - 本地核心：rt_enter_critical()/rt_exit_critical()
 */
    _SCHEDULER_CONTEXT(
        struct rt_thread        *current_thread;       /**< 当前在此 CPU 上执行的线程。 */

        rt_uint8_t              irq_switch_flag:1;     /**< 推迟请求的切换直到中断返回。 */
        rt_uint8_t              sched_lock_flag:1;     /**< CPU 当前拥有调度程序序列化。 */
#ifndef ARCH_USING_HW_THREAD_SELF
        rt_uint8_t              critical_switch_flag:1; /**< 切换被关键部分推迟。 */
#endif /* ARCH_USING_HW_THREAD_SELF */

        rt_uint8_t              current_priority;      /**< 为该 CPU 的运行线程记录的有效优先级。 */
        rt_list_t               priority_table[RT_THREAD_PRIORITY_MAX]; /**< 按优先级的就绪列表哨兵。 */
    #if RT_THREAD_PRIORITY_MAX > 32
        rt_uint32_t             priority_group;        /**< 标识非空就绪组的顶级位图。 */
        rt_uint8_t              ready_table[32];       /**< 每组中优先级的二级位图。 */
    #else
        rt_uint32_t             priority_group;        /**< 每个非空 CPU 优先级队列一位。 */
    #endif /* RT_THREAD_PRIORITY_MAX > 32 */

        rt_atomic_t             tick;                   /**< 在此 CPU 上观察到的蜱计数。 */
    );

    struct rt_thread            *idle_thread;           /**< 绑定到此 CPU 的最低优先级空闲线程。 */
    rt_atomic_t                 irq_nest;               /**< CPU 上的当前中断嵌套深度。 */

#ifdef RT_USING_SMART
    struct rt_spinlock          spinlock;               /**< RT-Smart 针对每个 CPU 状态的特定保护。 */
#endif /* RT_USING_SMART */
#ifdef RT_USING_CPU_USAGE_TRACER
    struct rt_cpu_usage_stats   cpu_stat;               /**< 累计 CPU 使用量统计。 */
#endif /* RT_USING_CPU_USAGE_TRACER */
#ifdef ARCH_USING_IRQ_CTX_LIST
    rt_slist_t                  irq_ctx_head;           /**< 嵌套架构 IRQ 上下文的堆栈/列表。 */
#endif /* ARCH_USING_IRQ_CTX_LIST */
};

#else /* !RT_USING_SMP */
struct rt_cpu
{
    struct rt_thread            *current_thread;        /**< 当前在 UP 构建中执行线程。 */
    struct rt_thread            *idle_thread;           /**< 系统空闲线程。 */

#ifdef RT_USING_CPU_USAGE_TRACER
    struct rt_cpu_usage_stats   cpu_stat;               /**< 累计 CPU 使用量统计。 */
#endif /* RT_USING_CPU_USAGE_TRACER */
#ifdef ARCH_USING_IRQ_CTX_LIST
    rt_slist_t                  irq_ctx_head;           /**< 嵌套架构中断上下文。 */
#endif /* ARCH_USING_IRQ_CTX_LIST */
};

#endif /* RT_USING_SMP */

typedef struct rt_cpu *rt_cpu_t;
/* 只读兼容拼写：应用程序获取但不能分配当前线程。 */
#define rt_current_thread rt_thread_self()

struct rt_thread;

/**
 * @brief 架构中断/异常上下文描述符。
 *
 * 启用 ARCH_USING_IRQ_CTX_LIST 的端口推送这些描述符，因此嵌套异常可以是 inspected（例如通过诊断或回溯代码）。
 */

typedef struct rt_interrupt_context {
    void *context;      /**< 指向体系结构定义的已保存寄存器帧的指针。 */
    rt_slist_t node;    /**< 当前 CPU 的嵌套 IRQ 上下文列表中的侵入节点。 */
} *rt_interrupt_context_t;

#ifdef RT_USING_SMART
/**
 * RT-Smart 等待对象唤醒适配器。
 *
 * 阻塞子系统安装一个回调，该回调知道如何将 @p thread 与其私有等待对象分离。  该回调返回 RT-Thread 状态，并在异步进程事件需要唤醒阻塞的用户线程时使用。
 */
typedef rt_err_t (*rt_wakeup_func_t)(void *object, struct rt_thread *thread);

/** 回调加上与一个阻塞线程关联的不透明等待对象数据。 */
struct rt_wakeup
{
    rt_wakeup_func_t func; /**< 执行唤醒的子系统特定操作。 */
    void *user_data;       /**< 等待对象作为回调的第一个参数传递。 */
};

/* RT-Smart 支持 64 个进程级信号号。 */
#define _LWP_NSIG       64

#ifdef ARCH_CPU_64BIT
#define _LWP_NSIG_BPW   64
#else
#define _LWP_NSIG_BPW   32
#endif

#define _LWP_NSIG_WORDS (RT_ALIGN(_LWP_NSIG, _LWP_NSIG_BPW) / _LWP_NSIG_BPW)

/** 传统的单参数用户空间信号处理程序。 */
typedef void (*lwp_sighandler_t)(int);
/** SA_SIGINFO 风格的用户空间处理程序接收扩展信号上下文。 */
typedef void (*lwp_sigaction_t)(int signo, siginfo_t *info, void *context);

/** 固定大小的信号位图分割成本地字块。 */
typedef struct
{
    unsigned long sig[_LWP_NSIG_WORDS]; /**< Bit N-1代表信号编号N。 */
} lwp_sigset_t;

#if _LWP_NSIG <= 64
#define lwp_sigmask(signo)      ((lwp_sigset_t){.sig = {[0] = ((long)(1u << ((signo)-1)))}})
#define lwp_sigset_init(mask)   ((lwp_sigset_t){.sig = {[0] = (long)(mask)}})
#endif /* _LWP_NSIG <= 64 */

/** 由 RT-Smart 进程安装的每个信号操作。 */
struct lwp_sigaction
{
    union
    {
        void (*_sa_handler)(int);                    /**< 传统的 sa_handler 回调。 */
        void (*_sa_sigaction)(int, siginfo_t *, void *); /**< 扩展 SA_SIGINFO 回调。 */
    } __sa_handler;
    lwp_sigset_t sa_mask;                            /**< 回调期间阻止额外信号。 */
    int sa_flags;                                    /**< POSIX 样式 SA_* 行为标志。 */
    void (*sa_restorer)(void);                       /**< 可选的用户空间信号返回蹦床。 */
};

/** 可选的特定于信号的有效负载与公共元数据分开存储。 */
typedef struct lwp_siginfo_ext
{
    union
    {
        /* 对于 SIGCHLD */
        struct
        {
            int status;                              /**< 子进程退出状态或停止/继续代码。 */
            clock_t utime;                           /**< 用户 CPU 孩子消耗的时间。 */
            clock_t stime;                           /**< 系统 CPU 子进程消耗的时间。 */
        } sigchld;
    };
} *lwp_siginfo_ext_t;

/** 一个排队的 RT-Smart 信号发生。 */
typedef struct lwp_siginfo
{
    rt_list_t node;                                  /**< lwp_sigqueue::siginfo_list 中的节点。 */

    struct
    {
        int signo;                                   /**< 信号编号。 */
        int code;                                    /**< 起源/原因代码类似于 si_code。 */

        int from_tid;                                /**< 已知时发送线程 ID。 */
        pid_t from_pid;                              /**< 已知时发送进程 ID。 */
    } ksiginfo;

    struct lwp_siginfo_ext *ext;                     /**< 可选的信号特定扩展有效负载。 */
} *lwp_siginfo_t;

/** 挂起信号队列和用于快速挂起检查的位图。 */
typedef struct lwp_sigqueue
{
    rt_list_t siginfo_list;                          /**< 排队信号发生的有序列表。 */
    lwp_sigset_t sigset_pending;                     /**< 当前待处理的信号号的联合。 */
} *lwp_sigqueue_t;

/** 一个 RT-Smart 线程私有的信号状态。 */
struct lwp_thread_signal {
    lwp_sigset_t sigset_mask;                        /**< 信号被该线程阻塞。 */
    struct lwp_sigqueue sig_queue;                   /**< 专门为此线程挂起的信号。 */
};

/** 描述挂起的用户空间上下文的架构中立指针。 */
struct rt_user_context
{
    void *sp;                                        /**< 保存的用户空间堆栈指针。 */
    void *pc;                                        /**< 保存的用户空间程序计数器。 */
    void *flag;                                      /**< 架构状态/标志值。 */

    void *ctx;                                       /**< 内核端上下文标记； NULL 表示用户模式。 */
};
#endif /* RT_USING_SMART */

/**
 * 在延迟线程回收期间调用线程清理回调。
 *
 * 它在线程停止执行后运行。  回调可以释放调用者拥有的资源，但不能假设它在退出线程的堆栈上运行；根据配置，它从空闲或系统失效线程运行。
 */
typedef void (*rt_thread_cleanup_t)(struct rt_thread *tid);

/**
 * @brief 线程控制 Block (TCB)。
 *
 * 线程既是一个托管内核对象，又是一个可调度的执行上下文。架构端口拥有`sp`以下的布局；调度器拥有RT_SCHED_THREAD_CTX扩展的字段； IPC和定时器代码通过嵌入的thread_timer协调。  大多数字段是内核私有的，必须通过公共线程 API 读取或更改。
 */
struct rt_thread
{
    struct rt_object            parent;                 /**< 基础对象；必须保留在第一个字段。 */

    /* 架构上下文、初始条目和拥有的堆栈范围。 */
    void                        *sp;                    /**< 上下文切换使用的保存的内核堆栈指针。 */
    void                        *entry;                 /**< 线程入口例程，一般为了 ABI 可移植性而存储。 */
    void                        *parameter;             /**< 传递给入口例程的不透明参数。 */
    void                        *stack_addr;            /**< 分配的堆栈区域的最低/基地址。 */
    rt_uint32_t                 stack_size;             /**< 堆栈区域大小（以字节为单位）。 */

    rt_err_t                    error;                  /**< 最后一个每线程内核错误；还传达唤醒/超时状态。 */

#ifdef RT_USING_SMP
    rt_atomic_t                 cpus_lock_nest;         /**< 旧版全 CPU 调度程序锁的嵌套计数。 */
#endif

    /* 优先级、就绪/等待列表成员资格、状态、时间片和 CPU 关联性。 */
    RT_SCHED_THREAD_CTX
    struct rt_timer             thread_timer;           /**< 睡眠和阻塞 IPC 重用的一次性超时计时器。 */
    rt_thread_cleanup_t         cleanup;                /**< 延迟回收期间执行的可选回调。 */

#ifdef RT_USING_MUTEX
    /* 优先级继承和退出清理使用的互斥锁所有权图。 */
    rt_list_t                   taken_object_list;      /**< 该线程当前拥有的互斥锁。 */
    rt_object_t                 pending_object;         /**< 该线程当前正在等待获取的互斥对象。 */
#endif /* RT_USING_MUTEX */

#ifdef RT_USING_EVENT
    /* 线程被阻塞时保留请求的事件条件。 */
    rt_uint32_t                 event_set;              /**< rt_event_recv() 请求的事件位。 */
    rt_uint8_t                  event_info;             /**< AND/OR/CLEAR 待处理接收的匹配选项。 */
#endif /* RT_USING_EVENT */

#ifdef RT_USING_SIGNALS
    rt_sigset_t                 sig_pending;            /**< 等待传递的经典信号位图。 */
    rt_sigset_t                 sig_mask;               /**< 经典信号的位图已启用/未屏蔽以供传递。 */

#ifndef RT_USING_SMP
    void                        *sig_ret;               /**< 保存的堆栈指针用于从信号处理程序返回。 */
#endif /* RT_USING_SMP */
    rt_sighandler_t             *sig_vectors;           /**< 为线程分配的每个信号处理程序向量。 */
    void                        *si_list;               /**< 私有排队信号信息列表。 */
#endif /* RT_USING_SIGNALS */

#ifdef RT_USING_PTHREADS
    void                        *pthread_data;          /**< POSIX-线程适配数据，所有 ABI 上的指针大小。 */
#endif /* RT_USING_PTHREADS */

    /* 轻量级进程（如果存在） */
#ifdef RT_USING_SMART
    void                        *msg_ret;               /**< RT-Smart IPC 路径使用的保存的返回值/消息。 */

    void                        *lwp;                   /**< 拥有轻量级进程对象。 */
    /* 用户空间入口和双栈信息。 */
    void                        *user_entry;            /**< 初始用户空间程序计数器。 */
    void                        *user_stack;            /**< 用户空间堆栈映射的基址/地址。 */
    rt_uint32_t                 user_stack_size;        /**< 用户空间堆栈范围（以字节为单位）。 */
    rt_uint32_t                 *kernel_sp;             /**< 在用户转换期间保存的内核堆栈指针。 */
    rt_list_t                   sibling;                /**< 所属进程的线程列表中的节点。 */

    struct lwp_thread_signal    signal;                 /**< 掩码和排队信号对此用户线程私有。 */
    struct rt_user_context      user_ctx;               /**< 保存的架构中立的用户空间上下文。 */
    struct rt_wakeup            wakeup_handle;          /**< 用于从 RT-Smart 等待中删除此线程的适配器。 */
    rt_atomic_t                 exit_request;           /**< 异步请求该线程终止。 */
    int                         tid;                    /**< 进程可见的线程标识符。 */
    int                         tid_ref_count;          /**< 保持 TID 映射活动的参考文献。 */
    void                        *susp_recycler;         /**< 回收器正在等待这个挂起的线程完成。 */
    void                        *robust_list;           /**< 用户空间鲁棒/PI 锁定列表；仔细验证每个访问。 */

#ifndef ARCH_MM_MMU
    lwp_sighandler_t            signal_handler[32];    /**< no-MMU RT-Smart 目标的每信号处理程序。 */
#else
    int                         step_exec;              /**< 调试器单步执行请求/状态。 */
    int                         debug_attach_req;       /**< 待处理的调试器附加请求。 */
    int                         debug_ret_user;         /**< 调试器应将控制权返回给用户空间。 */
    int                         debug_suspend;          /**< 线程被调试器挂起。 */
    struct rt_hw_exp_stack      *regs;                  /**< ptrace/调试的架构异常框架。 */
    void                        *thread_idr;             /**< 保存的架构线程 ID/TLS 寄存器值。 */
    int                         *clear_child_tid;       /**< 用户空间地址在退出时被清除并被 futex 唤醒。 */
#endif /* ARCH_MM_MMU */
#endif /* RT_USING_SMART */

#ifdef RT_USING_CPU_USAGE_TRACER
    rt_ubase_t                  user_time;              /**< 用户空间中累积的执行单元。 */
    rt_ubase_t                  system_time;            /**< 内核空间中累积的执行单元。 */
    rt_ubase_t                  total_time_prev;        /**< 之前用于增量的总时间快照。 */
    rt_uint8_t                  cpu_usage;              /**< 最近计算的 CPU 利用率。 */
#endif /* RT_USING_CPU_USAGE_TRACER */

#ifdef RT_USING_MEM_PROTECTION
    void *mem_regions;                                 /**< 架构定义的内存保护区域集。 */
#ifdef RT_USING_HW_STACK_GUARD
    void *stack_buf;                                   /**< 保留用于保护设置的堆栈分配元数据。 */
#endif /* RT_USING_HW_STACK_GUARD */
#endif /* RT_USING_MEM_PROTECTION */

    struct rt_spinlock          spinlock;               /**< 保护退出时互斥体清理和选定的 RT-Smart 回收器快照。 */
    rt_ubase_t                  user_data;              /**< 应用程序拥有的标量/指针大小的扩展槽。 */
};
typedef struct rt_thread *rt_thread_t;

#ifdef RT_USING_SMART
/** 当 RT-Smart 线程的保存状态表示用户空间执行时为真。 */
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
 * IPC 等待排序和通用控制命令。
 *
 * FIFO 保留到货订单。  PRIO 按有效调度优先级对等待者进行排序，以便可以首先唤醒数字为 smaller（较高优先级）的线程。  RT_WAITING_NO 使获取/接收操作成为非阻塞，而 RT_WAITING_FOREVER 则抑制超时定时器的安装。
 */
#define RT_IPC_FLAG_FIFO                0x00            /**< 先进先出 IPC。 @ref group_thread_comm。 */
#define RT_IPC_FLAG_PRIO                0x01            /**< PRIOed IPC。 @ref group_thread_comm。 */

#define RT_IPC_CMD_UNKNOWN              0x00            /**<未知IPC命令*/
#define RT_IPC_CMD_RESET                0x01            /**<重置IPC对象*/
#define RT_IPC_CMD_GET_STATE            0x02            /**< 获取IPC对象的状态 */
#define RT_IPC_CMD_SET_VLIMIT           0x03            /**<设置IPC值的最大限值*/

#define RT_WAITING_FOREVER              -1              /**< 永远阻塞，直到获得资源。 */
#define RT_WAITING_NO                   0               /**< 非块。 */

/**
 * @brief 信号量、互斥量、事件、邮箱和消息队列的公共基础。
 *
 * 父对象的标志存储IPC等待顺序策略。  suspend_thread 是接收方/获取方等待队列。  邮箱和消息队列另外还带有一个用于满缓冲区情况的发送者等待队列。
 */
struct rt_ipc_object
{
    struct rt_object parent;                            /**<继承自rt_object */

    rt_list_t suspend_thread;                           /**< 线程被阻塞等待获取/接收此资源。 */
};

/**
 * @addtogroup group_semaphore 信号量
 * @{
 */

#ifdef RT_USING_SEMAPHORE
/**
 * @brief 计数信号量控制块。
 *
 * 值是立即可用的令牌计数，并且永远不会超过 max_value。自旋锁使计数器更新和等待传输相对于中断和其他 CPU 而言是原子的。
 */
struct rt_semaphore
{
    struct rt_ipc_object parent;                        /**<继承自ipc_object */

    rt_uint16_t          value;                         /**< 当前可用且无阻塞的令牌。 */
    rt_uint16_t          max_value;                     /**< 值的饱和/验证限制。 */
    struct rt_spinlock   spinlock;                      /**< 保护值和继承的等待队列。 */
};
typedef struct rt_semaphore *rt_sem_t;
#endif /* RT_USING_SEMAPHORE */

/**@}*/

/**
 * @addtogroup group_mutex 互斥体
 * @{
 */

#ifdef RT_USING_MUTEX
/**
 * @brief 具有优先级反转缓解功能的递归互斥体。
 *
 * 所有者可以重复获取互斥锁； Hold 计算嵌套深度。  互斥锁还通过 taken_list 链接到所有者->taken_object_list。  优先级字段保留配置的上限和等待者之间的最佳优先级，以便实现可以传播并稍后恢复有效的优先级。  在正常操作期间，互斥体只能由所有者释放。内核清理是故意的例外：它可能会解除其记录所有者已进入 RT_THREAD_CLOSE 状态的互斥体。
 */
struct rt_mutex
{
    struct rt_ipc_object parent;                        /**<继承自ipc_object */

    rt_uint8_t           ceiling_priority;              /**< 配置的优先级上限；数字越低意味着越高。 */
    rt_uint8_t           priority;                      /**< 由待处理的等待者代表的最高有效优先级。 */
    rt_uint8_t           hold;                          /**< 所有者持有的递归获取深度。 */
    rt_uint8_t           reserved;                      /**< 填充/保留字节；呼叫者不得使用它。 */

    struct rt_thread    *owner;                         /**< 当前拥有互斥锁的线程，或 NULL。 */
    rt_list_t            taken_list;                    /**< 所有者中的节点->taken_object_list。 */
    struct rt_spinlock   spinlock;                      /**< 保护所有权、保留计数、优先级和服务员。 */
};
typedef struct rt_mutex *rt_mutex_t;
#endif /* RT_USING_MUTEX */

/**@}*/

/**
 * @addtogroup group_event 事件
 * @{
 */

#ifdef RT_USING_EVENT
/**
 * 事件接收选项标志。  AND/OR 恰好之一描述匹配；当接收成功时，CLEAR 以原子方式消耗匹配的位。
 */
#define RT_EVENT_FLAG_AND               0x01            /**<逻辑与*/
#define RT_EVENT_FLAG_OR                0x02            /**<逻辑或*/
#define RT_EVENT_FLAG_CLEAR             0x04            /**<清除标志*/

/**
 * @brief 事件位同步对象。
 *
 * 每个被阻止的接收器将其请求的掩码和选项存储在其 TCB 中。发送位或将它们放入集合中，并扫描等待者以查找匹配的 AND/OR 条件。  事件位代表状态，而不是排队的事件；重复发送已设置的位不会累积计数。
 */
struct rt_event
{
    struct rt_ipc_object parent;                        /**<继承自ipc_object */

    rt_uint32_t          set;                           /**< 当前 32 位事件状态。 */
    struct rt_spinlock   spinlock;                      /**< 保护设置和接收器唤醒选择。 */
};
typedef struct rt_event *rt_event_t;
#endif /* RT_USING_EVENT */

/**@}*/

/**
 * @addtogroup group_mailbox 邮箱
 * @{
 */

#ifdef RT_USING_MAILBOX
/**
 * @brief 指针宽度消息的环形缓冲区。
 *
 * 邮箱每条消息复制一个 rt_ubase_t 值；它不会复制该值引用的数据。  条目是当前占用情况，而 in_offset 和 out_offset 则以模大小换行。  接收方在继承的队列上等待，发送方在 suspend_sender_thread 上被满环等待阻塞。
 */
struct rt_mailbox
{
    struct rt_ipc_object parent;                        /**<继承自ipc_object */

    rt_ubase_t          *msg_pool;                      /**< 槽数组：init 时由调用者拥有，create 时由堆拥有。 */

    rt_uint16_t          size;                          /**< msg_pool 中的槽总数。 */

    rt_uint16_t          entry;                         /**< 当前排队消息的数量。 */
    rt_uint16_t          in_offset;                     /**< 下一个正常发送写入的环索引。 */
    rt_uint16_t          out_offset;                    /**< 下一个接收读取的环索引。 */

    rt_list_t            suspend_sender_thread;         /**< 由于环已满，线程被阻塞。 */
    struct rt_spinlock   spinlock;                      /**< 保护环索引、占用率和两个等待队列。 */
};
typedef struct rt_mailbox *rt_mailbox_t;
#endif /* RT_USING_MAILBOX */

/**@}*/

/**
 * @addtogroup group_messagequeue 消息队列
 * @{
 */

#ifdef RT_USING_MESSAGEQUEUE
/**
 * @brief 固定容量、按值复制消息的队列。
 *
 * msg_pool 分为 max_msgs 内部节点，每个节点都足够大，可容纳实现头加上对齐的 msg_size 有效负载。  三个私有指针形成排队消息链和空闲节点池。  对于普通发送，排队链是 FIFO，但优先发送/优先接收支持可以按消息优先级对节点进行排序。  与邮箱不同，发送将最多 msg_size 字节复制到队列拥有的节点中。
 */
struct rt_messagequeue
{
    struct rt_ipc_object parent;                        /**<继承自ipc_object */

    void                *msg_pool;                      /**< 节点存储：init 时由调用者拥有，create 时由堆拥有。 */

    rt_uint16_t          msg_size;                      /**< 每个节点中存储的最大有效负载字节。 */
    rt_uint16_t          max_msgs;                      /**< 总节点数和最大队列深度。 */

    rt_uint16_t          entry;                         /**< 当前排队的消息数。 */

    void                *msg_queue_head;                /**< 第一个排队的内部消息节点。 */
    void                *msg_queue_tail;                /**< 最后排队的内部消息节点。 */
    void                *msg_queue_free;                /**< 内部自由节点链的头。 */

    rt_list_t            suspend_sender_thread;         /**< 由于没有可用的空闲节点，发件人被阻止。 */
    struct rt_spinlock   spinlock;                      /**< 保护节点链、条目和等待队列。 */
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
 * @brief 系统堆后端的通用统计对象。
 *
 * 小内存和平板分配器通过将此描述符嵌入到其私有实现对象中来公开相同的公共 rt_mem_t 句柄。  值报告分配器管理的有效负载/记帐字节，并且在应用对齐和元数据开销后不需要等于原始 BSP 区域边界。
 */
struct rt_memory
{
    struct rt_object        parent;                 /**< 基础对象；必须保留在第一个字段。 */
    const char *            algorithm;              /**< 人类可读的分配器/后端名称。 */
    rt_ubase_t              address;                /**< 管理区域的对齐起始地址。 */
    rt_size_t               total;                  /**< 此分配器管理的总字节数。 */
    rt_size_t               used;                   /**< 当前分配的字节数。 */
    rt_size_t               max;                    /**< 自初始化以来使用的高水位线。 */
};
typedef struct rt_memory *rt_mem_t;
#endif /* RT_USING_HEAP */

/*
 * 内存管理堆和分区
 */

#ifdef RT_USING_SMALL_MEM
typedef rt_mem_t rt_smem_t;
#endif /* RT_USING_SMALL_MEM */

#ifdef RT_USING_SLAB
typedef rt_mem_t rt_slab_t;
#endif /* RT_USING_SLAB */

#ifdef RT_USING_MEMHEAP
/**
 * @brief 在内存堆分配之前存储的边界标记和列表链接。
 *
 * 每个物理块都参与地址排序的下一个/上一个链；只有空闲区块参与next_free/prev_free。  当多个内存堆为系统分配器提供数据时，pool_ptr 标识所属堆。  magic 对分配状态进行编码，并进行检查以检测无效或重复的释放。
 */
struct rt_memheap_item
{
    rt_uint32_t             magic;                      /**< 完整性/分配状态标记。 */
    struct rt_memheap      *pool_ptr;                   /**< 拥有该块的堆。 */

    struct rt_memheap_item *next;                       /**< 按地址的下一个物理块。 */
    struct rt_memheap_item *prev;                       /**< 按地址的前一个物理块。 */

    struct rt_memheap_item *next_free;                  /**< 分配器搜索顺序中的下一个空闲块。 */
    struct rt_memheap_item *prev_free;                  /**< 分配器搜索顺序中的前一个空闲块。 */
#ifdef RT_USING_MEMTRACE
    rt_uint8_t              owner_thread_name[4];       /**< 用于诊断的截断分配线程名称。 */
#endif /* RT_USING_MEMTRACE */
};

/**
 * @brief 在一个调用者提供的内存区域上的可变大小分配器。
 *
 * block_list 指向第一个物理边界标签块； free_header 是嵌入式空闲列表哨兵，free_list 通常指向该哨兵作为搜索锚点。  嵌入式信号量通常会序列化分配。  当locked为true时，外部系统堆锁已经提供了序列化，避免了递归锁定和早期启动对信号量的依赖。
 */
struct rt_memheap
{
    struct rt_object        parent;                     /**<继承自rt_object */

    void                   *start_addr;                 /**< 调用者提供的开始；分配器假定需要对齐。 */

    rt_size_t               pool_size;                  /**< 提供的尺寸向下舍入；包括分配器边界标头。 */
    rt_size_t               available_size;            /**< 分配器跟踪的当前空闲字节。 */
    rt_size_t               max_used_size;              /**< 已分配字节的高水位线。 */

    struct rt_memheap_item *block_list;                 /**< 物理区块链的哨兵/条目。 */

    struct rt_memheap_item *free_list;                  /**< 空闲列表哨兵/搜索 anchor（通常为 &free_header）。 */
    struct rt_memheap_item  free_header;                /**< 自由区块链的嵌入式哨兵。 */

    struct rt_semaphore     lock;                       /**< 内部分配器类似互斥信号量。 */
    rt_bool_t               locked;                     /**< 当外部提供同步时为真。 */
};
#endif /* RT_USING_MEMHEAP */

#ifdef RT_USING_MEMPOOL
/**
 * @brief 固定大小的块池，具有可选的块分配。
 *
 * 空闲块重用其第一个指针大小的字节来链接 block_list。  当 block_free_count 为零时，可以暂停拍摄； rt_mp_free() 返回一个块并唤醒一名服务员。  池既不构造也不销毁存储在块中的对象，并且调用者必须将每个块返回到其原始池一次。
 */
struct rt_mempool
{
    struct rt_object    parent;                            /**<继承自rt_object */

    void                *start_address;                    /**< 后备存储：init 时由调用者拥有，create 时由堆拥有。 */
    rt_size_t           size;                             /**< 为池提供的总字节数。 */

    rt_size_t           block_size;                       /**< 每个可分配块中的对齐字节。 */
    rt_uint8_t          *block_list;                       /**< 侵入式自由区块链的头部。 */

    rt_size_t           block_total_count;                /**< 从该区域雕刻的方块数量。 */
    rt_size_t           block_free_count;                 /**< 当前可用的块数。 */

    rt_list_t           suspend_thread;                   /**< 线程被阻塞等待空闲块。 */
    struct rt_spinlock  spinlock;                         /**< 保护空闲链、柜台和服务员。 */
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
 * @brief 用于发现和特定于类别的控制范围的粗略类别。
 *
 * 该值标识设备的公共角色，而不是用于访问它的具体驱动程序或总线。  类驱动程序可以将 rt_device 嵌入到更大的对象中，并在基础对象之后保留协议特定的状态。
 */
enum rt_device_class_type
{
    RT_Device_Class_Char = 0,                           /**< 字符设备 */
    RT_Device_Class_Block,                              /**< 块设备 */
    RT_Device_Class_NetIf,                              /**<网络接口*/
    RT_Device_Class_MTD,                                /**<存储设备*/
    RT_Device_Class_CAN,                                /**< CAN 设备 */
    RT_Device_Class_RTC,                                /**< RTC 设备 */
    RT_Device_Class_Sound,                              /**< 声音设备 */
    RT_Device_Class_Graphic,                            /**< 图形设备 */
    RT_Device_Class_I2CBUS,                             /**< I2C 总线设备 */
    RT_Device_Class_USBDevice,                          /**< USB从设备 */
    RT_Device_Class_USBHost,                            /**< USB 主机总线 */
    RT_Device_Class_USBOTG,                             /**< USB OTG总线 */
    RT_Device_Class_SPIBUS,                             /**< SPI总线设备 */
    RT_Device_Class_SPIDevice,                          /**< SPI 设备 */
    RT_Device_Class_SDIO,                               /**< SDIO总线设备 */
    RT_Device_Class_PM,                                 /**< PM伪设备 */
    RT_Device_Class_Pipe,                               /**< 管道设备 */
    RT_Device_Class_Portal,                             /**< 门户设备 */
    RT_Device_Class_Timer,                              /**< 定时器设备 */
    RT_Device_Class_Miscellaneous,                      /**<其他设备*/
    RT_Device_Class_Sensor,                             /**< 传感器设备 */
    RT_Device_Class_Touch,                              /**< 触摸设备 */
    RT_Device_Class_PHY,                                /**< PHY 设备 */
    RT_Device_Class_Security,                           /**<安全装置*/
    RT_Device_Class_WLAN,                               /**< WLAN 设备 */
    RT_Device_Class_Pin,                                /**< 引脚设备 */
    RT_Device_Class_ADC,                                /**< ADC 设备 */
    RT_Device_Class_DAC,                                /**< DAC 设备 */
    RT_Device_Class_WDT,                                /**< WDT 设备 */
    RT_Device_Class_PWM,                                /**< PWM 设备 */
    RT_Device_Class_Bus,                                /**< 总线设备 */
    RT_Device_Class_Unknown                             /**<未知设备*/
};

/**
 * 设备功能和运行时状态标志存储在 rt_device::flag 中。
 *
 * 低访问位描述支持的方向，中间位描述生命周期/功能，高位选择中断或 DMA 传输模式。这些是注册时间/设备状态标志，与 rt_device::open_flag 中记录的每次打开请求不同。
 */
#define RT_DEVICE_FLAG_DEACTIVATE       0x000           /**< 设备未初始化 */

#define RT_DEVICE_FLAG_RDONLY           0x001           /**<只读*/
#define RT_DEVICE_FLAG_WRONLY           0x002           /**<只写*/
#define RT_DEVICE_FLAG_RDWR             0x003           /**<读写*/

#define RT_DEVICE_FLAG_REMOVABLE        0x004           /**<可移动设备*/
#define RT_DEVICE_FLAG_STANDALONE       0x008           /**<独立设备*/
#define RT_DEVICE_FLAG_ACTIVATED        0x010           /**<设备已激活*/
#define RT_DEVICE_FLAG_SUSPENDED        0x020           /**<设备已暂停*/
#define RT_DEVICE_FLAG_STREAM           0x040           /**<流模式*/
#define RT_DEVICE_FLAG_DYNAMIC          0x080           /**< open() 时确定设备 */

#define RT_DEVICE_FLAG_INT_RX           0x100           /**< Rx 上的 INT 模式 */
#define RT_DEVICE_FLAG_DMA_RX           0x200           /**< Rx 上的 DMA 模式 */
#define RT_DEVICE_FLAG_INT_TX           0x400           /**< Tx 上的 INT 模式 */
#define RT_DEVICE_FLAG_DMA_TX           0x800           /**< Tx 上的 DMA 模式 */

#define RT_DEVICE_OFLAG_CLOSE           0x000           /**<设备已关闭*/
#define RT_DEVICE_OFLAG_RDONLY          0x001           /**< 只读访问 */
#define RT_DEVICE_OFLAG_WRONLY          0x002           /**< 只写访问 */
#define RT_DEVICE_OFLAG_RDWR            0x003           /**<读写*/
#define RT_DEVICE_OFLAG_OPEN            0x008           /**<设备已打开*/
#define RT_DEVICE_OFLAG_MASK            0xf0f           /**< 打开标志的掩码 */

/**
 * 通用设备命令 0x01 - 0x1F 通用设备控制命令 0x20 - 0x3F udevice 控制命令 0x40 - 特殊设备控制命令
 */
#define RT_DEVICE_CTRL_RESUME           0x01            /**<恢复设备*/
#define RT_DEVICE_CTRL_SUSPEND          0x02            /**< 挂起设备 */
#define RT_DEVICE_CTRL_CONFIG           0x03            /**<配置设备*/
#define RT_DEVICE_CTRL_CLOSE            0x04            /**<关闭设备*/
#define RT_DEVICE_CTRL_NOTIFY_SET       0x05            /**< 设置通知函数 */
#define RT_DEVICE_CTRL_SET_INT          0x06            /**<设置中断*/
#define RT_DEVICE_CTRL_CLR_INT          0x07            /**<清除中断*/
#define RT_DEVICE_CTRL_GET_INT          0x08            /**< 获取中断状态 */
#define RT_DEVICE_CTRL_CONSOLE_OFLAG    0x09            /**< 获取控制台打开标志 */
#define RT_DEVICE_CTRL_MASK             0x1f            /**< 控制命令的掩码 */

/**
 * 构建特定于类的命令命名空间的基础。  类驱动程序可以向该值添加较小的命令偏移量，而不会与通用命令或其他设备类的命令发生冲突。
 */
#define RT_DEVICE_CTRL_BASE(Type)        ((RT_Device_Class_##Type + 1) * 0x100)

typedef struct rt_driver *rt_driver_t;
typedef struct rt_device *rt_device_t;

#ifdef RT_USING_DEVICE_OPS
/**
 * @brief 由具体或类设备驱动程序实现的统一操作。
 *
 * Components/drivers/core/device.c 中的核心包装器在通过此表进行分派之前处理对象查找、延迟初始化、状态检查和引用计数。  根据包装器的合同报告 NULL 可选操作。  驱动程序读/写回调通常返回一个非负的传输单元计数；单个设备类可能会记录附加的负错误约定。  核心包装器本身使用零加 errno 来表示关闭的设备或丢失的操作。  pos 和 size 表示的单位是设备类 specific（对于流来说是字节，对于块设备通常是块）。
 */
struct rt_device_ops
{
    /* 驱动程序实现的通用设备接口。 */
    rt_err_t  (*init)   (rt_device_t dev); /**< 将硬件/软件状态置于初始化状态。 */
    rt_err_t  (*open)   (rt_device_t dev, rt_uint16_t oflag); /**< 应用一个打开请求的模式标志。 */
    rt_err_t  (*close)  (rt_device_t dev); /**< 当最后一个用户关闭时释放/禁用资源。 */
    rt_ssize_t (*read)  (rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size); /**< 将数据从设备传输到缓冲区。 */
    rt_ssize_t (*write) (rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size); /**< 将数据从缓冲区传输到设备。 */
    rt_err_t  (*control)(rt_device_t dev, int cmd, void *args); /**< 执行通用或特定于类的控制命令。 */
};
#endif /* RT_USING_DEVICE_OPS */

/**
 * @brief 与设备或通道关联的轮询/选择兼容等待队列。
 *
 * waiting_list 包含框架定义的等待节点。  flag 是等待队列的内部 CLEAN/WAKEUP 状态，而不是设备就绪事件位掩码；唤醒密钥单独传递给节点回调。  自旋锁使状态更新和等待通知与中断端生产者成为原子的。
 */
struct rt_wqueue
{
    rt_uint32_t flag;                  /**< 内部 RT_WQ_FLAG_CLEAN/WAKEUP 状态。 */
    rt_list_t waiting_list;            /**< 等待准备就绪的任务或轮询请求。 */
    struct rt_spinlock spinlock;       /**< 保护标志和 waiting_list。 */
};
typedef struct rt_wqueue rt_wqueue_t;

#ifdef RT_USING_DM
struct rt_driver;
struct rt_bus;
#endif /* RT_USING_DM */

/**
 * @brief 每个 RT-Thread 设备实例共享的基础对象。
 *
 * 类/具体驱动程序将此结构嵌入到偏移量零处，按名称注册它，并提供操作加上 user_data。  设备核心拥有类型、生命周期标志、开放引用记账和调度。  对于RT_USING_DM，同一对象也参与总线/司机匹配；设备模型扩展而不是取代 rt_device API。
 */
struct rt_device
{
    struct rt_object          parent;                   /**<继承自rt_object */

#ifdef RT_USING_DM
    struct rt_bus *bus;                                 /**< 注册该设备的总线。 */
    rt_list_t node;                                     /**< 总线设备集合中的节点。 */
    struct rt_driver *drv;                              /**< 驱动程序已成功绑定到该设备。 */
#ifdef RT_USING_OFW
    void *ofw_node;                                     /**< 打开描述此实例的固件/设备树节点。 */
#endif /* RT_USING_OFW */
    void *power_domain_unit;                            /**< 设备模型电源域附件（如果有）。 */
#ifdef RT_USING_DVFS
    void *dvfs_scaling;                                 /**< 每设备动态电压/频率缩放状态。 */
#endif
#ifdef RT_USING_DMA
    const void *dma_ops;                                /**< 为此设备选择的 DMA 映射/操作集。 */
#endif
#endif /* RT_USING_DM */

    enum rt_device_class_type type;                     /**< 公共设备类。 */
    rt_uint16_t               flag;                     /**< 功能和当前激活/挂起状态。 */
    rt_uint16_t               open_flag;                /**< 当前打开的有效模式和传输标志。 */

    rt_uint8_t                ref_count;                /**< 开放引用，包括核心接受的 -RT_ENOSYS 开放结果。 */
#ifdef RT_USING_DM
    rt_uint8_t                master_id;                /**< 设备型号主/所有者标识符，范围 0..255。 */
#endif
    rt_uint8_t                device_id;                /**< 驱动程序或框架分配的实例 ID，范围 0..255。 */

    /*
 * 上层安装的可选异步通知。较低的驱动程序可以从其 ISR/DMA 完成路径调用它们，因此回调必须遵循该驱动程序的上下文规则，并且注册者必须保持函数和引用状态都处于活动状态，直到在替换/取消注册之前停止进行中的回调。
 */
    rt_err_t (*rx_indicate)(rt_device_t dev, rt_size_t size); /**< 通知可以读取大小单位；可以在 ISR 上下文中运行。 */
    rt_err_t (*tx_complete)(rt_device_t dev, void *buffer);   /**< 通知异步传输缓冲区完成。 */

#ifdef RT_USING_DEVICE_OPS
    const struct rt_device_ops *ops;                    /**< 驱动程序提供的不可变操作表。 */
#else
    /* 旧版 ABI 将相同的通用操作直接存储在每个对象中。 */
    rt_err_t  (*init)   (rt_device_t dev); /**< 初始化设备。 */
    rt_err_t  (*open)   (rt_device_t dev, rt_uint16_t oflag); /**< 应用请求的打开模式。 */
    rt_err_t  (*close)  (rt_device_t dev); /**< 关闭/释放设备。 */
    rt_ssize_t (*read)  (rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size); /**< 读取设备特定的单位。 */
    rt_ssize_t (*write) (rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size); /**< 写入特定于设备的单位。 */
    rt_err_t  (*control)(rt_device_t dev, int cmd, void *args); /**< 执行控制命令。 */
#endif /* RT_USING_DEVICE_OPS */

#ifdef RT_USING_POSIX_DEVIO
    const struct dfs_file_ops *fops;                    /**< POSIX/DFS 文件操作由该设备公开。 */
    struct rt_wqueue wait_queue;                        /**< 轮询/选择该设备的服务员。 */
#endif /* RT_USING_POSIX_DEVIO */

    rt_err_t (*readlink)
        (rt_device_t dev, char *buf, int len);          /**< 返回该设备公开的 devfs 符号链接目标。 */

    void                     *user_data;                /**< 不透明类/具体驱动程序私有状态。 */
};

/**
 * @brief 用于注册特定于设备的通知回调的对。
 */
struct rt_device_notify
{
    void (*notify)(rt_device_t dev);                    /**< 驱动程序触发的回调；执行上下文是特定于驱动程序的。 */
    struct rt_device *dev;                              /**< 与通知关联的设备。 */
};

#ifdef RT_USING_SMART
/**
 * @brief RT-Smart 同步消息/回复通道。
 *
 * 通道是一种 IPC 对象，它协调发送者消息、阻塞的发送者线程、一个回复目标和可轮询的读取器准备情况。  slock 保护所有队列和状态转换； ref 控制生命周期，而用户保留通道。
 */
struct rt_channel
{
    struct rt_ipc_object parent;                        /**< 基本 IPC 对象和通用等待队列。 */
    struct rt_thread *reply;                            /**< 发送线程当前正在等待接收回复。 */
    struct rt_spinlock slock;                           /**< 保护通道状态和所有私有队列。 */
    rt_list_t wait_msg;                                 /**< 待处理的发送者消息描述符。 */
    rt_list_t wait_thread;                              /**< 发送者线程被阻止等待接收/回复。 */
    rt_wqueue_t reader_queue;                           /**< 轮询/选择可读通道状态的队列。 */
    rt_uint8_t  stat;                                   /**< 实现定义的通道生命周期/状态位。 */
    rt_ubase_t  ref;                                    /**< 通道引用计数。 */
};
typedef struct rt_channel *rt_channel_t;
#endif /* RT_USING_SMART */

/**@}*/
#endif /* RT_USING_DEVICE */

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* C++ 的 RT-Thread 定义 */
namespace rtthread {

enum TICK_WAIT {
    WAIT_NONE = 0,       /**< 执行非阻塞操作。 */
    WAIT_FOREVER = -1,   /**< 阻止而不安装有限超时。 */
};

}

#endif /* __cplusplus */

#endif /* __RT_DEF_H__ */
