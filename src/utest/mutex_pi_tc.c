/*
 * Copyright (c) 2006-2019, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 */

/**
 * @file mutex_pi_tc.c
 * @brief 互斥量优先级继承（PI）、超时撤销、嵌套传播和异常唤醒回归测试。
 *
 * 优先级数值越小越高。高优先级线程等待低优先级 owner 时，owner 应临时继承
 * 该优先级；释放、超时或等待者异常离队后，应按仍持锁集合正确回退。本测试用
 * 3 把锁、5 个工作线程与一个主工作线程构造链式依赖，再按指定次序拆解。
 * `_sync_flag` 既是统一起跑门也是完成计数。断言覆盖 `RT_EOK`、
 * `-RT_ETIMEOUT`、`-RT_EINTR`，以及每阶段 current/init priority 的关系。
 */

#define __RT_IPC_SOURCE__

#include <rtthread.h>
#include <stdlib.h>
#include "utest.h"

#ifdef ARCH_CPU_64BIT
#define THREAD_STACKSIZE 8192
#else
#define THREAD_STACKSIZE 4096
#endif

#define MUTEX_NUM 3
#define THREAD_NUM 5

static struct rt_mutex _mutex[MUTEX_NUM];
static volatile int _sync_flag;

/** @brief 普通竞争线程：按编号选择互斥量并验证取得、释放后的有效优先级。 */
static void test_thread_entry(void *para)
{
    while (!_sync_flag)
    {
        rt_thread_delay(1);
    }

    rt_ubase_t thread_id = (rt_ubase_t)para;
    rt_err_t ret;
    rt_thread_mdelay(50 + thread_id * 100);
    ret = rt_mutex_take(&_mutex[thread_id % MUTEX_NUM], RT_WAITING_FOREVER);
    uassert_true(ret == RT_EOK);
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == RT_SCHED_PRIV(rt_thread_self()).init_priority);

    if (thread_id == 1)
    {
        rt_thread_mdelay(100); // 等待主工作线程重新获取 _mutex[1]。
        uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 8);
    }

    ret = rt_mutex_release(&_mutex[thread_id % MUTEX_NUM]);
    uassert_true(ret == RT_EOK);
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == RT_SCHED_PRIV(rt_thread_self()).init_priority);

    _sync_flag ++;
}

/** @brief 按精确延时取得和释放三把锁，构造并拆解多级 PI 依赖。 */
static void test_main_thread_entry(void *para)
{
    while (!_sync_flag)
    {
        rt_thread_delay(1);
    }

    rt_err_t ret;

    ret = rt_mutex_take(&_mutex[0], RT_WAITING_FOREVER);
    uassert_true(ret == RT_EOK);
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 12);
    rt_thread_mdelay(100);         // 等待 t0 尝试取得 mutex0。
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 12);

    ret = rt_mutex_take(&_mutex[1], RT_WAITING_FOREVER);
    uassert_true(ret == RT_EOK);
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 12);
    rt_thread_mdelay(100);         // 等待 t1 尝试取得 mutex1。
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 9);

    ret = rt_mutex_take(&_mutex[2], RT_WAITING_FOREVER);
    uassert_true(ret == RT_EOK);
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 9);
    rt_thread_mdelay(100);         // 等待 t2 尝试取得 mutex2。
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 8);

    rt_thread_mdelay(100);         // 等待 t3 进入 mutex0 等待链。
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 7);

    rt_thread_mdelay(100);         // 等待 t4 进入 mutex1 等待链。
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 7);

    rt_thread_mdelay(100);
    rt_mutex_release(&_mutex[0]);   // 将 _mutex0 移交给 t3。
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 8);

    rt_thread_mdelay(100);
    rt_mutex_release(&_mutex[1]);   // 将 _mutex1 移交给 t1。
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 8);

    rt_thread_mdelay(50);
    rt_mutex_take(&_mutex[1], RT_WAITING_FOREVER);   // 再次等待当前由 t1 持有的 _mutex1。
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 8);
    rt_mutex_release(&_mutex[1]);   // 释放本线程取得的 _mutex1。
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 8);

    rt_thread_mdelay(100);
    rt_mutex_release(&_mutex[2]);
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 12);

    _sync_flag ++;
}

