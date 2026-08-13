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
 * @file sched_thread_tc.c
 * @brief 通过成对线程的十万次挂起/恢复“乒乓”压力测试调度状态转换。
 *
 * 每个 CPU 配置一对同优先级线程。A 每轮进入禁止调度临界区、挂起自己、恢复 B，
 * 再退出临界区；B 对 A 做完全对称的操作。关键点是“当前线程已标记挂起”和“伙伴
 * 已恢复”之间不能发生破坏状态的普通调度，真正切换应在最外层临界区退出后进行。
 * SMP 下多对线程还可能在不同 CPU 并行，扩大就绪队列和状态锁的竞争覆盖。
 *
 * `_progress_counter` 只生成周期性存活断言，不校验最终精确值；真正失败信号是死锁、
 * 崩溃或用例超时。每个工作线程结束会 release 完成信号量，但原代码主线程只 take
 * `TEST_THREAD_COUNT` 次，而创建了两倍数量的线程，因此这并不是严格等待所有线程的
 * 完整 join；注释如实保留这一现有测试边界。测试项为 `core.scheduler_thread`。
 */

#define __RT_KERNEL_SOURCE__
#include <rtthread.h>
#include "utest.h"

#define TEST_LOOP_TIMES (100 * 1000)
#define TEST_PROGRESS_COUNTS (36)
#define TEST_THREAD_COUNT (RT_CPUS_NR * 1)
#define TEST_PROGRESS_ON (TEST_LOOP_TIMES*TEST_THREAD_COUNT/TEST_PROGRESS_COUNTS)

/* 工作线程结束通知；每个入口退出前 release 一次。 */
static struct rt_semaphore _thr_exit_sem;
/* 所有线程共享的无锁进度计数，仅用于定期报告测试仍在推进。 */
static rt_atomic_t _progress_counter;

/* 第二维 0/1 分别保存一对互相唤醒的线程句柄；启动前全部填写完成。 */
static volatile rt_thread_t threads_group[TEST_THREAD_COUNT][2];

/**
 * @brief 每对中的 A 线程入口，反复把执行权交给同组 B。
 *
 * param 被转换为线程对下标。首次运行时 B 可能仍为 READY，resume 可能没有实际状态
 * 转换；A 自身挂起并在退出临界区后切走。以后 B 恢复 A，形成稳定乒乓。完成十万轮
 * 后释放一次完成信号量。
 */
static void _thread_entry1(void *param)
{
    rt_base_t critical_level;
    size_t idx = (size_t)param;

    for (size_t i = 0; i < TEST_LOOP_TIMES; i++)
    {
        /* 保护“挂起自己 + 恢复伙伴”成为不可被普通调度拆开的状态事务。 */
        critical_level = rt_enter_critical();

        rt_thread_suspend(rt_thread_self());
        rt_thread_resume(threads_group[idx][1]);

        rt_exit_critical_safe(critical_level);

        /* 原子计数避免 SMP 下多个工作线程互相覆盖进度。 */
        if (rt_atomic_add(&_progress_counter, 1) % TEST_PROGRESS_ON == 0)
            uassert_true(1);
    }

    rt_sem_release(&_thr_exit_sem);
    return;
}

/**
 * @brief 每对中的 B 线程入口，执行与 A 对称的 B -> A 交接。
 *
 * 使用同一 pair 下标找到 threads_group[idx][0]；临界区配对和完成通知语义与 A 相同。
 */
static void _thread_entry2(void *param)
{
    rt_base_t critical_level;
    size_t idx = (size_t)param;

    for (size_t i = 0; i < TEST_LOOP_TIMES; i++)
    {
        critical_level = rt_enter_critical();

        rt_thread_suspend(rt_thread_self());
        rt_thread_resume(threads_group[idx][0]);

        rt_exit_critical_safe(critical_level);

        if (rt_atomic_add(&_progress_counter, 1) % TEST_PROGRESS_ON == 0)
            uassert_true(1);
    }

    rt_sem_release(&_thr_exit_sem);
    return;
}

/**
 * @brief 创建全部线程对、发布句柄、统一启动并等待既有数量的完成通知。
 *
 * 先创建所有对象并填满 threads_group，随后才启动，保证工作线程读取伙伴指针时不会
 * 看到未初始化槽。所有线程优先级和时间片相同，使交接主要由显式 suspend/resume
 * 驱动。末尾循环按原逻辑只消费每对一个完成计数，剩余通知可能留在信号量中。
 */
static void scheduler_tc(void)
{
    for (size_t i = 0; i < TEST_THREAD_COUNT; i++)
    {
        rt_thread_t t1 =
            rt_thread_create(
                "t1",
                _thread_entry1,
                (void *)i,
                UTEST_THR_STACK_SIZE,
                UTEST_THR_PRIORITY + 1,
                100);
        rt_thread_t t2 =
            rt_thread_create(
                "t2",
                _thread_entry2,
                (void *)i,
                UTEST_THR_STACK_SIZE,
                UTEST_THR_PRIORITY + 1,
                100);

        threads_group[i][0] = t1;
        threads_group[i][1] = t2;
    }

    for (size_t i = 0; i < TEST_THREAD_COUNT; i++)
    {
        rt_thread_startup(threads_group[i][0]);
        rt_thread_startup(threads_group[i][1]);
    }

    for (size_t i = 0; i < TEST_THREAD_COUNT; i++)
    {
        rt_sem_take(&_thr_exit_sem, RT_WAITING_FOREVER);
    }
}

static rt_err_t utest_tc_init(void)
{
    /* 零计数信号量作为完成通知队列，等待者按优先级排列。 */
    rt_sem_init(&_thr_exit_sem, "test", 0, RT_IPC_FLAG_PRIO);
    return RT_EOK;
}

static rt_err_t utest_tc_cleanup(void)
{
    /* 注销静态完成信号量；原测试未逐个显式删除动态工作线程。 */
    rt_sem_detach(&_thr_exit_sem);
    return RT_EOK;
}

static void testcase(void)
{
    /* 单个压力单元包含全部线程对的生命周期。 */
    UTEST_UNIT_RUN(scheduler_tc);
}
UTEST_TC_EXPORT(testcase, "core.scheduler_thread", utest_tc_init, utest_tc_cleanup, 10);
