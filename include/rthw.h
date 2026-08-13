/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2006-03-18     Bernard      第一个版本
 * 2006-04-25     Bernard      添加rt_hw_context_switch_interrupt声明
 * 2006-09-24     Bernard      添加rt_hw_context_switch_to声明
 * 2012-12-29     Bernard      添加rt_hw_exception_install声明
 * 2017-10-17     Hichard      添加一些宏
 * 2018-11-17     Jesven       添加rt_hw_spinlock_t
 *                             add smp support
 * 2019-05-18     Bernard      添加空定义以表示不启用缓存情况
 * 2023-09-15     xqyjlj       性能 rt_hw_interrupt_disable/启用
 * 2023-10-16     Shell        支持新的回溯框架
 */

#ifndef __RT_HW_H__
#define __RT_HW_H__

/**
 * @file rthw.h
 * @brief 架构/BSP 层与 RT-Thread 内核之间的约定。
 *
 * 本头文件集中声明依赖 CPU、中断控制器、缓存层次、异常模型或开发板控制台的操作。
 * 内核代码无需了解具体硬件即可调用这些接口；选定的 CPU 移植层或 BSP 必须提供相应实现。
 *
 * 这些 API 可在早期启动、线程、中断和异常等上下文中使用。因此，调用者必须遵守各组
 * 接口说明的上下文和同步规则。除非另有明确说明，地址有效性、缓存行对齐、权限和 CPU
 * 间同步均由调用者负责。
 */

#include <rtdef.h>

#if defined (RT_USING_CACHE) || defined(RT_USING_SMP) || defined(RT_HW_INCLUDE_CPUPORT)
/*
 * cpuport.h 提供架构专用类型和基础操作，例如 rt_hw_spinlock_t、缓存屏障及 CPU
 * 专用辅助函数。仅当启用的功能需要这些架构约定时才包含该文件。
 */
#include <cpuport.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name 内存映射寄存器访问器
 *
 * 每个宏都会把整数地址转换为指定宽度的指针，赋予 volatile 语义后解引用。结果是左值，
 * 因而既可读也可写，例如
 * `HWREG32(base + offset) = value`.
 *
 * volatile 可阻止编译器删除或合并单次访问，但它不是 CPU 内存屏障，也不提供 CPU 间
 * 同步。调用者必须保证地址有效且满足对齐要求，并确保设备支持所选访问宽度。BSP 可在
 * 包含本头文件前重定义这些宏。
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
 * 供移植层或调用者用于对齐和缓存维护计算的默认缓存行大小提示。当前公共代码不会自动
 * 使用它；实际一致性粒度不是 32 字节时，使用此提示的一方必须重定义该值。
 */
#ifndef RT_CPU_CACHE_LINE_SZ
#define RT_CPU_CACHE_LINE_SZ    32
#endif

/**
 * 传给指令/数据缓存维护 API 的操作选择值。这些值占用相互独立的位，但是否支持同时
 * 刷新和失效取决于移植层；调用者不能假定所有移植层都接受该组合。
 */
enum RT_HW_CACHE_OPS
{
    /** 回写脏缓存行，使后续观察者能看到当前内存内容。 */
    RT_HW_CACHE_FLUSH      = 0x01,
    /** 丢弃缓存行，使后续读取从内存获取当前内容。 */
    RT_HW_CACHE_INVALIDATE = 0x02,
};

/**
 * @name CPU 缓存接口
 *
 * 启用、停用和状态查询是架构服务。范围操作把 @p ops 应用于从 @p addr 开始、长度为
 * @p size 字节的区间；移植层通常会把范围扩展到完整缓存行。与 DMA 协作时，调用者必须
 * 按传输方向选择操作，并安排所需的内存屏障。
 *
 * 禁用 RT_USING_CACHE 时，维护调用会变为无操作宏，状态读取返回零；无操作宏的参数
 * 不会被求值。
 * @{
 */
#ifdef RT_USING_CACHE

#ifdef RT_USING_SMART
#include <cache.h>
#endif

/** 按 CPU 移植层要求的顺序启用指令缓存。 */
void rt_hw_cpu_icache_enable(void);
/** 停用指令缓存，并执行移植层要求的维护操作。 */
void rt_hw_cpu_icache_disable(void);
/**
 * 查询 CPU 移植层实现的指令缓存启用状态。现有部分移植层仅提供始终返回零的占位实现，
 * 因此不能据此跨架构证明缓存已停用。
 */
rt_base_t rt_hw_cpu_icache_status(void);
/** 对范围 [@p addr, @p addr + @p size) 内已缓存的指令执行 @p ops。 */
void rt_hw_cpu_icache_ops(int ops, void* addr, int size);

