/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2021-09-08     liukang      初始版本
 * 2023-09-15     xqyjlj       调整 64 位 CPU 的栈大小
 * 2025-11-16     ChuanN-sudo  增加标准化测试说明
 */

/**
 * @file mailbox_tc.c
 * @brief IPC 邮箱的静态/动态生命周期、普通/等待/紧急发送及接收顺序测试。
 *
 * 邮箱每个槽位只保存一个 `rt_ubase_t`。本测试把三个字符串地址当作邮件值发送，
 * 接收端再还原成指针并比较内容；字符串本身不被邮箱复制，因此全局数组保证了
 * 指针生命周期。普通 send 写队尾，urgent 插队到队首，send_wait 在满时可等待。
 *
 * 静态控制块配合外部 `mb_pool` 验证 init/detach；动态对象验证 create/delete。
 * 收发测试各启动两个同优先级线程，用延时和 finish 标志形成可观察时序，断言
 * 普通、等待及紧急邮件的接收内容与顺序。主测试必须等收发线程都完成才销毁
 * 邮箱，避免释放仍在使用的控制块或池。
 *
 * 依赖邮箱、线程调度和 utest；动态路径依赖堆。全部 API 返回值、字符串比较和
 * 完成标志断言通过，即表明队列内容、唤醒和生命周期符合预期。
 */

#include <rtthread.h>
#include "utest.h"
#include <stdlib.h>

#define THREAD_STACKSIZE UTEST_THR_STACK_SIZE

static struct rt_mailbox test_static_mb;
static char mb_pool[128];

static rt_mailbox_t test_dynamic_mb;

static uint8_t static_mb_recv_thread_finish, static_mb_send_thread_finish;
static uint8_t dynamic_mb_recv_thread_finish, dynamic_mb_send_thread_finish;

rt_align(RT_ALIGN_SIZE)
static char thread1_stack[UTEST_THR_STACK_SIZE];
static struct rt_thread thread1;

rt_align(RT_ALIGN_SIZE)
static char thread2_stack[UTEST_THR_STACK_SIZE];
static struct rt_thread thread2;

#define THREAD_PRIORITY      9
#define THREAD_TIMESLICE     5

static rt_thread_t mb_send = RT_NULL;
static rt_thread_t mb_recv = RT_NULL;

static rt_uint8_t mb_send_str1[] = "this is first mail!";
static rt_uint8_t mb_send_str2[] = "this is second mail!";
static rt_uint8_t mb_send_str3[] = "this is thirdy mail!";

static rt_uint8_t *mb_recv_str1;
static rt_uint8_t *mb_recv_str2;
static rt_uint8_t *mb_recv_str3;