/** @brief 创建基础 PI 链的所有锁和不同优先级线程，等待完成计数后统一 detach。 */
static void test_mutex_pi(void)
{
    rt_thread_t t_main;
    rt_thread_t t[THREAD_NUM];
    rt_uint8_t prio[THREAD_NUM] = {13, 9, 8, 7, 11}; // 五个工作线程的基础优先级。

    for (int i = 0; i < MUTEX_NUM; i++)
    {
        rt_mutex_init(&_mutex[i], "test1", 0);
    }

    _sync_flag  = 0;

    t_main = rt_thread_create("t_main", test_main_thread_entry, RT_NULL, THREAD_STACKSIZE, 12, 10000);
    uassert_true(t_main != RT_NULL);
    rt_thread_startup(t_main);

    for (rt_ubase_t i = 0; i < THREAD_NUM; i++)
    {
        t[i] = rt_thread_create("t", test_thread_entry, (void *)i, THREAD_STACKSIZE, prio[i], 10000);
        uassert_true(t[i] != RT_NULL);
        rt_thread_startup(t[i]);
    }

    _sync_flag = 1;

    while (_sync_flag != THREAD_NUM + 1 + 1)
    {
        rt_thread_mdelay(100);
    }

    for (int i = 0; i < MUTEX_NUM; i++)
    {
        rt_mutex_detach(&_mutex[i]);
    }
}

static struct rt_mutex _timeout_mutex;

/** @brief timeout 场景的低优先级 owner，持锁跨过高优先级等待者的截止时间。 */
static void test_main_timeout_entry(void *para)
{
    rt_err_t ret;

    ret = rt_mutex_take(&_timeout_mutex, RT_WAITING_FOREVER);
    uassert_true(ret == -RT_EOK);
    rt_thread_mdelay(100);
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 10);
    rt_thread_mdelay(100);
    uassert_true(RT_SCHED_PRIV(rt_thread_self()).current_priority == 12);
    rt_mutex_release(&_timeout_mutex);
    _sync_flag ++;
}

/** @brief 高优先级有限等待者，断言超时并触发 owner 的继承优先级回退。 */
static void test_timeout_entry(void *para)
{
    rt_err_t ret;

    rt_thread_mdelay(50);
    ret = rt_mutex_take(&_timeout_mutex, rt_tick_from_millisecond(100));
    uassert_true(ret == -RT_ETIMEOUT);
    _sync_flag ++;
}

/** @brief 组织 PI 超时场景并等待 owner、waiter 都完成后脱离互斥量。 */
static void test_mutex_pi_timeout(void)
{
    _sync_flag = 0;

    rt_mutex_init(&_timeout_mutex, "_timeout_mutex", 0);

    rt_thread_t t1 = rt_thread_create("t1", test_main_timeout_entry, RT_NULL, THREAD_STACKSIZE, 12, 10000);
    uassert_true(t1 != RT_NULL);
    rt_thread_startup(t1);

    rt_thread_t t2 = rt_thread_create("t2", test_timeout_entry, (void *)t1, THREAD_STACKSIZE, 10, 10000);
    uassert_true(t2 != RT_NULL);
    rt_thread_startup(t2);

    while (_sync_flag != 2)
    {
        rt_thread_mdelay(100);
    }

    rt_mutex_detach(&_timeout_mutex);
}

#define TC_THREAD_NUM 4
#define TC_MUTEX_NUM TC_THREAD_NUM
static rt_thread_t t[TC_THREAD_NUM], t_hi_prio;
static struct rt_mutex m[TC_MUTEX_NUM];

/** @brief 链式依赖节点：先持有自己的锁，再等待前一把锁，形成 owner 等待 owner。 */
static void test_recursive_mutex_depend_entry(void *para)
{
    rt_ubase_t id = (rt_ubase_t)para;

    rt_mutex_take(&m[id], RT_WAITING_FOREVER);

    rt_thread_mdelay(50);

    if (id != 0)
    {
        rt_mutex_take(&m[id - 1], RT_WAITING_FOREVER);
    }

    if (id == 0)
    {
        rt_thread_mdelay(250);
        rt_mutex_release(&m[id]);
    }
    else
    {
        rt_mutex_release(&m[id - 1]);
        rt_mutex_release(&m[id]);
    }
    _sync_flag ++;
}

/** @brief 链尾高优先级有限等待者，使优先级 3 沿完整依赖链传播，随后超时。 */
static void test_recursive_mutex_depend_hi_pri_entry(void *para)
{
    rt_thread_mdelay(100);
    rt_err_t err = rt_mutex_take(&m[TC_MUTEX_NUM - 1], rt_tick_from_millisecond(100));
    uassert_true(err == -RT_ETIMEOUT);
    _sync_flag ++;
}

