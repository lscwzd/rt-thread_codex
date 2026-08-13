/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2025-09-02     Rbb666       增加 rt_thread_suspend 综合测试
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "utest.h"

/**
 * @file thread_suspend_tc.c
 * @brief 强制挂起/恢复线程的正常行为、边界状态及持锁死锁风险测试。
 *
 * 强制挂起与“线程主动等待 IPC”不同：控制线程可以在目标执行任意代码时停止它。
 * 普通场景通过 `work_counter` 验证挂起期间计数不再变化、resume 后重新增长。
 * API 边界场景检查重复挂起、未启动线程等状态返回。死锁风险场景故意让 owner
 * 持有互斥量时被挂起，再启动 waiter，证明 waiter 无法推进；随后恢复 owner，
 * 让其释放锁，验证系统可以解除风险并完成清理。
 *
 * 全局 volatile 标志只是测试观测点，不代替生产代码同步；线程间真正的完成通知
 * 使用 `sync_sem`。每个测试都必须恢复/删除可能仍运行的线程，并删除信号量和
 * 互斥量，避免把挂起线程留给后续 utest。
 */

#define THREAD_STACK_SIZE    1024
#define THREAD_TIMESLICE     5
#define TEST_THREAD_PRIORITY 25

/* 正常挂起/恢复场景的线程句柄、完成信号量和观测标志。 */
static rt_thread_t          target_thread     = RT_NULL;
static rt_thread_t          monitor_thread    = RT_NULL;
static rt_sem_t             sync_sem          = RT_NULL;
static volatile rt_uint32_t work_counter      = 0;
static volatile rt_bool_t   suspend_test_done = RT_FALSE;
static volatile rt_bool_t   suspend_success   = RT_FALSE;
static volatile rt_bool_t   resume_success    = RT_FALSE;

/* “持锁 owner 被强制挂起”风险场景的对象和时序标志。 */
static rt_mutex_t           test_mutex         = RT_NULL;
static rt_thread_t          holder_thread      = RT_NULL;
static rt_thread_t          waiter_thread      = RT_NULL;
static volatile rt_uint32_t shared_counter     = 0;
static volatile rt_bool_t   holder_got_mutex   = RT_FALSE;
static volatile rt_bool_t   deadlock_detected  = RT_FALSE;
static volatile rt_bool_t   test_completed     = RT_FALSE;
static volatile rt_bool_t   thread_started     = RT_FALSE;
static volatile rt_bool_t   thread_should_exit = RT_FALSE;

/** @brief 被控制的目标线程：每 10 ms 增加计数，供监控线程判断是否真正停住。 */
static void target_work_thread(void *parameter)
{
    while (1)
    {
        if (!suspend_test_done)
        {
            work_counter++;
        }
        /* 主动延时模拟正常工作，也让监控线程有稳定调度机会。 */
        rt_thread_mdelay(10);
    }
}

/** @brief 控制线程：采样计数、挂起目标、验证静止、恢复并通知主测试完成。 */
static void monitor_control_thread(void *parameter)
{
    rt_uint32_t counter_before, counter_after;

    /* 先给目标线程足够时间进入稳定计数循环。 */
    rt_thread_mdelay(300);

    /* 记录挂起前基准值。 */
    counter_before = work_counter;

    /* 强制把另一个线程从可运行状态转为挂起。 */
    if (rt_thread_suspend(target_thread) == RT_EOK)
    {
        suspend_success = RT_TRUE;

        /* 主动调度，确保状态变化已经在执行顺序上生效。 */
        rt_schedule();

        /* 观察窗口内目标不应再得到 CPU。 */
        rt_thread_mdelay(500);

        counter_after = work_counter;

        /* 计数完全不变才证明挂起成功。 */
        if (counter_after == counter_before)
        {
            /* 恢复目标并检查 API 返回。 */
            if (rt_thread_resume(target_thread) == RT_EOK)
            {
                resume_success = RT_TRUE;
                /* 留出恢复运行窗口。 */
                rt_thread_mdelay(200);
            }
        }
    }

    /* 阻止目标继续更新测试观测值。 */
    suspend_test_done = RT_TRUE;

    /* 用真正 IPC 通知主测试控制线程已完成全部检查。 */
    rt_sem_release(sync_sem);

    /* Keep running until deleted */
    while (1)
    {
        rt_thread_mdelay(100);
    }
}

