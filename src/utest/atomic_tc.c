/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2022-07-27     flybreak     the first version
 * 2023-03-21     WangShun     add atomic test
 * 2023-09-15     xqyjlj       change stack size in cpu64
 * 2025-11-16     h0bbl3s      Add standardized utest documentation block
 */

/**
 * @file atomic_tc.c
 * @brief 验证原子读改写 API 的返回语义，以及多线程竞争下的不可分割性。
 *
 * 本测试分为两层。test_atomic_api 在单线程中逐项检查加、减、按位运算、交换、
 * 原子标志、加载/存储和强比较交换；每个断言既检查修改后的目标值，也检查 API
 * 返回的是“修改前旧值”还是成功布尔值。test_atomic_add 则让三个同优先级线程
 * 并发对共享 count 各加一百万次，并以完成信号量作为 join 屏障。最终值必须精确为
 * 三百万；若读改写不是原子的，竞争导致的丢失更新会使结果偏小。
 *
 * 该测试证明单次原子操作的线程安全性，不证明多个原子操作组合后自动成为事务。
 * 测试项名称为 `core.atomic`，超时 10 秒。
 */

#include <rtthread.h>
#include "utest.h"
#include "rtatomic.h"
#include <rthw.h>

#define THREAD_PRIORITY         25
#define THREAD_TIMESLICE        1
#define THREAD_STACKSIZE        UTEST_THR_STACK_SIZE

/* 根据指针宽度选择期望值，用于核对 rt_atomic_t 是否采用本架构的原生字宽。 */
#define ATOMIC_WORD(val_if_64, val_if_32)                                           \
    ((rt_atomic_t)((sizeof(void *) == sizeof(uint64_t)) ? (val_if_64) : (val_if_32)))

/* 三个工作线程共同更新的计数器；只能通过原子 API 访问。 */
static rt_atomic_t count = 0;
/* 初值为 0 的动态信号量；每个工作线程退出前 release 一次，主测试 take 三次。 */
static rt_sem_t sem_t;

/**
 * @brief 单线程逐项验证全部核心原子 API 的值语义。
 *
 * 前置条件是局部 base 没有并发访问。每个小节先设置已知输入，再断言目标的新值和
 * 返回的旧值；比较交换还覆盖“期望值相等而成功”和“不相等而保持目标不变”两条
 * 分支。局部变量无需清理。
 */
static void test_atomic_api(void)
{
    rt_atomic_t base;
    rt_atomic_t oldval;
    rt_atomic_t result;

    /* 原子类型应与当前 32/64 位指针的机器字宽一致。 */
    uassert_true(sizeof(rt_atomic_t) == ATOMIC_WORD(sizeof(uint64_t), sizeof(uint32_t)));

    /* rt_atomic_add */
    base = 0;
    result = rt_atomic_add(&base, 10);
    uassert_true(base == 10);
    uassert_true(result == 0);
    /* 负增量同时验证有符号值处理。 */
    base = 2;
    result = rt_atomic_add(&base, -4);
    uassert_true(base == -2);
    uassert_true(result == 2);

    /* rt_atomic_sub */
    base = 11;
    result = rt_atomic_sub(&base, 10);
    uassert_true(base == 1);
    uassert_true(result == 11);
    /* 减去负数等价于增加。 */
    base = 2;
    result = rt_atomic_sub(&base, -5);
    uassert_true(base == 7);
    uassert_true(result == 2);

    /* rt_atomic_or */
    base = 0xFF00;
    result = rt_atomic_or(&base, 0x0F0F);
    uassert_true(base == 0xFF0F);
    uassert_true(result == 0xFF00);

    /* rt_atomic_xor */
    base = 0xFF00;
    result = rt_atomic_xor(&base, 0x0F0F);
    uassert_true(base == 0xF00F);
    uassert_true(result == 0xFF00);

    /* rt_atomic_and */
    base = 0xFF00;
    result = rt_atomic_and(&base, 0x0F0F);
    uassert_true(base == 0x0F00);
    uassert_true(result == 0xFF00);

    /* rt_atomic_exchange */
    base = 0xFF00;
    result = rt_atomic_exchange(&base, 0x0F0F);
    uassert_true(base == 0x0F0F);
    uassert_true(result == 0xFF00);

    /* 原标志为 0：置 1，并返回旧值 0。 */
    base = 0x0;
    result = rt_atomic_flag_test_and_set(&base);
    uassert_true(base == 0x1);
    uassert_true(result == 0x0);
    /* 原标志已为 1：保持 1，并返回旧值 1。 */
    base = 0x1;
    result = rt_atomic_flag_test_and_set(&base);
    uassert_true(base == 0x1);
    uassert_true(result == 0x1);

    /* rt_atomic_flag_clear */
    base = 0x1;
    rt_atomic_flag_clear(&base);
    uassert_true(base == 0x0);

    /* rt_atomic_load */
    base = 0xFF00;
    result = rt_atomic_load(&base);
    uassert_true(base == 0xFF00);
    uassert_true(result == 0xFF00);

    /* rt_atomic_store */
    base = 0xFF00;
    rt_atomic_store(&base, 0x0F0F);
    uassert_true(base == 0x0F0F);

    /* expected 与 base 相等时，用 desired=11 替换并返回成功。 */
    base = 10;
    oldval = 10;
    result = rt_atomic_compare_exchange_strong(&base, &oldval, 11);
    uassert_true(base == 11);
    uassert_true(result == 0x1);
    /* expected 不匹配时不能写入 desired，并返回失败。 */
    base = 10;
    oldval = 5;
    result = rt_atomic_compare_exchange_strong(&base, &oldval, 11);
    uassert_true(base == 10);
    uassert_true(result == 0x0);
}