/** @brief 断言嵌套链全部提升到 3，并在高优先级等待超时后全部恢复到 10。 */
static void test_mutex_pi_recursive_prio_update(void)
{
    _sync_flag = 0;

    for (int i = 0; i < TC_MUTEX_NUM; i++)
    {
        rt_mutex_init(&m[i], "test", 0);
    }

    for (rt_ubase_t i = 0; i < TC_THREAD_NUM; i++)
    {
        t[i] = rt_thread_create("t", test_recursive_mutex_depend_entry, (void *)i, THREAD_STACKSIZE, 10, 10000);
        rt_thread_startup(t[i]);
    }
    t_hi_prio = rt_thread_create("t", test_recursive_mutex_depend_hi_pri_entry, (void *)RT_NULL, THREAD_STACKSIZE, 3, 10000);
    rt_thread_startup(t_hi_prio);

    rt_thread_mdelay(150);

    for (int i = 0; i < TC_THREAD_NUM; i++)
    {
        uassert_true(RT_SCHED_PRIV(t[i]).current_priority == 3);
    }

    rt_thread_mdelay(100);

    for (int i = 0; i < TC_THREAD_NUM; i++)
    {
        uassert_true(RT_SCHED_PRIV(t[i]).current_priority == 10);
    }

    while (_sync_flag != TC_THREAD_NUM + 1)
    {
        rt_thread_mdelay(100);
    }

    for (int i = 0; i < TC_MUTEX_NUM; i++)
    {
        rt_mutex_detach(&m[i]);
    }
    _sync_flag ++;
}

/** @brief 永久等待链尾锁；被定时器外部 resume 后预期返回 `-RT_EINTR`。 */
static void test_mutex_waiter_to_wakeup_entry(void *para)
{
    rt_thread_mdelay(100);
    rt_err_t err = rt_mutex_take(&m[TC_MUTEX_NUM - 1], RT_WAITING_FOREVER);
    uassert_true(err == -RT_EINTR);
    _sync_flag ++;
}

/** @brief 单次定时器回调，主动恢复高优先级等待线程以模拟异常唤醒。 */
static void wakeup_func(void *para)
{
    rt_thread_resume(t_hi_prio);
}
/** @brief 验证异常唤醒会从等待链移除线程并撤销整条 PI 传播。 */
static void test_mutex_pi_wakeup_mutex_waiter(void)
{
    struct rt_timer wakeup_timer;

    _sync_flag = 0;

    for (int i = 0; i < TC_MUTEX_NUM; i++)
    {
        rt_mutex_init(&m[i], "test", 0);
    }

    for (rt_ubase_t i = 0; i < TC_THREAD_NUM; i++)
    {
        t[i] = rt_thread_create("t", test_recursive_mutex_depend_entry, (void *)i, THREAD_STACKSIZE, 10, 10000);
        rt_thread_startup(t[i]);
    }
    t_hi_prio = rt_thread_create("t", test_mutex_waiter_to_wakeup_entry, (void *)RT_NULL, THREAD_STACKSIZE, 3, 10000);
    rt_thread_startup(t_hi_prio);

    rt_timer_init(&wakeup_timer, "wakeup_timer", wakeup_func, RT_NULL, rt_tick_from_millisecond(200), RT_TIMER_FLAG_ONE_SHOT);
    rt_timer_start(&wakeup_timer);
    rt_thread_mdelay(150);

    for (int i = 0; i < TC_THREAD_NUM; i++)
    {
        uassert_true(RT_SCHED_PRIV(t[i]).current_priority == 3);
    }

    rt_thread_mdelay(100);

    for (int i = 0; i < TC_THREAD_NUM; i++)
    {
        uassert_true(RT_SCHED_PRIV(t[i]).current_priority == 10);
    }

    while (_sync_flag != TC_THREAD_NUM + 1)
    {
        rt_thread_mdelay(100);
    }

    for (int i = 0; i < TC_MUTEX_NUM; i++)
    {
        rt_mutex_detach(&m[i]);
    }
    rt_timer_detach(&wakeup_timer);
}

/** @brief utest 初始化入口；子用例自行重置其同步状态。 */
static rt_err_t utest_tc_init(void)
{
    return RT_EOK;
}

/** @brief utest 清理入口；各子用例已等待线程并释放锁/定时器。 */
static rt_err_t utest_tc_cleanup(void)
{
    return RT_EOK;
}

/** @brief 顺序执行基础 PI、递归传播、超时撤销和异常唤醒测试。 */
static void testcase(void)
{
    UTEST_UNIT_RUN(test_mutex_pi);
    UTEST_UNIT_RUN(test_mutex_pi_recursive_prio_update);
    UTEST_UNIT_RUN(test_mutex_pi_timeout);
    UTEST_UNIT_RUN(test_mutex_pi_wakeup_mutex_waiter);
}
UTEST_TC_EXPORT(testcase, "core.mutex_pi", utest_tc_init, utest_tc_cleanup, 1000);

/********************* 文件结束 ************************/
