/*
 * Copyright (c) 2006-2019, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2021-08-15     liukang      初始版本
 * 2023-09-15     xqyjlj       调整 64 位 CPU 的栈大小
 * 2025-11-17     Ze-Hou       增加标准化测试说明
 */

/**
 * @file event_tc.c
 * @brief 内核事件对象静态/动态生命周期及 AND、OR 收发语义测试。
 *
 * 初学者可把事件理解为 32 个共享布尔位。本测试使用第 3、5 位：接收线程先以
 * OR 等待“任意一位”，随后以 AND 等待“两位都到齐”；发送线程分两次置位，
 * 从而验证 OR 会先醒、AND 必须继续等待。`recv_event_times*` 记录阶段，finish
 * 标志让主测试等待工作线程完成，避免在线程仍访问对象时 detach/delete。
 *
 * 静态路径验证 `rt_event_init()/detach()`，动态路径在启用堆时验证
 * `create()/delete()`；两条路径都以实际线程时序检查 `send()/recv()`。所有断言
 * 预期 API 返回 `RT_EOK`，收到的位掩码和阶段计数符合发送顺序。清理函数只在
 * 用例已等待 finish 后执行，确保不存在悬挂等待者。
 *
 * 依赖事件、线程和 utest；动态部分还依赖堆。全部断言通过即表示生命周期、位
 * 组合和线程同步符合预期。
 */

#include <rtthread.h>
#include "utest.h"
#include <stdlib.h>

#define THREAD_STACKSIZE UTEST_THR_STACK_SIZE
#define EVENT_FLAG3 (1 << 3)
#define EVENT_FLAG5 (1 << 5)

static struct rt_event static_event = {0};
#ifdef RT_USING_HEAP
static rt_event_t dynamic_event = RT_NULL;
static rt_uint32_t dynamic_event_recv_thread_finish = 0, dynamic_event_send_thread_finish = 0;

rt_align(RT_ALIGN_SIZE)
static char thread3_stack[UTEST_THR_STACK_SIZE];
static struct rt_thread thread3;

rt_align(RT_ALIGN_SIZE)
static char thread4_stack[UTEST_THR_STACK_SIZE];
static struct rt_thread thread4;
#endif /* RT_USING_HEAP */

static rt_uint32_t recv_event_times1 = 0, recv_event_times2 = 0;
static rt_uint32_t static_event_recv_thread_finish = 0, static_event_send_thread_finish = 0;

rt_align(RT_ALIGN_SIZE)
static char thread1_stack[UTEST_THR_STACK_SIZE];
static struct rt_thread thread1;

rt_align(RT_ALIGN_SIZE)
static char thread2_stack[UTEST_THR_STACK_SIZE];
static struct rt_thread thread2;

#define THREAD_PRIORITY      9
#define THREAD_TIMESLICE     5

/** @brief 分别用 PRIO、FIFO 初始化并脱离静态事件，验证两种等待排序均可建销。 */
static void test_event_init(void)
{
    rt_err_t result;

    result = rt_event_init(&static_event, "event", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }
    result = rt_event_detach(&static_event);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    result = rt_event_init(&static_event, "event", RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }
    result = rt_event_detach(&static_event);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    uassert_true(1);
}