/** 按 CPU 移植层要求的顺序启用数据缓存。 */
void rt_hw_cpu_dcache_enable(void);
/** 停用数据缓存，并执行移植层要求的回写/维护操作。 */
void rt_hw_cpu_dcache_disable(void);
/**
 * 查询 CPU 移植层实现的数据缓存启用状态。部分移植层即使实现了维护操作仍无条件返回零；
 * 只能按该移植层文档约定解释此结果。
 */
rt_base_t rt_hw_cpu_dcache_status(void);
/** 对范围 [@p addr, @p addr + @p size) 内已缓存的数据执行 @p ops。 */
void rt_hw_cpu_dcache_ops(int ops, void* addr, int size);
#else

/* 无缓存构建：以零运行时开销和零参数求值成本保留 API。 */
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
 * @brief 复位处理器或整个硬件平台。
 *
 * 这通常是 BSP/SoC 服务，常常不会返回。它可能由恢复代码调用，因此实现不应依赖普通
 * 线程调度仍能正常工作。
 */
void rt_hw_cpu_reset(void);

/**
 * @brief 使处理器或硬件平台进入关闭状态。
 *
 * 具体行为由开发板决定：可能关机、移交给固件，或永久停留在低功耗循环中。硬件支持关闭时，
 * 实现通常不会返回。
 */
void rt_hw_cpu_shutdown(void);

/**
 * @brief 返回当前 CPU 架构的文本说明。
 * @return 指向 CPU 移植层拥有的持久只读字符串的指针。
 */
const char *rt_hw_cpu_arch(void);

/**
 * @brief 在新线程的栈上构造初始保存上下文。
 *
 * 调度器初始化线程时调用本函数。移植层会布置一个模拟的异常/上下文帧，使第一次恢复时从
 * @p entry 开始执行并传入 @p parameter。若入口函数返回，其保存的返回地址必须转到
 * @p exit。
 *
 * @param entry 线程入口地址；为兼容汇编 ABI 而保持无类型。
 * @param parameter 传给线程入口函数的参数。
 * @param stack_addr 通用线程代码选定的初始栈地址；栈顶/栈底的精确约定由 CPU 移植层决定。
 * @param exit 入口函数返回时调用的清理跳板。
 * @return 存入线程控制块并由对应上下文切换实现使用的保存栈指针。
 *
 * @note 栈对齐、寄存器顺序、状态寄存器内容和权限状态共同构成与移植层汇编代码共享的 ABI。
 */
rt_uint8_t *rt_hw_stack_init(void       *entry,
                             void       *parameter,
                             rt_uint8_t *stack_addr,
                             void       *exit);

#ifdef RT_USING_HW_STACK_GUARD
/**
 * @brief 为 @p thread 配置架构提供的硬件栈保护。
 *
 * 实现可配置 MPU 区域、限制寄存器或其他硬件溢出检测机制。传入的线程必须已有有效栈范围。
 */
void rt_hw_stack_guard_init(rt_thread_t thread);
#endif

/**
 * 通用中断 API 使用的中断服务例程函数签名。
 *
 * @param vector 控制器送达的中断/向量编号。
 * @param param 使用 rt_hw_interrupt_install() 注册的不透明参数。
 *
 * 回调在中断上下文运行，必须使用中断安全 API、避免阻塞，并限制执行时间。架构的中断入口/退出
 * 代码负责实现 RT-Thread 的中断嵌套协议。
 */
typedef void (*rt_isr_handler_t)(int vector, void *param);

/** 由中断控制器实现维护的描述符。 */
struct rt_irq_desc
{
    rt_isr_handler_t handler; /**< 已安装的 ISR，或移植层的默认处理函数。 */
    void            *param;   /**< 传给 @ref handler 的不透明值。 */

#ifdef RT_USING_INTERRUPT_INFO
    char             name[RT_NAME_MAX]; /**< 用于诊断的向量名称。 */
    rt_uint32_t      counter;            /**< 累计分发次数。 */
#ifdef RT_USING_SMP
    /** 用于诊断中断亲和性和负载的各 CPU 分发次数。 */
    rt_ubase_t       cpu_counter[RT_CPUS_NR];
#endif
#endif
};

/**
 * @name 中断控制器接口
 *
 * BSP 通过本组接口初始化向量表/控制器、控制单个向量的投递并记录处理函数。向量编号及非法
 * 向量的处理方式由控制器决定。
 * @{
 */
/** 在驱动安装 ISR 前初始化中断子系统。 */
void rt_hw_interrupt_init(void);

