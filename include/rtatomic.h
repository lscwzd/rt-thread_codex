/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2023-03-14     WangShun     初始版本
 * 2023-05-20     Bernard      增加 stdc 原子操作检测。
 * 2026-03-09     wdfk-prog    增加 8/16 位原子操作支持
 */

/**
 * @file rtatomic.h
 * @brief 与底层实现无关的原子操作及原子链表辅助接口。
 *
 * RT-Thread 向上层提供统一的 fetch 型原子操作接口，同时允许 BSP/体系结构从
 * 以下三种实现中选择一种：
 *
 * 1. `RT_USING_STDC_ATOMIC`：C11 `<stdatomic.h>` 原语。这里使用的无后缀标准
 *    函数采用顺序一致性内存序。
 * 2. `RT_USING_HW_ATOMIC`：CPU 移植层提供的原语，通常由独占加载/存储、
 *    比较并交换或等效指令实现。此接口不为所有移植层强制规定统一的内存序；
 *    屏障以及获取/释放的强度由各体系结构实现自行定义。
 * 3. 软件后备实现：通过 rt_hw_interrupt_disable() 进入一个很短的临界区。
 *
 * fetch 加/减、按位操作和 exchange 都返回修改*前*观察到的值。相反，
 * compare-and-exchange 以布尔值报告是否成功：失败时会通过 `expected` 写回
 * 实际读到的值。flag test-and-set 返回操作前的清除/置位状态，但调用者不应
 * 假定每个后端都会把返回值规范化为恰好零或一。
 *
 * API 中的 `volatile` 可防止普通的读/写操作被不恰当地省略，但它本身不是
 * 同步机制。硬件后端的内存排序属于 CPU 移植层约定的一部分。软件后备实现
 * 使用 RT-Thread 的通用排他原语：在 UP 上屏蔽本地中断，而在 SMP 构建中该
 * 原语映射为 rt_cpus_lock()/rt_cpus_unlock()。因此它能使操作串行化，但不是
 * 无锁实现；该通用锁对 CPU 移植和早期启动阶段的限制仍然适用。
 *
 * 当前仅为 C 编译单元提供此接口。原子存储类型别名仍可供 C++ 使用，但本
 * 头文件不会尝试把 C11 `<stdatomic.h>` 的语义映射到行为可能不同的 C++
 * 工具链实现。
 */
#ifndef __RT_ATOMIC_H__
#define __RT_ATOMIC_H__

/* 提供原子存储类型、内联属性和中断控制原语。 */
#include <rthw.h>

#if !defined(__cplusplus)

/**
 * @name 体系结构原子原语约定
 *
 * 当 `RT_USING_HW_ATOMIC` 选择硬件后端时，CPU 移植层必须提供这些函数。
 * 与本机字长相同宽度的操作使用 `rt_atomic_t`；可选的字节和半字操作分别由
 * `ARCH_USING_HW_ATOMIC_8` 和 `ARCH_USING_HW_ATOMIC_16` 选择。
 * @{
 */

/** 以原子方式读取一个本机字宽对象，并返回其当前值。 */
rt_atomic_t rt_hw_atomic_load(volatile rt_atomic_t *ptr);