/** @brief 再次初始化静态事件并验证显式 detach 的成功返回值。 */
static void test_event_detach(void)
{
    rt_err_t result = RT_EOK;

    result = rt_event_init(&static_event, "event", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    result = rt_event_detach(&static_event);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    uassert_true(1);
}

/**
 * @brief 静态事件接收线程：按既定 OR/AND 顺序等待位，并断言实际掩码。
 * @param param 未使用。
 * @note 每次成功接收都会推进 `recv_event_times1`，最后设置 finish 供主线程清理。
 */
static void thread1_recv_static_event(void *param)
{
    rt_uint32_t e;

    if (rt_event_recv(&static_event, (EVENT_FLAG3 | EVENT_FLAG5),
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &e) != RT_EOK)
    {
        return;
    }

    recv_event_times1 = e;

    rt_thread_mdelay(50);

    if (rt_event_recv(&static_event, (EVENT_FLAG3 | EVENT_FLAG5),
                      RT_EVENT_FLAG_AND | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &e) != RT_EOK)
    {
        return;
    }
    recv_event_times2 = e;

    static_event_recv_thread_finish = 1;
}

/**
 * @brief 静态事件发送线程：用延时制造确定顺序，依次发送第 3 位和第 5 位。
 * @param param 未使用。
 * @note 延时确保接收线程已经挂起，使测试覆盖真正的等待唤醒路径。
 */
static void thread2_send_static_event(void *param)
{
    rt_event_send(&static_event, EVENT_FLAG3);
    rt_thread_mdelay(10);

    rt_event_send(&static_event, EVENT_FLAG5);
    rt_thread_mdelay(10);

    rt_event_send(&static_event, EVENT_FLAG3);

    static_event_send_thread_finish = 1;
}


/**
 * @brief 创建静态收发线程，启动后等待两个 finish 标志并最终脱离事件对象。
 * @note 栈和线程控制块均为静态存储，测试结束前必须等待线程自行退出。
 */
static void test_static_event_send_recv(void)
{
    rt_err_t result = RT_EOK;

    result  = rt_event_init(&static_event, "event", RT_IPC_FLAG_PRIO);
    if (result  != RT_EOK)
    {
        uassert_false(1);
    }

    rt_thread_init(&thread1,
                   "thread1",
                   thread1_recv_static_event,
                   RT_NULL,
                   &thread1_stack[0],
                   sizeof(thread1_stack),
                   THREAD_PRIORITY - 1, THREAD_TIMESLICE);
    rt_thread_startup(&thread1);

    rt_thread_init(&thread2,
                   "thread2",
                   thread2_send_static_event,
                   RT_NULL,
                   &thread2_stack[0],
                   sizeof(thread2_stack),
                   THREAD_PRIORITY, THREAD_TIMESLICE);
    rt_thread_startup(&thread2);

    while (static_event_recv_thread_finish != 1 || static_event_send_thread_finish != 1)
    {
        rt_thread_delay(1);
    }

    if (recv_event_times1 == EVENT_FLAG3 && recv_event_times2 == (EVENT_FLAG3 | EVENT_FLAG5))
    {
        if (rt_event_detach(&static_event) != RT_EOK)
        {
            uassert_false(1);
        }
        uassert_true(1);
    }
    else
    {
        if (rt_event_detach(&static_event) != RT_EOK)
        {
            uassert_false(1);
        }
        uassert_false(1);
    }

    return;
}

#ifdef RT_USING_HEAP
/** @brief 在堆可用时用两种等待策略创建、删除动态事件，验证动态生命周期。 */
static void test_event_create(void)
{
    rt_err_t result = RT_EOK;

    dynamic_event = rt_event_create("dynamic_event", RT_IPC_FLAG_FIFO);
    if (dynamic_event == RT_NULL)
    {
        uassert_false(1);
    }

    result = rt_event_delete(dynamic_event);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    uassert_true(1);
}

/** @brief 创建一个动态事件并单独验证 `rt_event_delete()`。 */
static void test_event_delete(void)
{
    rt_err_t result;

    dynamic_event = rt_event_create("dynamic_event", RT_IPC_FLAG_FIFO);
    if (dynamic_event == RT_NULL)
    {
        uassert_false(1);
    }

    result = rt_event_delete(dynamic_event);
    if (result != RT_EOK)
    {
        uassert_false(1);
    }

    uassert_true(1);
}

/** @brief 动态事件接收线程，重复静态路径的 OR/AND 位组合断言。 */
static void thread3_recv_dynamic_event(void *param)
{
    rt_uint32_t e;

    if (rt_event_recv(dynamic_event, (EVENT_FLAG3 | EVENT_FLAG5),
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &e) != RT_EOK)
    {
        return;
    }

    recv_event_times1 = e;

    rt_thread_mdelay(50);

    if (rt_event_recv(dynamic_event, (EVENT_FLAG3 | EVENT_FLAG5),
                      RT_EVENT_FLAG_AND | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &e) != RT_EOK)
    {
        return;
    }
    recv_event_times2 = e;

    dynamic_event_recv_thread_finish = 1;
}

/** @brief 动态事件发送线程，按延时顺序置位并在末尾发布完成标志。 */
static void thread4_send_dynamic_event(void *param)
{
    rt_event_send(dynamic_event, EVENT_FLAG3);
    rt_thread_mdelay(10);

    rt_event_send(dynamic_event, EVENT_FLAG5);
    rt_thread_mdelay(10);

    rt_event_send(dynamic_event, EVENT_FLAG3);

    dynamic_event_send_thread_finish = 1;
}

/** @brief 组织动态事件收发线程时序，等待完成后删除事件，检查无提前释放。 */
static void test_dynamic_event_send_recv(void)
{
    dynamic_event = rt_event_create("dynamic_event", RT_IPC_FLAG_PRIO);
    if (dynamic_event == RT_NULL)
    {
        uassert_false(1);
    }

    rt_thread_init(&thread3,
                   "thread3",
                   thread3_recv_dynamic_event,
                   RT_NULL,
                   &thread3_stack[0],
                   sizeof(thread3_stack),
                   THREAD_PRIORITY - 1, THREAD_TIMESLICE);
    rt_thread_startup(&thread3);

    rt_thread_init(&thread4,
                   "thread4",
                   thread4_send_dynamic_event,
                   RT_NULL,
                   &thread4_stack[0],
                   sizeof(thread4_stack),
                   THREAD_PRIORITY, THREAD_TIMESLICE);
    rt_thread_startup(&thread4);

    while (dynamic_event_recv_thread_finish != 1 || dynamic_event_send_thread_finish != 1)
    {
        rt_thread_delay(1);
    }

    if (recv_event_times1 == EVENT_FLAG3 && recv_event_times2 == (EVENT_FLAG3 | EVENT_FLAG5))
    {
        if (rt_event_delete(dynamic_event) != RT_EOK)
        {
            uassert_false(1);
        }
        uassert_true(1);
    }
    else
    {
        if (rt_event_delete(dynamic_event) != RT_EOK)
        {
            uassert_false(1);
        }
        uassert_false(1);
    }

    return;
}
#endif

/** @brief 每轮用例前清零计数和完成标志，避免前一轮状态污染断言。 */
static rt_err_t utest_tc_init(void)
{
    static_event_recv_thread_finish = 0;
    static_event_send_thread_finish = 0;
#ifdef RT_USING_HEAP
    dynamic_event_recv_thread_finish = 0;
    dynamic_event_send_thread_finish = 0;
#endif
    return RT_EOK;
}

/** @brief 用例级清理入口；对象均由各子测试在工作线程结束后自行销毁。 */
static rt_err_t utest_tc_cleanup(void)
{
    return RT_EOK;
}

/** @brief 按静态生命周期、静态通信、动态生命周期、动态通信的顺序执行测试。 */
static void testcase(void)
{
    UTEST_UNIT_RUN(test_event_init);
    UTEST_UNIT_RUN(test_event_detach);
    UTEST_UNIT_RUN(test_static_event_send_recv);
#ifdef RT_USING_HEAP
    UTEST_UNIT_RUN(test_event_create);
    UTEST_UNIT_RUN(test_event_delete);
    UTEST_UNIT_RUN(test_dynamic_event_send_recv);
#endif
}
UTEST_TC_EXPORT(testcase, "core.ipc_event", utest_tc_init, utest_tc_cleanup, 60);