/** 在中断控制器中阻止投递 @p vector。 */
void rt_hw_interrupt_mask(int vector);

/** 在中断控制器中允许投递 @p vector。 */
void rt_hw_interrupt_umask(int vector);

/**
 * @brief 为 @p vector 安装 @p handler 和不透明参数 @p param。
 * @param vector 当前控制器可识别的中断编号。
 * @param handler 要与该向量关联的中断上下文回调。
 * @param param 不透明回调参数；其存储期必须长于注册期。
 * @param name 启用中断统计时使用的诊断标签。
 * @return 按移植层约定返回之前安装的处理函数；通常无用户处理函数时返回 RT_NULL。
 * @note 安装处理函数不一定会解除该中断源的屏蔽。
 */
rt_isr_handler_t rt_hw_interrupt_install(int              vector,
                                         rt_isr_handler_t handler,
                                         void            *param,
                                         const char      *name);

/**
 * @brief 移除匹配的中断注册。
 *
 * 在支持检查或共享卸载的实现中，@p handler 和 @p param 共同标识该注册。回收回调数据前，
 * 调用者必须屏蔽中断源，并按所选中断控制器要求与正在执行的处理函数同步。
 *
 * @param vector 要移除注册的中断编号。
 * @param handler 先前注册的回调；移植层验证时会使用它。
 * @param param 先前注册的不透明参数或共享 IRQ 标识。
 */
void rt_hw_interrupt_uninstall(int              vector,
                               rt_isr_handler_t handler,
                               void            *param);
/** @} */

#ifdef RT_USING_SMP
/**
 * @brief 仅在调用 CPU 时禁用中断。
 * @return 先前的本地硬件中断状态。将此精确值传递给 rt_hw_local_irq_enable() 以保留嵌套和先前的掩码状态。
 */
rt_base_t rt_hw_local_irq_disable(void);

/** 恢复local_irq_disable()保存的调用CPU的中断状态。 */
void rt_hw_local_irq_enable(rt_base_t level);

/**
 * @brief 输入 SMP 范围的 CPU 临界区。
 * @return rt_cpus_unlock()之前的本地中断状态。
 *
 * 对于当前线程，常见实现将全局 CPU spinlock（每个线程嵌套）与本地中断/调度程序排除相结合。  在早期启动或没有当前线程的其他上下文中，它仅禁用本地中断并且不获取全局自旋锁。  调用者不得在该特殊上下文中假设跨 CPU 排除。
 */
rt_base_t rt_cpus_lock(void);

/** 离开 SMP 范围的临界区并恢复保存的中断状态。 */
void rt_cpus_unlock(rt_base_t level);

/* 通用内核关键部分在 SMP 构建中使用 SMP 范围的锁。 */
#define rt_hw_interrupt_disable rt_cpus_lock
#define rt_hw_interrupt_enable rt_cpus_unlock
#else
/**
 * @brief 禁用单处理器上的可屏蔽中断。
 * @return 先前的中断状态，稍后必须逐字恢复。
 *
 * 嵌套关键部分通过保存每个返回的状态并按 LIFO 顺序恢复它们来工作。调用者不得用无条件硬件使能来代替恢复，因为中断可能已经在进入时被禁用。
 */
rt_base_t rt_hw_interrupt_disable(void);

/** 恢复rt_hw_interrupt_disable()返回的中断状态。 */
void rt_hw_interrupt_enable(rt_base_t level);

/* 在UP上，本地和通用中断排除是相同的操作。 */
#define rt_hw_local_irq_disable rt_hw_interrupt_disable
#define rt_hw_local_irq_enable rt_hw_interrupt_enable

#endif /*RT_USING_SMP*/

/**
 * 查询调用CPU是否禁止可屏蔽中断。
 *
 * 当准确查询可用时，CPU 端口应覆盖弱通用实现。  弱默认返回 RT_FALSE，因此调用者不得将此例程视为未实现端口上的硬件状态保证。
 *
 * @return 端口报告禁用状态，或来自弱回退的 RT_FALSE。
 */
rt_bool_t rt_hw_interrupt_is_disabled(void);

/**
 * @name 架构上下文切换接口
 *
 * 这些例程由 CPU 端口实现，通常在汇编中。 @p from 和 @p to 标识与保存的堆栈指针而不是普通堆栈值相关的位置；它们的确切表示是调度程序和端口共享的 ABI。 `*_to` 形式启动第一个线程并且没有传出上下文。 `*_interrupt` 使用端口的立即或延迟切换机制请求或执行来自中断上下文的切换。
 *
 * 调用者不得将它们用作通用线程 API。调度程序锁定、中断嵌套、FPU 状态、地址空间切换以及第一个切换是否返回都是架构敏感的。
 * @{
 */