/** 以原子方式用 @p val 替换一个本机字宽对象。 */
void rt_hw_atomic_store(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** 以原子方式读取一个 8 位对象，并返回其当前值。 */
rt_atomic8_t rt_hw_atomic_load8(volatile rt_atomic8_t *ptr);

/** 以原子方式用 @p val 替换一个 8 位对象。 */
void rt_hw_atomic_store8(volatile rt_atomic8_t *ptr, rt_atomic8_t val);

/** 以原子方式读取一个 16 位对象，并返回其当前值。 */
rt_atomic16_t rt_hw_atomic_load16(volatile rt_atomic16_t *ptr);

/** 以原子方式用 @p val 替换一个 16 位对象。 */
void rt_hw_atomic_store16(volatile rt_atomic16_t *ptr, rt_atomic16_t val);

/** 以原子方式加上 @p val，并返回相加前的值。 */
rt_atomic_t rt_hw_atomic_add(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** 以原子方式减去 @p val，并返回相减前的值。 */
rt_atomic_t rt_hw_atomic_sub(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** 以原子方式对 8 位对象执行按位 AND，并返回操作前的值。 */
rt_atomic8_t rt_hw_atomic_and8(volatile rt_atomic8_t *ptr, rt_atomic8_t val);

/** 以原子方式对 8 位对象执行按位 OR，并返回操作前的值。 */
rt_atomic8_t rt_hw_atomic_or8(volatile rt_atomic8_t *ptr, rt_atomic8_t val);

/** 以原子方式对 16 位对象执行按位 AND，并返回操作前的值。 */
rt_atomic16_t rt_hw_atomic_and16(volatile rt_atomic16_t *ptr, rt_atomic16_t val);

/** 以原子方式对 16 位对象执行按位 OR，并返回操作前的值。 */
rt_atomic16_t rt_hw_atomic_or16(volatile rt_atomic16_t *ptr, rt_atomic16_t val);

/** 以原子方式对本机字宽对象执行按位 AND，并返回操作前的值。 */
rt_atomic_t rt_hw_atomic_and(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** 以原子方式对本机字宽对象执行按位 OR，并返回操作前的值。 */
rt_atomic_t rt_hw_atomic_or(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** 以原子方式对本机字宽对象执行按位 XOR，并返回操作前的值。 */
rt_atomic_t rt_hw_atomic_xor(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** 以原子方式替换一个本机字宽对象，并返回其操作前的值。 */
rt_atomic_t rt_hw_atomic_exchange(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** 以原子方式将标志对象清零。 */
void rt_hw_atomic_flag_clear(volatile rt_atomic_t *ptr);

/** 以原子方式将标志对象置为一，并返回其此前为零还是非零的状态。 */
rt_atomic_t rt_hw_atomic_flag_test_and_set(volatile rt_atomic_t *ptr);

/**
 * 将 @p ptr 的值与 `*expected` 比较，只有相等时才存入 @p desired。
 *
 * @param ptr 要测试且可能更新的对象。
 * @param expected 期望的输入值；若比较失败，会用实际观察到的值覆盖它。
 * @param desired 比较成功时要存入的值。
 * @return 替换成功时为非零，值不匹配时为零。
 */
rt_atomic_t rt_hw_atomic_compare_exchange_strong(volatile rt_atomic_t *ptr, rt_atomic_t *expected, rt_atomic_t desired);

/** @} */

#if defined(RT_USING_STDC_ATOMIC)

/*
 * 即使处于 C11 模式，C 标准也允许实现定义 __STDC_NO_ATOMICS__。对于明确
 * 配置为使用标准原子后端的代码，本文件会拒绝该组合，而不会静默降级。
 *
 * 下面的 fetch 算术/按位操作和 exchange 返回操作前的值。通用 C11 宏会根据
 * 指针所指向的 `_Atomic` 类型推断 8 位、16 位或本机字宽操作。这里没有显式
 * 指定内存序，因此使用 C11 默认的 memory_order_seq_cst。
 *
 * 下面保持不变的 flag 宏存在兼容性注意事项：rt_atomic_t 是原子整数，而不是
 * C11 `atomic_flag`，但它被传给 atomic_flag_clear/test_and_set。该类型不匹配
 * 不属于 C11 API 合约，某些实现只会操作 flag 大小的存储空间。不要从这个
 * 分支推断本机字宽 flag 语义；使用这些宏的配置必须验证工具链行为，或选择
 * 已修正的移植层/后端实现。
 */
#ifndef __STDC_NO_ATOMICS__
#define rt_atomic_load(ptr) atomic_load(ptr)
#define rt_atomic_store(ptr, v) atomic_store(ptr, v)
#define rt_atomic_load8(ptr) atomic_load(ptr)
#define rt_atomic_store8(ptr, v) atomic_store(ptr, v)
#define rt_atomic_load16(ptr) atomic_load(ptr)
#define rt_atomic_store16(ptr, v) atomic_store(ptr, v)
#define rt_atomic_add(ptr, v) atomic_fetch_add(ptr, v)
#define rt_atomic_sub(ptr, v) atomic_fetch_sub(ptr, v)
#define rt_atomic_and8(ptr, v) atomic_fetch_and(ptr, v)
#define rt_atomic_or8(ptr, v) atomic_fetch_or(ptr, v)
#define rt_atomic_and16(ptr, v) atomic_fetch_and(ptr, v)
#define rt_atomic_or16(ptr, v) atomic_fetch_or(ptr, v)
#define rt_atomic_and(ptr, v) atomic_fetch_and(ptr, v)
#define rt_atomic_or(ptr, v)  atomic_fetch_or(ptr, v)
#define rt_atomic_xor(ptr, v) atomic_fetch_xor(ptr, v)
#define rt_atomic_exchange(ptr, v) atomic_exchange(ptr, v)
#define rt_atomic_flag_clear(ptr) atomic_flag_clear(ptr)
#define rt_atomic_flag_test_and_set(ptr) atomic_flag_test_and_set(ptr)
#define rt_atomic_compare_exchange_strong(ptr, v,des) atomic_compare_exchange_strong(ptr, v ,des)
#else
#error "The standard library C doesn't support the atomic operation"
#endif /* __STDC_NO_ATOMICS__ */

#elif defined(RT_USING_HW_ATOMIC)
/*
 * 本机字宽操作始终转交给 CPU 移植层。字节和半字支持会独立声明：CPU 可能有
 * 原子字操作指令，却没有原子的子字读/改/写指令。
 */
#define rt_atomic_load(ptr) rt_hw_atomic_load(ptr)
#define rt_atomic_store(ptr, v) rt_hw_atomic_store(ptr, v)
#if defined(ARCH_USING_HW_ATOMIC_8)
/* 该体系结构保证 8 位原子原语可由硬件安全执行。 */
#define rt_atomic_load8(ptr) rt_hw_atomic_load8(ptr)
#define rt_atomic_store8(ptr, v) rt_hw_atomic_store8(ptr, v)
#define rt_atomic_and8(ptr, v) rt_hw_atomic_and8(ptr, v)
#define rt_atomic_or8(ptr, v)  rt_hw_atomic_or8(ptr, v)
#else
/*
 * 缺少 8 位硬件支持时，映射到软件辅助函数名。按本头文件当前的条件编译
 * 布局，这些内联辅助函数只会在下面完整的软件后端分支中生成。因此，缺少
 * ARCH_USING_HW_ATOMIC_8 的 HW-atomic 配置若其移植层未提供兼容辅助函数，
 * 接口就无法解析。这是配置缺口，并非一定可用的通用后备实现。
 */
#define rt_atomic_load8(ptr) rt_soft_atomic_load8(ptr)
#define rt_atomic_store8(ptr, v) rt_soft_atomic_store8(ptr, v)
#define rt_atomic_and8(ptr, v) rt_soft_atomic_and8(ptr, v)
#define rt_atomic_or8(ptr, v)  rt_soft_atomic_or8(ptr, v)
#endif
#if defined(ARCH_USING_HW_ATOMIC_16)
/* 该体系结构保证 16 位原子原语可由硬件安全执行。 */
#define rt_atomic_load16(ptr) rt_hw_atomic_load16(ptr)
#define rt_atomic_store16(ptr, v) rt_hw_atomic_store16(ptr, v)
#define rt_atomic_and16(ptr, v) rt_hw_atomic_and16(ptr, v)
#define rt_atomic_or16(ptr, v)  rt_hw_atomic_or16(ptr, v)
#else
/*
 * 缺少 16 位硬件支持时，映射到软件辅助函数名。与 8 位情形相同，下面的内联
 * 定义在该 HW 分支中会被排除；移植层/配置必须提供这些函数，或启用 16 位
 * 硬件支持。仅有宏映射并不能使 API 可以链接。
 */
#define rt_atomic_load16(ptr) rt_soft_atomic_load16(ptr)
#define rt_atomic_store16(ptr, v) rt_soft_atomic_store16(ptr, v)
#define rt_atomic_and16(ptr, v) rt_soft_atomic_and16(ptr, v)
#define rt_atomic_or16(ptr, v)  rt_soft_atomic_or16(ptr, v)
#endif
#define rt_atomic_add(ptr, v) rt_hw_atomic_add(ptr, v)
#define rt_atomic_sub(ptr, v) rt_hw_atomic_sub(ptr, v)
#define rt_atomic_and(ptr, v) rt_hw_atomic_and(ptr, v)
#define rt_atomic_or(ptr, v)  rt_hw_atomic_or(ptr, v)
#define rt_atomic_xor(ptr, v) rt_hw_atomic_xor(ptr, v)
#define rt_atomic_exchange(ptr, v) rt_hw_atomic_exchange(ptr, v)
#define rt_atomic_flag_clear(ptr) rt_hw_atomic_flag_clear(ptr)
#define rt_atomic_flag_test_and_set(ptr) rt_hw_atomic_flag_test_and_set(ptr)
#define rt_atomic_compare_exchange_strong(ptr, v,des) rt_hw_atomic_compare_exchange_strong(ptr, v ,des)

#else
/*
 * 未选择标准或 CPU 原子后端。把每个公开操作映射到下面的内联临界区实现。
 * rt_hw_interrupt_disable() 返回调用前的中断状态，随后
 * rt_hw_interrupt_enable(level) 会原样恢复它；因此，按照 UP 或 SMP CPU
 * 移植层的约定，这些辅助函数可在已进入排他区或嵌套的上下文中安全调用。
 */
#include <rthw.h>
#define rt_atomic_load(ptr) rt_soft_atomic_load(ptr)
#define rt_atomic_store(ptr, v) rt_soft_atomic_store(ptr, v)
#define rt_atomic_load8(ptr) rt_soft_atomic_load8(ptr)
#define rt_atomic_store8(ptr, v) rt_soft_atomic_store8(ptr, v)
#define rt_atomic_load16(ptr) rt_soft_atomic_load16(ptr)
#define rt_atomic_store16(ptr, v) rt_soft_atomic_store16(ptr, v)
#define rt_atomic_add(ptr, v) rt_soft_atomic_add(ptr, v)
#define rt_atomic_sub(ptr, v) rt_soft_atomic_sub(ptr, v)
#define rt_atomic_and8(ptr, v) rt_soft_atomic_and8(ptr, v)
#define rt_atomic_or8(ptr, v)  rt_soft_atomic_or8(ptr, v)
#define rt_atomic_and16(ptr, v) rt_soft_atomic_and16(ptr, v)
#define rt_atomic_or16(ptr, v)  rt_soft_atomic_or16(ptr, v)
#define rt_atomic_and(ptr, v) rt_soft_atomic_and(ptr, v)
#define rt_atomic_or(ptr, v)  rt_soft_atomic_or(ptr, v)
#define rt_atomic_xor(ptr, v) rt_soft_atomic_xor(ptr, v)
#define rt_atomic_exchange(ptr, v) rt_soft_atomic_exchange(ptr, v)
#define rt_atomic_flag_clear(ptr) rt_soft_atomic_flag_clear(ptr)
#define rt_atomic_flag_test_and_set(ptr) rt_soft_atomic_flag_test_and_set(ptr)
#define rt_atomic_compare_exchange_strong(ptr, v,des) rt_soft_atomic_compare_exchange_strong(ptr, v ,des)

/**
 * @brief 使用通用内核互斥机制以原子方式读取一个 8 位值。
 *
 * @param ptr 要读取的值的地址；在整个操作期间必须保持有效。
 * @return 持有通用互斥令牌期间观察到的值。
 */
rt_inline rt_atomic8_t rt_soft_atomic_load8(volatile rt_atomic8_t *ptr)
{
    rt_base_t level;
    rt_atomic8_t temp;

    level = rt_hw_interrupt_disable();
    temp = *ptr;
    rt_hw_interrupt_enable(level);

    return temp;
}

/**
 * @brief 使用通用内核互斥机制以原子方式存储一个 8 位值。
 *
 * @param ptr 目标对象的地址。
 * @param val 在恢复先前中断状态前写入的新值。
 */
rt_inline void rt_soft_atomic_store8(volatile rt_atomic8_t *ptr, rt_atomic8_t val)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    *ptr = val;
    rt_hw_interrupt_enable(level);
}

/**
 * @brief 使用通用内核互斥机制以原子方式读取一个 16 位值。
 *
 * 这会保护目标平台上的访问：否则，对齐的半字访问可能被修改同一逻辑对象的代码中断。
 *
 * @param ptr 要读取的值的地址。
 * @return 本次操作在保护范围内观察到的值。
 */
rt_inline rt_atomic16_t rt_soft_atomic_load16(volatile rt_atomic16_t *ptr)
{
    rt_base_t level;
    rt_atomic16_t temp;

    level = rt_hw_interrupt_disable();
    temp = *ptr;
    rt_hw_interrupt_enable(level);

    return temp;
}

/**
 * @brief 使用通用内核互斥机制以原子方式存储一个 16 位值。
 *
 * @param ptr 目标对象的地址。
 * @param val 要存储的新值。
 */
rt_inline void rt_soft_atomic_store16(volatile rt_atomic16_t *ptr, rt_atomic16_t val)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    *ptr = val;
    rt_hw_interrupt_enable(level);
}

/**
 * @brief 以原子方式对 8 位值执行 AND，并返回操作前的值。
 *
 * 新值为 `old & val`。返回 `old` 符合 fetch-and 语义，调用方可据此准确判断清除了
 * 哪些位。
 *
 * @param ptr 要更新的对象。
 * @param val 要与对象执行 AND 的位掩码。
 * @return 执行 AND 前的对象值。
 */
rt_inline rt_atomic8_t rt_soft_atomic_and8(volatile rt_atomic8_t *ptr, rt_atomic8_t val)
{
    rt_base_t level;
    rt_atomic8_t temp;

    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = temp & val;
    rt_hw_interrupt_enable(level);

    return temp;
}

/**
 * @brief 以原子方式对 8 位值执行 OR，并返回操作前的值。
 *
 * @param ptr 要更新的对象。
 * @param val 要在对象中置位的位掩码。
 * @return 执行 OR 前的对象值。
 */
rt_inline rt_atomic8_t rt_soft_atomic_or8(volatile rt_atomic8_t *ptr, rt_atomic8_t val)
{
    rt_base_t level;
    rt_atomic8_t temp;

    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = temp | val;
    rt_hw_interrupt_enable(level);

    return temp;
}

/**
 * @brief 以原子方式对 16 位值执行 AND，并返回操作前的值。
 *
 * @param ptr 要更新的对象。
 * @param val 要与对象执行 AND 的位掩码。
 * @return 执行 AND 前的对象值。
 */
rt_inline rt_atomic16_t rt_soft_atomic_and16(volatile rt_atomic16_t *ptr, rt_atomic16_t val)
{
    rt_base_t level;
    rt_atomic16_t temp;

    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = temp & val;
    rt_hw_interrupt_enable(level);

    return temp;
}

/**
 * @brief 以原子方式对 16 位值执行 OR，并返回操作前的值。
 *
 * @param ptr 要更新的对象。
 * @param val 要在对象中置位的位掩码。
 * @return 执行 OR 前的对象值。
 */
rt_inline rt_atomic16_t rt_soft_atomic_or16(volatile rt_atomic16_t *ptr, rt_atomic16_t val)
{
    rt_base_t level;
    rt_atomic16_t temp;

    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = temp | val;
    rt_hw_interrupt_enable(level);

    return temp;
}

/**
 * @brief 以原子方式替换本机字宽对象，并返回其旧值。
 *
 * @param ptr 要替换的对象。
 * @param val 新值。
 * @return 替换前 @p ptr 保存的值。
 */
rt_inline rt_atomic_t rt_soft_atomic_exchange(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = val;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief 以原子方式为本机字宽对象相加，并返回其旧值。
 *
 * 此函数执行所选后端的普通原子加法，不会饱和处理或检查溢出。由于 rt_atomic_t
 * 为有符号类型，为保证可移植性，调用方必须避免使数学和超出其取值范围；不要依赖
 * C11、硬件和软件后端具有相同的回绕行为。
 *
 * @param ptr 要更新的计数器。
 * @param val 增量；对于有符号基础类型，它可以为负。
 * @return 相加前的计数器值。
 */
rt_inline rt_atomic_t rt_soft_atomic_add(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr += val;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief 以原子方式从本机字宽对象相减，并返回其旧值。
 *
 * 与 rt_soft_atomic_add() 相同，调用方必须让数学结果保持在有符号 rt_atomic_t
 * 的取值范围内，而不能依赖溢出行为。
 *
 * @param ptr 要更新的计数器。
 * @param val 要减去的数值。
 * @return 相减前的计数器值。
 */
rt_inline rt_atomic_t rt_soft_atomic_sub(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr -= val;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief 以原子方式对本机字宽值执行 XOR，并返回操作前的值。
 *
 * @param ptr 要更新的位字段。
 * @param val 要翻转的位掩码。
 * @return 执行 XOR 前的位字段值。
 */
rt_inline rt_atomic_t rt_soft_atomic_xor(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = (*ptr) ^ val;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief 以原子方式对本机字宽值执行 AND，并返回操作前的值。
 *
 * @param ptr 要更新的位字段。
 * @param val 执行 AND 后要保留的位掩码。
 * @return 执行 AND 前的位字段值。
 */
rt_inline rt_atomic_t rt_soft_atomic_and(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = (*ptr) & val;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief 以原子方式对本机字宽值执行 OR，并返回操作前的值。
 *
 * @param ptr 要更新的位字段。
 * @param val 要置位的位掩码。
 * @return 执行 OR 前的位字段值。
 */
rt_inline rt_atomic_t rt_soft_atomic_or(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = (*ptr) | val;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief 在通用内核互斥保护下以原子方式读取本机字宽值。
 *
 * @param ptr 要读取的对象地址。
 * @return 在临界区内观察到的值。
 */
rt_inline rt_atomic_t rt_soft_atomic_load(volatile rt_atomic_t *ptr)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief 在通用内核互斥保护下以原子方式存储本机字宽值。
 *
 * @param ptr 目标对象的地址。
 * @param val 新值。
 */
rt_inline void rt_soft_atomic_store(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    level = rt_hw_interrupt_disable();
    *ptr = val;
    rt_hw_interrupt_enable(level);
}

/**
 * @brief 设置原子标志，并报告它此前是否已置位。
 *
 * 对象会被改为一。返回零表示本次调用取得了此前清除的标志；返回一表示观察到的值
 * 非零。返回值会被规范化为零或一，而不会返回任意的先前非零值。
 *
 * @param ptr 本机字宽标志对象，通常初始化为零。
 * @return 调用前为清除状态时返回零，否则返回一。
 */
rt_inline rt_atomic_t rt_soft_atomic_flag_test_and_set(volatile rt_atomic_t *ptr)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    if (*ptr == 0)
    {
        temp = 0;
        *ptr = 1;
    }
    else
        temp = 1;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief 清除原子标志。
 *
 * @param ptr 要重置为零的本机字宽标志对象。
 */
rt_inline void rt_soft_atomic_flag_clear(volatile rt_atomic_t *ptr)
{
    rt_base_t level;
    level = rt_hw_interrupt_disable();
    *ptr = 0;
    rt_hw_interrupt_enable(level);
}

/**
 * @brief 在通用内核互斥保护下实现的强比较并交换操作。
 *
 * 不存在伪失败路径：只要相等，就一定会用 @p desired 替换 `*ptr1`。不相等时，
 * 会将观察到的值复制到 `*ptr2`，从而让调用方的重试循环能与最新观察值比较。
 * `ptr1` 和 `ptr2` 在整个临界区内都应指向有效对象，通常不应彼此别名。
 *
 * @param ptr1 要测试并在满足条件时修改的原子对象。
 * @param ptr2 输入和输出的期望值。
 * @param desired 当 `*ptr1 == *ptr2` 时使用的替换值。
 * @return 发生替换时返回一，不匹配时返回零。
 */
rt_inline rt_atomic_t rt_soft_atomic_compare_exchange_strong(volatile rt_atomic_t *ptr1, rt_atomic_t *ptr2,
        rt_atomic_t desired)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    if ((*ptr1) != (*ptr2))
    {
        *ptr2 = *ptr1;
        temp = 0;
    }
    else
    {
        *ptr1 = desired;
        temp = 1;
    }
    rt_hw_interrupt_enable(level);
    return temp;
}
#endif /* RT_USING_STDC_ATOMIC */

/**
 * @brief 将原子计数器减一，并测试其是否到达零。
 *
 * `rt_atomic_sub()` 返回相减前的值，因此旧值为一恰好表示发生了到零的转换。
 *
 * @param ptr 要减一的计数器。
 * @return 仅在从一变为零时返回 RT_TRUE；否则返回 RT_FALSE。
 */
rt_inline rt_bool_t rt_atomic_dec_and_test(volatile rt_atomic_t *ptr)
{
    return rt_atomic_sub(ptr, 1) == 1;
}

/**
 * @brief 仅当当前值不等于 @p u 时加上 @p a。
 *
 * 循环从一个快照开始。比较并交换失败时，会用导致失败的值刷新 `c`，因此下一次
 * 循环会使用新值重试；若该值为 @p u，则停止。强原语保证循环的每一次失败都对应
 * 真实的并发干扰，而非允许发生的伪失败。
 *
 * 每次计算表达式 `c + a` 时，结果都必须可由 rt_atomic_t 表示；有符号溢出不是
 * 可移植的原子回绕语义。
 *
 * @param ptr 要更新的计数器。
 * @param a 要加上的数值。
 * @param u 禁止相加时使用的哨兵值。
 * @return 成功相加前立即观察到的值；若禁止相加，则返回 @p u。
 */
rt_inline rt_atomic_t rt_atomic_fetch_add_unless(volatile rt_atomic_t *ptr, rt_atomic_t a, rt_atomic_t u)
{
    rt_atomic_t c = rt_atomic_load(ptr);

    do {
        if (c == u)
        {
            break;
        }
    } while (!rt_atomic_compare_exchange_strong(ptr, &c, c + a));

    return c;
}

/**
 * @brief 仅当计数器不等于 @p u 时加上 @p a，并返回成功状态。
 *
 * @return 值被修改时返回 RT_TRUE；值等于 @p u 时返回 RT_FALSE。
 */
rt_inline rt_bool_t rt_atomic_add_unless(volatile rt_atomic_t *ptr, rt_atomic_t a, rt_atomic_t u)
{
    return rt_atomic_fetch_add_unless(ptr, a, u) != u;
}

/**
 * @brief 仅在引用计数器非零时将其加一。
 *
 * 当零表示对象已不可再取得新引用时，此函数很有用。它本身并不管理对象生命周期；
 * 对象回收仍须由所有者协调。
 *
 * @return 成功加一时返回 RT_TRUE；观察到零时返回 RT_FALSE。
 */
rt_inline rt_bool_t rt_atomic_inc_not_zero(volatile rt_atomic_t *ptr)
{
    return rt_atomic_add_unless(ptr, 1, 0);
}

/**
 * @brief 初始化一个空的原子单向链栈或链表头节点。
 *
 * 此直接赋值仅用于头节点向并发上下文可见前的初始化。头节点发布后，应通过
 * enqueue/dequeue 更新它。
 *
 * @param l 要初始化的头节点；其原子 next 字段会被设为零。
 */
rt_inline void rt_ll_slist_init(rt_ll_slist_t *l)
{
    l->next = 0;
}

/**
 * @brief 将一个节点压入原子单向链表的头部。
 *
 * 此函数反复将 @p n 连接到观察到的头节点，并通过比较并交换将头节点替换为 @p n。
 * 如果另一生产者先成功，比较并交换会刷新 `exp`，重新构建 `n->next` 后再次尝试
 * 压入。因此，尽管历史名称为 `enqueue`，该操作实际具有后进先出行为。
 *
 * @param l 共享链表头节点。
 * @param n 要压入的脱离节点。它不得已从任何链表可达，并且在并发上下文仍可能
 *        观察到它时必须保持存活。
 */
rt_inline void rt_ll_slist_enqueue(rt_ll_slist_t *l, rt_ll_slist_t *n)
{
    rt_base_t exp;
    exp = rt_atomic_load(&l->next);
    do
    {
        n->next = exp;
    } while (!rt_atomic_compare_exchange_strong(&l->next, &exp, (rt_base_t)n));
}

/**
 * @brief 弹出并返回当前原子链表的头节点。
 *
 * 非空循环会读取候选头节点的后继节点，然后尝试将该后继节点发布为新的链表头节点。
 * 发生并发干扰时会刷新 `exp` 并重试。链表为空时返回 RT_NULL。
 *
 * 此仅使用指针的算法未给头节点附加代数计数，因此不能独立防止 ABA 问题。当其他
 * 上下文仍可能将节点作为已观察到的候选节点持有时，不得释放或复用该节点；使用者
 * 需要采用适当的回收和生命周期管理规则。
 *
 * @param l 共享链表头节点。
 * @return 被移除的节点；链表为空时为 RT_NULL。
 */
rt_inline rt_ll_slist_t *rt_ll_slist_dequeue(rt_ll_slist_t *l)
{
    rt_base_t exp;
    rt_ll_slist_t *head;

    exp = rt_atomic_load(&l->next);
    do
    {
        head = (rt_ll_slist_t *)exp;
    } while (head && !rt_atomic_compare_exchange_strong(&l->next, &exp, rt_atomic_load(&head->next)));
    return head;
}

#endif /* __cplusplus */

#endif /* __RT_ATOMIC_H__ */
