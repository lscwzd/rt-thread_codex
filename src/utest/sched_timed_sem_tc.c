/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-01-25     Shell        init ver.
 * 2025-12-12     lhxj         Add standardized utest documentation block
 */

/**
 * @file sched_timed_sem_tc.c
 * @brief 在一个 tick 的超时边界反复制造“信号量释放与定时器超时”唤醒竞态。
 *
 * producer 每轮先忙等到 tick 刚变化，再加入随机短延迟后 release `_ipc_sem`；consumer
 * 同时用 rt_sem_take_interruptible(..., 1) 只等待一个 tick。于是资源释放和线程
 * 超时回调经常在同一节拍边缘竞争。内核必须保证只有一条路径成功把 consumer 从
 * 等待链移到就绪队列，不能重复入队、破坏链表或返回无关错误。
 *
 * consumer 返回 RT_EOK 或 -RT_ETIMEOUT 都是合法结果；测试只把超时计数用于观察，
 * 任何第三种错误才失败。两个工作线程分别 release `_thr_exit_sem`，主测试 take 两次
 * 作为 join。循环次数按 10 秒的 tick 数设置，但实际墙钟时长会受随机延迟、调度和
 * 平台 tick 精度影响；套件超时留到 20 秒。测试项为 `core.scheduler_timed_sem`。
 */

#define __RT_KERNEL_SOURCE__
#include <rtthread.h>
#include <stdlib.h>
#include "utest.h"

#define TEST_SECONDS 10
#define TEST_LOOP_TICKS (TEST_SECONDS * RT_TICK_PER_SECOND)
#define TEST_PROGRESS_COUNTS (36)
#define TEST_PROGRESS_ON (TEST_LOOP_TICKS*2/TEST_PROGRESS_COUNTS)

/* 完成屏障：producer 和 consumer 各 release 一次。 */
static struct rt_semaphore _thr_exit_sem;
/* 被故意置于 release/timeout 竞争中的资源信号量，初值为 0。 */
static struct rt_semaphore _ipc_sem;
/* 两线程共享的存活进度计数，只通过原子加法更新。 */
static rt_atomic_t _progress_counter;
/* consumer 输给超时路径的次数；超时是本测试允许且希望覆盖的结果。 */
static rt_base_t _timedout_failed_times = 0;

/* producer/consumer 的循环次数相同，但不要求每次迭代一一配对。 */

/**
 * @brief 等待下一次 tick 边沿，再添加一个有界于后续边沿检测的随机忙等延迟。
 *
 * 第一段循环保证返回前至少观察到一次 tick 变化。第二段最多执行 rand() 次读取，但
 * 若又跨过一个 tick 会提前停止，目的是把 release 分散在节拍区间不同位置。忙等不
 * 产生同步保证，只负责扩大竞态时序覆盖；系统 tick 必须正常运行，否则会卡住。
 */
static void _wait_until_edge(void)
{
    rt_tick_t entry_level, current;
    rt_base_t random_latency;

    entry_level = rt_tick_get();
    do
    {
        current = rt_tick_get();
    }
    while (current == entry_level);

    /* 随机扰动 release 相对于 timeout ISR 的先后位置。 */
    random_latency = rand();
    entry_level = current;
    for (size_t i = 0; i < random_latency; i++)
    {
        current = rt_tick_get();
        if (current != entry_level)
            break;
    }
}

/**
 * @brief 生产者线程：每个 tick 边沿附近释放一个资源，并上报进度与完成事件。
 *
 * 与 consumer 同优先级但时间片为 4。每轮 release 可能直接唤醒等待者，也可能在
 * consumer 已超时后增加信号量值；两种情况都合法。
 */
static void _producer_entry(void *param)
{
    for (size_t i = 0; i < TEST_LOOP_TICKS; i++)
    {
        _wait_until_edge();

        rt_sem_release(&_ipc_sem);

        if (rt_atomic_add(&_progress_counter, 1) % TEST_PROGRESS_ON == 0)
            uassert_true(1);
    }

    rt_sem_release(&_thr_exit_sem);
    return;
}

/**
 * @brief 消费者线程：反复执行一个 tick 的可中断信号量等待。
 *
 * RT_EOK 表示 release 赢得竞态，-RT_ETIMEOUT 表示定时器赢得竞态；其他状态说明
 * 调度/IPC 状态机出现非预期结果。完成后与 producer 一样释放 join 信号量。
 */
static void _consumer_entry(void *param)
{
    int error;
    for (size_t i = 0; i < TEST_LOOP_TICKS; i++)
    {
        error = rt_sem_take_interruptible(&_ipc_sem, 1);
        if (error == -RT_ETIMEOUT)
        {
            _timedout_failed_times++;
        }
        else
        {
            if (error != RT_EOK)
                uassert_true(0);
        }

        if (rt_atomic_add(&_progress_counter, 1) % TEST_PROGRESS_ON == 0)
            uassert_true(1);
    }

    rt_sem_release(&_thr_exit_sem);
    return;
}

/**
 * @brief 创建并启动竞争线程，等待二者全部结束并打印超时统计。
 *
 * 主线程不依据超时次数判定通过，因为该比例高度依赖 CPU 性能、tick ISR 延迟和
 * 随机序列；真正通过条件是没有异常返回、断言、死锁或超时。
 */
static void timed_sem_tc(void)
{
    rt_thread_t prod = rt_thread_create(
        "prod",
        _producer_entry,
        (void *)0,
        UTEST_THR_STACK_SIZE,
        UTEST_THR_PRIORITY + 1,
        4);

    rt_thread_t cons = rt_thread_create(
        "cons",
        _consumer_entry,
        (void *)0,
        UTEST_THR_STACK_SIZE,
        UTEST_THR_PRIORITY + 1,
        100);

    rt_thread_startup(prod);
    rt_thread_startup(cons);

    for (size_t i = 0; i < 2; i++)
    {
        rt_sem_take(&_thr_exit_sem, RT_WAITING_FOREVER);
    }

    /* 仅供观察竞态覆盖程度，不是稳定的性能基准或通过阈值。 */
    LOG_I("Total failed times: %ld(in %d)\n", _timedout_failed_times, TEST_LOOP_TICKS);
}

static rt_err_t utest_tc_init(void)
{
    /* 用一块新分配内存中的现有比特扰动 rand 序列；它不用于安全随机。 */
    int *pseed = rt_malloc(sizeof(int));
    srand(*(int *)pseed);
    rt_free(pseed);

    rt_sem_init(&_ipc_sem, "ipc", 0, RT_IPC_FLAG_PRIO);
    rt_sem_init(&_thr_exit_sem, "test", 0, RT_IPC_FLAG_PRIO);
    return RT_EOK;
}

static rt_err_t utest_tc_cleanup(void)
{
    /* join 完成后没有等待者，可安全注销两个静态信号量。 */
    rt_sem_detach(&_ipc_sem);
    rt_sem_detach(&_thr_exit_sem);
    return RT_EOK;
}

static void testcase(void)
{
    /* 单个压力单元内部已经包含创建、同步和统计全过程。 */
    UTEST_UNIT_RUN(timed_sem_tc);
}
UTEST_TC_EXPORT(testcase, "core.scheduler_timed_sem", utest_tc_init, utest_tc_cleanup, TEST_SECONDS * 2);
    /* 两个信号量都以优先级顺序管理等待线程，并从 0 开始。 */