/* Thread that holds the mutex */
static void mutex_holder_thread(void *parameter)
{
    if (rt_mutex_take(test_mutex, RT_WAITING_FOREVER) == RT_EOK)
    {
        holder_got_mutex = RT_TRUE;

        /* Simulate critical section work */
        for (int i = 0; i < 1000 && !test_completed; i++)
        {
            shared_counter++;
            if (i % 200 == 0)
            {
                rt_thread_mdelay(10);
            }
        }

        if (!test_completed)
        {
            rt_mutex_release(test_mutex);
        }
    }
    rt_kprintf("Holder thread exiting\n");

    /* Keep running until deleted */
    while (1)
    {
        rt_thread_mdelay(100);
    }
}

/* Thread that waits for the mutex */
static void mutex_waiter_thread(void *parameter)
{
    /* Wait a bit to ensure holder gets mutex first */
    rt_thread_mdelay(50);

    rt_err_t result = rt_mutex_take(test_mutex, rt_tick_from_millisecond(1500));
    if (result == RT_EOK)
    {
        shared_counter += 1000;
        rt_mutex_release(test_mutex);
    }
    else
    {
        /* Timeout indicates deadlock - holder is suspended and cannot release lock */
        deadlock_detected = RT_TRUE;
        rt_kprintf("Deadlock detected: waiter timeout (holder suspended with mutex)\n");
    }

    /* Keep running until deleted */
    while (1)
    {
        rt_thread_mdelay(100);
    }
}

void simple_thread_entry(void *param)
{
    volatile rt_bool_t *flag = (volatile rt_bool_t *)param;
    *flag                    = RT_TRUE;

    /* Keep the thread running until it's suspended and deleted */
    while (1)
    {
        rt_thread_mdelay(100);
    }
}

/* Test normal usage of rt_thread_suspend function */
static void test_suspend_force_normal_usage(void)
{
    /* Reset global variables */
    work_counter      = 0;
    suspend_test_done = RT_FALSE;
    suspend_success   = RT_FALSE;
    resume_success    = RT_FALSE;

    /* Create synchronization semaphore */
    sync_sem = rt_sem_create("sync", 0, RT_IPC_FLAG_FIFO);
    uassert_not_null(sync_sem);

    /* Create target work thread */
    target_thread = rt_thread_create("target",
                                     target_work_thread,
                                     RT_NULL,
                                     THREAD_STACK_SIZE,
                                     TEST_THREAD_PRIORITY,
                                     THREAD_TIMESLICE);

    uassert_not_null(target_thread);

    /* Create monitor thread */
    monitor_thread = rt_thread_create("monitor",
                                      monitor_control_thread,
                                      RT_NULL,
                                      THREAD_STACK_SIZE,
                                      UTEST_THR_PRIORITY,
                                      THREAD_TIMESLICE);

    uassert_not_null(monitor_thread);

    /* Start threads */
    rt_thread_startup(target_thread);
    rt_thread_startup(monitor_thread);

    /* Wait for test completion */
    rt_sem_take(sync_sem, RT_WAITING_FOREVER);

    /* Wait for a while to ensure threads exit normally */
    rt_thread_mdelay(100);

    /* Verify test results */
    uassert_true(suspend_success);
    uassert_true(resume_success);
    uassert_true(work_counter > 0);

    /* Clean up resources */
    if (sync_sem)
    {
        rt_sem_delete(sync_sem);
        sync_sem = RT_NULL;
    }

    /* Delete threads */
    if (target_thread != RT_NULL)
    {
        rt_thread_delete(target_thread);
        target_thread = RT_NULL;
    }

    if (monitor_thread != RT_NULL)
    {
        rt_thread_delete(monitor_thread);
        monitor_thread = RT_NULL;
    }
}