#ifdef RT_USING_SMP
/* SMP 端口接收传入的 TCB 以进行 CPU/地址空间簿记。 */
/** 将传出线程上下文保存在 @p from 并在线程上下文中恢复 @p to。 */
void rt_hw_context_switch(rt_ubase_t from, rt_ubase_t to, struct rt_thread *to_thread);
/** 恢复 @p to 处的第一个可运行上下文；没有传出线程。 */
void rt_hw_context_switch_to(rt_ubase_t to, struct rt_thread *to_thread);
/**
 * 从 @p context 描述的中断上下文切换/延迟，保存 @p from 并选择 @p to 作为传入保存的堆栈上下文。
 */
void rt_hw_context_switch_interrupt(void *context, rt_ubase_t from, rt_ubase_t to, struct rt_thread *to_thread);
#else
/** 将传出线程上下文保存在 @p from 并在线程上下文中恢复 @p to。 */
void rt_hw_context_switch(rt_ubase_t from, rt_ubase_t to);
/** 恢复 @p to 处的第一个可运行上下文；没有传出线程。 */
void rt_hw_context_switch_to(rt_ubase_t to);
/**
 * 在中断上下文中切换/延迟。 TCB 参数允许端口检查属于传出和传入线程的状态以及保存的堆栈上下文位置 @p from 和 @p to。
 */
void rt_hw_context_switch_interrupt(rt_ubase_t from, rt_ubase_t to, rt_thread_t from_thread, rt_thread_t to_thread);
#endif /*RT_USING_SMP*/
/** @} */

/**
 * @brief 行走一个堆栈帧所需的最小机器状态。
 *
 * 端口可以将 @ref fp 解释为帧指针、堆栈游标或其他展开 cookie。调用者必须将这两个字段视为下一个展开操作的不透明输入。
 */
struct rt_hw_backtrace_frame {
    rt_uintptr_t fp; /**< 架构定义的框架/展开光标。 */
    rt_uintptr_t pc; /**< 此帧表示的程序计数器。 */
};

/**
 * @brief 获取@p thread的第一个放卷帧。
 * @param thread 目标线程；对另一个 CPU 上当前正在执行的线程的支持是特定于体系结构的。
 * @param frame 成功时初始化输出帧。
 * @return RT_EOK 表示成功，或者当没有可用帧时出现负错误。
 */
rt_err_t rt_hw_backtrace_frame_get(rt_thread_t thread, struct rt_hw_backtrace_frame *frame);

/**
 * @brief 将 @p frame 前进到其调用者框架。
 * @return RT_EOK（如果生产了另一个框架）；负错误标记跟踪的结束或无效/不可展开的堆栈。
 */
rt_err_t rt_hw_backtrace_frame_unwind(rt_thread_t thread, struct rt_hw_backtrace_frame *frame);

/**
 * @brief 将 NUL 结尾的字符串写入 BSP 的最低级控制台。
 *
 * 内核格式化输出最终使用此后端。它可以在完整设备初始化之前或通过诊断达到。 ISR 中是否安全以及并发输出是否串行化是 BSP 特定的。
 */
void rt_hw_console_output(const char *str);

/**
 * @brief 显示从机器地址@p addr 开始的内存。
 * @param size 请求显示长度/计数；它的单位和舍入由架构实现定义。
 * @note 用于低级诊断。调用者负责地址有效性、权限、对齐以及可能的访问错误。
 */
void rt_hw_show_memory(rt_uint32_t addr, rt_size_t size);

/**
 * @brief 安装架构异常回调钩子。
 *
 * 在实现异常挂钩分派的端口上，该寄存器
 * @p exception_handle 用于特定于体系结构的已保存异常上下文。无论回调是保留还是调用，上下文表示、其 rt_err_t 结果的解释以及 RT_NULL 行为都是 CPU 端口特定的；某些端口仅提供兼容性存根。  实际调度的回调在异常上下文中运行，不得阻塞，也不得保留指向瞬态帧的指针。
 */
void rt_hw_exception_install(rt_err_t (*exception_handle)(void *context));

/**
 * @brief 请求大约 @p us 微秒的硬件延迟。
 *
 * BSP 实现通常提供适合短硬件时序和早期启动的校准忙等待。  弱通用回退不会延迟：它记录不支持的操作警告并返回。  因此，需要时序正确性的代码必须确保活动的 BSP 覆盖该符号；精度和最大实际间隔是 BSP 特定的。
 */