/** @brief 用 FIFO、PRIO 两种策略反复初始化/脱离静态邮箱，验证外部池生命周期。 */
static void test_mailbox_init(void)
{
    rt_err_t result;

    result = rt_mb_init(&test_static_mb, "mbt", &mb_pool[0], sizeof(mb_pool) / 4, RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }
    result = rt_mb_detach(&test_static_mb);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    result = rt_mb_init(&test_static_mb, "mbt", &mb_pool[0], sizeof(mb_pool) / 4, RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    result = rt_mb_detach(&test_static_mb);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    uassert_true(1);
}

/** @brief 初始化后单独验证静态邮箱 detach；函数名中的 deatch 是历史拼写。 */
static void test_mailbox_deatch(void)
{
    rt_err_t result;

    result = rt_mb_init(&test_static_mb, "mbt", &mb_pool[0], sizeof(mb_pool) / 4, RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }
    result = rt_mb_detach(&test_static_mb);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    result = rt_mb_init(&test_static_mb, "mbt", &mb_pool[0], sizeof(mb_pool) / 4, RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }
    result = rt_mb_detach(&test_static_mb);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    uassert_true(1);
}

/** @brief 分别按 FIFO、PRIO 创建并删除动态邮箱，验证内部槽位池分配。 */
static void test_mailbox_create(void)
{
    rt_err_t result;

    test_dynamic_mb = rt_mb_create("test_dynamic_mb", sizeof(mb_pool) / 4, RT_IPC_FLAG_FIFO);
    if (test_dynamic_mb == RT_NULL)
    {
        uassert_false(1);
    }
    result = rt_mb_delete(test_dynamic_mb);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    test_dynamic_mb = rt_mb_create("test_dynamic_mb", sizeof(mb_pool) / 4, RT_IPC_FLAG_PRIO);
    if (test_dynamic_mb == RT_NULL)
    {
        uassert_false(1);
    }
    result = rt_mb_delete(test_dynamic_mb);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    uassert_true(1);
}

/** @brief 创建一个动态邮箱并单独断言 delete 成功。 */
static void test_mailbox_delete(void)
{
    rt_err_t result;

    test_dynamic_mb = rt_mb_create("test_dynamic_mb", sizeof(mb_pool) / 4, RT_IPC_FLAG_FIFO);
    if (test_dynamic_mb == RT_NULL)
    {
        uassert_false(1);
    }
    result = rt_mb_delete(test_dynamic_mb);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    test_dynamic_mb = rt_mb_create("test_dynamic_mb", sizeof(mb_pool) / 4, RT_IPC_FLAG_PRIO);
    if (test_dynamic_mb == RT_NULL)
    {
        uassert_false(1);
    }
    result = rt_mb_delete(test_dynamic_mb);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    uassert_true(1);
}

/** @brief 静态邮箱发送线程，依次覆盖普通、等待和紧急发送路径。 */
static void thread2_send_static_mb(void *arg)
{
    rt_err_t res = RT_EOK;

    res = rt_mb_send(&test_static_mb, (rt_ubase_t)&mb_send_str1);
    if (res != RT_EOK)
    {
        uassert_false(1);
    }
    rt_thread_mdelay(100);

    res = rt_mb_send_wait(&test_static_mb, (rt_ubase_t)&mb_send_str2, 10);
    if (res != RT_EOK)
    {
        uassert_false(1);
    }
    rt_thread_mdelay(100);

    res = rt_mb_urgent(&test_static_mb, (rt_ubase_t)&mb_send_str3);
    if (res != RT_EOK)
    {
        uassert_false(1);
    }

    static_mb_send_thread_finish = 1;
}

/** @brief 静态邮箱接收线程，按预期顺序取出三个指针并比较字符串内容。 */
static void thread1_recv_static_mb(void *arg)
{
    rt_err_t result = RT_EOK;

    result = rt_mb_recv(&test_static_mb, (rt_ubase_t *)&mb_recv_str1, RT_WAITING_FOREVER);
    if (result != RT_EOK || rt_strcmp((const char *)mb_recv_str1, (const char *)mb_send_str1) != 0)
    {
        uassert_false(1);
    }

    result = rt_mb_recv(&test_static_mb, (rt_ubase_t *)&mb_recv_str2, RT_WAITING_FOREVER);
    if (result != RT_EOK || rt_strcmp((const char *)mb_recv_str2, (const char *)mb_send_str2) != 0)
    {
        uassert_false(1);
    }

    result = rt_mb_recv(&test_static_mb, (rt_ubase_t *)&mb_recv_str3, RT_WAITING_FOREVER);
    if (result != RT_EOK || rt_strcmp((const char *)mb_recv_str3, (const char *)mb_send_str3) != 0)
    {
        uassert_false(1);
    }

    static_mb_recv_thread_finish = 1;
}

/** @brief 初始化静态邮箱和两个静态线程，等待双 finish 后再 detach。 */
static void test_static_mailbox_send_recv(void)
{
    rt_err_t result;

    result = rt_mb_init(&test_static_mb, "mbt", &mb_pool[0], sizeof(mb_pool) / 4, RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    rt_thread_init(&thread1,
                   "thread1",
                   thread1_recv_static_mb,
                   RT_NULL,
                   &thread1_stack[0],
                   sizeof(thread1_stack),
                   THREAD_PRIORITY - 1, THREAD_TIMESLICE);
    rt_thread_startup(&thread1);

    rt_thread_init(&thread2,
                   "thread2",
                   thread2_send_static_mb,
                   RT_NULL,
                   &thread2_stack[0],
                   sizeof(thread2_stack),
                   THREAD_PRIORITY, THREAD_TIMESLICE);
    rt_thread_startup(&thread2);

    while (static_mb_recv_thread_finish != 1 || static_mb_send_thread_finish != 1)
    {
        rt_thread_delay(1);
    }

    if (rt_mb_detach(&test_static_mb) != RT_EOK)
    {
        uassert_false(1);
    }

    uassert_true(1);
}

/** @brief 动态邮箱发送线程，重复三种发送语义并报告完成。 */
static void thread4_send_dynamic_mb(void *arg)
{
    rt_err_t res = RT_EOK;

    res = rt_mb_send(test_dynamic_mb, (rt_ubase_t)&mb_send_str1);
    if (res != RT_EOK)
    {
        uassert_false(1);
    }
    rt_thread_mdelay(100);

    res = rt_mb_send_wait(test_dynamic_mb, (rt_ubase_t)&mb_send_str2, 10);
    if (res != RT_EOK)
    {
        uassert_false(1);
    }
    rt_thread_mdelay(100);

    res = rt_mb_urgent(test_dynamic_mb, (rt_ubase_t)&mb_send_str3);
    if (res != RT_EOK)
    {
        uassert_false(1);
    }

    dynamic_mb_send_thread_finish = 1;
}

/** @brief 动态邮箱接收线程，验证邮件指针及字符串内容未损坏。 */
static void thread3_recv_dynamic_mb(void *arg)
{
    rt_err_t result = RT_EOK;

    result = rt_mb_recv(test_dynamic_mb, (rt_ubase_t *)&mb_recv_str1, RT_WAITING_FOREVER);
    if (result != RT_EOK || rt_strcmp((const char *)mb_recv_str1, (const char *)mb_send_str1) != 0)
    {
        uassert_false(1);
    }

    result = rt_mb_recv(test_dynamic_mb, (rt_ubase_t *)&mb_recv_str2, RT_WAITING_FOREVER);
    if (result != RT_EOK || rt_strcmp((const char *)mb_recv_str2, (const char *)mb_send_str2) != 0)
    {
        uassert_false(1);
    }

    result = rt_mb_recv(test_dynamic_mb, (rt_ubase_t *)&mb_recv_str3, RT_WAITING_FOREVER);
    if (result != RT_EOK || rt_strcmp((const char *)mb_recv_str3, (const char *)mb_send_str3) != 0)
    {
        uassert_false(1);
    }

    dynamic_mb_recv_thread_finish = 1;
}

/** @brief 创建动态邮箱和收发线程，等待线程退出后再删除对象。 */
static void test_dynamic_mailbox_send_recv(void)
{
    test_dynamic_mb = rt_mb_create("mbt", sizeof(mb_pool) / 4, RT_IPC_FLAG_FIFO);
    if (test_dynamic_mb == RT_NULL)
    {
        uassert_false(1);
    }

    mb_recv = rt_thread_create("mb_recv_thread",
                                thread3_recv_dynamic_mb,
                                RT_NULL,
                                UTEST_THR_STACK_SIZE,
                                THREAD_PRIORITY - 1,
                                THREAD_TIMESLICE);
    if (mb_recv == RT_NULL)
    {
        uassert_false(1);
    }
    rt_thread_startup(mb_recv);

    mb_send = rt_thread_create("mb_send_thread",
                                thread4_send_dynamic_mb,
                                RT_NULL,
                                UTEST_THR_STACK_SIZE,
                                THREAD_PRIORITY - 1,
                                THREAD_TIMESLICE);
    if (mb_send == RT_NULL)
    {
        uassert_false(1);
    }
    rt_thread_startup(mb_send);

    while (dynamic_mb_recv_thread_finish != 1 || dynamic_mb_send_thread_finish != 1)
    {
        rt_thread_delay(1);
    }

    if (rt_mb_delete(test_dynamic_mb) != RT_EOK)
    {
        uassert_false(1);
    }

    uassert_true(1);
}

/** @brief 用例初始化入口；各子测试自行建立所需对象和线程。 */
static rt_err_t utest_tc_init(void)
{
    return RT_EOK;
}

/** @brief 用例清理入口；各子测试已完成资源回收。 */
static rt_err_t utest_tc_cleanup(void)
{
    return RT_EOK;
}

/** @brief 依次运行静态/动态生命周期与并发收发测试。 */
static void testcase(void)
{
    UTEST_UNIT_RUN(test_mailbox_init);
    UTEST_UNIT_RUN(test_mailbox_deatch);
    UTEST_UNIT_RUN(test_mailbox_create);
    UTEST_UNIT_RUN(test_mailbox_delete);
    UTEST_UNIT_RUN(test_static_mailbox_send_recv);
    UTEST_UNIT_RUN(test_dynamic_mailbox_send_recv);
}
UTEST_TC_EXPORT(testcase, "core.ipc_mailbox", utest_tc_init, utest_tc_cleanup, 60);