/* Basic API test */
static void test_suspend_force_api_basic(void)
{
    rt_thread_t api_thread;

    /* Reset global variables */
    thread_started     = RT_FALSE;
    thread_should_exit = RT_FALSE;

    /* Create a simple test thread */
    api_thread = rt_thread_create("api_test",
                                  simple_thread_entry,
                                  (void *)&thread_started,
                                  THREAD_STACK_SIZE,
                                  UTEST_THR_PRIORITY,
                                  THREAD_TIMESLICE);

    uassert_not_null(api_thread);

    rt_thread_startup(api_thread);
    rt_thread_mdelay(50); /* Wait for thread to start */

    uassert_true(thread_started);

    /* Test basic suspend functionality */
    rt_err_t result = rt_thread_suspend(api_thread);
    uassert_true(result == RT_EOK);

    rt_schedule();
    rt_thread_mdelay(100);

    /* Resume thread */
    result = rt_thread_resume(api_thread);
    uassert_true(result == RT_EOK);

    rt_thread_mdelay(50);

    /* Clean up - delete the thread directly */
    if (api_thread != RT_NULL)
    {
        rt_thread_delete(api_thread);
        api_thread = RT_NULL;
    }

    /* Reset global variables for next test */
    thread_started     = RT_FALSE;
    thread_should_exit = RT_FALSE;
}

/* Test suspend on thread that is created but not started */
static void test_suspend_force_not_started_thread(void)
{
    rt_thread_t not_started_thread;

    /* Create a thread but don't start it */
    not_started_thread = rt_thread_create("not_started",
                                          simple_thread_entry,
                                          (void *)&thread_started,
                                          THREAD_STACK_SIZE,
                                          UTEST_THR_PRIORITY,
                                          THREAD_TIMESLICE);

    uassert_not_null(not_started_thread);

    /* Verify thread is in INIT state */
    uassert_true((RT_SCHED_CTX(not_started_thread).stat & RT_THREAD_STAT_MASK) == RT_THREAD_INIT);

    /* Try to suspend a thread that hasn't been started yet */
    rt_err_t suspend_result = rt_thread_suspend(not_started_thread);
    rt_schedule();
    uassert_true(suspend_result == -RT_ERROR);

    /* Try to resume the not-started thread */
    rt_err_t resume_result = rt_thread_resume(not_started_thread);
    uassert_true(resume_result == -RT_EINVAL);

    /* Now start the thread to see if it works normally */
    rt_err_t startup_result = rt_thread_startup(not_started_thread);
    uassert_true(startup_result == RT_EOK);

    /* Wait a bit to see if thread starts normally */
    rt_thread_mdelay(100);

    /* The thread should have started successfully despite previous suspend/resume calls */
    uassert_true(thread_started == RT_TRUE);

    /* Clean up */
    if (not_started_thread != RT_NULL)
    {
        rt_thread_delete(not_started_thread);
        not_started_thread = RT_NULL;
    }

    /* Reset flag for next test */
    thread_started = RT_FALSE;
}