void rt_hw_us_delay(rt_uint32_t us);

/**
 * @return 调用 CPU 的逻辑 ID，在 RT-Thread 预期的范围内。单处理器端口通常返回零。
 */
int rt_hw_cpu_id(void);

#if defined(RT_USING_SMP) || defined(RT_USING_AMP)
/**
 * @brief 向选定的 CPU 发送处理器间中断。
 * @param ipi_vector 架构/控制器特定的 IPI 向量编号。
 * @param cpu_mask 目标逻辑CPU的位掩码；位 N 选择 CPU N。
 *
 * 该操作仅引发中断。在 IPI 之前发布的数据的关联处理程序和内存排序属于 SMP/AMP 子系统和 CPU 端口。
 */
void rt_hw_ipi_send(int ipi_vector, unsigned int cpu_mask);
#endif

#ifdef RT_USING_SMP

/** 将硬件自旋锁初始化为其解锁状态。 */
void rt_hw_spin_lock_init(rt_hw_spinlock_t *lock);

/**
 * @brief 获取@p lock，旋转直至获得所有权。
 *
 * 该原语不暗示中断状态处理；调用者必须使用受保护数据所需的 IRQ/调度程序锁定协议。自旋锁不是睡眠锁，必须仅保护有界的关键部分。
 */
void rt_hw_spin_lock(rt_hw_spinlock_t *lock);

/** 释放@p lock，并根据端口ABI发布受保护的写入。 */
void rt_hw_spin_unlock(rt_hw_spinlock_t *lock);

/** 通用 SMP 范围的 CPU 锁实现使用的全局硬件锁。 */
extern rt_hw_spinlock_t _cpus_lock;

/* 运行时初始化之前存在的锁的常量初始值设定项。 */
#define __RT_HW_SPIN_LOCK_INITIALIZER(lockname) {0}

/** 为 SMP 硬件锁生成类型化的未锁定初始化表达式。 */
#define __RT_HW_SPIN_LOCK_UNLOCKED(lockname)    \
    (rt_hw_spinlock_t) __RT_HW_SPIN_LOCK_INITIALIZER(lockname)

/** 定义具有静态解锁初始化的命名硬件自旋锁 @p x。 */
#define RT_DEFINE_HW_SPINLOCK(x)  rt_hw_spinlock_t x = __RT_HW_SPIN_LOCK_UNLOCKED(x)

/**
 * @brief 在 SMP 初始化期间启动配置的辅助 CPU。
 *
 * BSP 为配置的辅助 CPU 执行特定于平台的复位释放或固件调用。  单独的启动尝试可能会失败，或者平台可能会使 CPU 处于离线状态；只有成功启动的 CPU 才会进入 RT-Thread secondary-CPU 路径。
 */
void rt_hw_secondary_cpu_up(void);

/**
 * @brief 在辅助 CPU 上执行架构空闲操作。
 *
 * 这通常从辅助空闲路径调用，并且通常是等待中断指令加上所需的平台簿记。
 */
void rt_hw_secondary_cpu_idle_exec(void);

#else /* !RT_USING_SMP */

/*
 * UP 兼容形式：存储保存锁上保存的中断状态。它不是 CPU 间的锁定，并且必须向匹配的解锁提供相同的变量，以便可以恢复准确的先前中断状态。
 */
#define RT_DEFINE_HW_SPINLOCK(x)    rt_ubase_t x

/** 将中断状态保存到 @p lock 中并排除本地中断并发。 */
#define rt_hw_spin_lock(lock)     *(lock) = rt_hw_interrupt_disable()
/** 恢复rt_hw_spin_lock()之前存储的中断状态。 */
#define rt_hw_spin_unlock(lock)   rt_hw_interrupt_enable(*(lock))


#endif /* RT_USING_SMP */

#ifndef RT_USING_CACHE
    /*
 * 此配置分支提供兼容性无操作，因此通用代码可以在没有 RT_USING_CACHE 的情况下进行编译。  缓存配置本身并不能证明 CPU 不需要排序障碍。  需要真正排序的端口必须安排适当的架构定义和功能配置，而不是依赖这些后备。
 */
    /** 此配置中的兼容性无操作；不是硬件订购保证。 */
    #define rt_hw_isb()
    /** 此配置中的兼容性无操作；不是硬件订购保证。 */
    #define rt_hw_dmb()
    /** 此配置中的兼容性无操作；不是硬件订购保证。 */
    #define rt_hw_dsb()
#endif /* RT_USING_CACHE */

#ifdef __cplusplus
}
#endif

#endif