/**
 * @brief 原子累加压力工作线程入口。
 *
 * 三个实例无锁并发执行相同的一百万次 add。parameter 未使用；完成后释放一次 sem_t，
 * 这是主测试判断“本线程所有写入均已结束”的同步点。
 */
static void ture_entry(void *parameter)
{
    int i;
    for (i = 0; i < 1000000; i++)
    {
        rt_atomic_add(&count, 1);
    }
    rt_sem_release(sem_t);
}

/**
 * @brief 验证三个竞争线程的原子累加不会丢失更新。
 *
 * 测试先创建零计数完成信号量并清 count，再依次创建/启动三个工作线程。随后连续
 * take 三次，相当于等待全部工作线程完成；屏障之后原子读取 count 并断言 3000000。
 * 动态线程会在退出后由内核回收。该函数没有显式删除 sem_t，这是原测试既有行为。
 */
static void test_atomic_add(void)
{
    rt_thread_t thread;
    size_t i;
    sem_t = rt_sem_create("atomic_sem", 0, RT_IPC_FLAG_PRIO);

    rt_atomic_store(&count, 0);

    thread = rt_thread_create("t1", ture_entry, RT_NULL, THREAD_STACKSIZE, THREAD_PRIORITY, THREAD_TIMESLICE);
    rt_thread_startup(thread);
    thread = rt_thread_create("t2", ture_entry, RT_NULL, THREAD_STACKSIZE, THREAD_PRIORITY, THREAD_TIMESLICE);
    rt_thread_startup(thread);
    thread = rt_thread_create("t3", ture_entry, RT_NULL, THREAD_STACKSIZE, THREAD_PRIORITY, THREAD_TIMESLICE);
    rt_thread_startup(thread);

    for (i = 0; i < 3; i++)
    {
        rt_sem_take(sem_t, RT_WAITING_FOREVER);
    }
    i = rt_atomic_load(&count);
    uassert_true(i == 3000000);
}

static rt_err_t utest_tc_init(void)
{
    /* 本用例没有跨轮次夹具，具体对象由测试函数建立。 */
    return RT_EOK;
}

static rt_err_t utest_tc_cleanup(void)
{
    /* 保留原测试的空清理流程。 */
    return RT_EOK;
}

static void testcase(void)
{
    /* 先验证每个 API 的基础语义，再运行真正并发的累加压力场景。 */
    UTEST_UNIT_RUN(test_atomic_api);
    UTEST_UNIT_RUN(test_atomic_add);
}
UTEST_TC_EXPORT(testcase, "core.atomic", utest_tc_init, utest_tc_cleanup, 10);