/* Test deadlock risk */
static void test_suspend_force_deadlock_risk(void)
{
    /* Reset global variables */
    shared_counter    = 0;
    holder_got_mutex  = RT_FALSE;
    deadlock_detected = RT_FALSE;
    test_completed    = RT_FALSE;

    /* Create mutex */
    test_mutex = rt_mutex_create("test_mutex", RT_IPC_FLAG_PRIO);
    uassert_not_null(test_mutex);

    /* Create and start holder thread */
    holder_thread = rt_thread_create("holder", mutex_holder_thread, RT_NULL,
                                     THREAD_STACK_SIZE, UTEST_THR_PRIORITY, THREAD_TIMESLICE);
    uassert_not_null(holder_thread);
    rt_thread_startup(holder_thread);

    /* Create and start waiter thread */
    waiter_thread = rt_thread_create("waiter", mutex_waiter_thread, RT_NULL,
                                     THREAD_STACK_SIZE, UTEST_THR_PRIORITY + 1, THREAD_TIMESLICE);
    uassert_not_null(waiter_thread);
    /* Now start waiter thread, it will try to acquire mutex held by suspended thread */
    rt_thread_startup(waiter_thread);

    /* Wait for holder to get mutex */
    int timeout = 100; /* 1 second timeout */
    while (!holder_got_mutex && timeout-- > 0)
    {
        rt_thread_mdelay(10);
    }

    uassert_true(holder_got_mutex);

    /* This is the critical test! Suspend thread that holds the mutex */
    rt_err_t suspend_result = rt_thread_suspend(holder_thread);
    uassert_true(suspend_result == RT_EOK);
    rt_kprintf("Suspended holder thread (which holds the mutex)\n");

    rt_schedule();

    /* Wait for waiter thread to try acquiring lock */
    rt_thread_mdelay(2000);

    uassert_true(deadlock_detected == RT_TRUE);

    /* Resume thread */
    rt_err_t resume_result = rt_thread_resume(holder_thread);
    uassert_true(resume_result == RT_EOK);
    rt_kprintf("Resumed holder thread\n");

    test_completed = RT_TRUE;

    /* Wait for threads to complete */
    rt_thread_mdelay(1000);

    /* Verify rt_thread_suspend and rt_thread_resume executed successfully */
    uassert_true(suspend_result == RT_EOK);
    uassert_true(resume_result == RT_EOK);

    /* Verify system didn't crash, threads can work normally */
    uassert_true(shared_counter > 0);

    /* Clean up resources */
    if (test_mutex)
    {
        rt_mutex_delete(test_mutex);
        test_mutex = RT_NULL;
    }

    /* Delete threads */
    if (holder_thread != RT_NULL)
    {
        rt_thread_delete(holder_thread);
        holder_thread = RT_NULL;
    }

    if (waiter_thread != RT_NULL)
    {
        rt_thread_delete(waiter_thread);
        waiter_thread = RT_NULL;
    }

    /* Wait again to ensure threads are cleaned up */
    rt_thread_mdelay(200);
}

static rt_err_t utest_tc_init(void)
{
    return RT_EOK;
}

static rt_err_t utest_tc_cleanup(void)
{
    /* Reset all global variables to ensure clean state between tests */
    work_counter       = 0;
    suspend_test_done  = RT_FALSE;
    suspend_success    = RT_FALSE;
    resume_success     = RT_FALSE;
    shared_counter     = 0;
    holder_got_mutex   = RT_FALSE;
    deadlock_detected  = RT_FALSE;
    test_completed     = RT_FALSE;
    thread_started     = RT_FALSE;
    thread_should_exit = RT_FALSE;

    /* Clean up any remaining resources - safety check */
    if (sync_sem != RT_NULL)
    {
        rt_sem_delete(sync_sem);
        sync_sem = RT_NULL;
    }

    if (test_mutex != RT_NULL)
    {
        rt_mutex_delete(test_mutex);
        test_mutex = RT_NULL;
    }

    /* Ensure all thread pointers are NULL */
    target_thread  = RT_NULL;
    monitor_thread = RT_NULL;
    holder_thread  = RT_NULL;
    waiter_thread  = RT_NULL;

    /* Give system time to complete cleanup */
    rt_thread_mdelay(50);

    return RT_EOK;
}

static void testcase(void)
{
    UTEST_UNIT_RUN(test_suspend_force_api_basic);
    UTEST_UNIT_RUN(test_suspend_force_not_started_thread);
    UTEST_UNIT_RUN(test_suspend_force_normal_usage);
    UTEST_UNIT_RUN(test_suspend_force_deadlock_risk);
}
UTEST_TC_EXPORT(testcase, "core.thread_suspend", utest_tc_init, utest_tc_cleanup, 30);
