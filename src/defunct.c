/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-08-30     heyuanjie87  the first version
 *
*/

/**
 * @file defunct.c
 * @brief 延迟回收已经退出线程（defunct/zombie thread）的资源。
 *
 * 线程不能在自己的退出栈上立即释放该栈，否则后续退栈和上下文切换会访问已释放
 * 内存。因此退出路径只把线程从调度系统移走，并挂入 _rt_thread_defunct 链表；
 * 真正的对象分离、cleanup 回调、栈和控制块释放由另一个安全上下文稍后完成。
 *
 * UP 且非 Smart 配置由空闲线程调用 rt_defunct_execute()；SMP 或 Smart 配置创建
 * 独立的 tsystem 线程，并用 system_sem 在新僵尸入队时唤醒它。队列可能被多个
 * CPU 或中断相关路径访问，因此 _defunct_spinlock 同时保护入队和出队。
 */

#include <rthw.h>
#include <rtthread.h>

#ifndef SYSTEM_THREAD_STACK_SIZE
#define SYSTEM_THREAD_STACK_SIZE IDLE_THREAD_STACK_SIZE
#endif
/* 等待回收的线程链表；线程复用自身的 RT_THREAD_LIST_NODE 链接到这里。 */
static rt_list_t          _rt_thread_defunct = RT_LIST_OBJECT_INIT(_rt_thread_defunct);
/* 保护僵尸链表，irqsave 形式允许调用者来自不同中断状态。 */
static struct rt_spinlock _defunct_spinlock;
#if defined(RT_USING_SMP) || defined(RT_USING_SMART)
static struct rt_thread rt_system_thread;
rt_align(RT_ALIGN_SIZE) static rt_uint8_t rt_system_stack[SYSTEM_THREAD_STACK_SIZE];
static struct rt_semaphore system_sem;
#endif

/**
 * @brief 将已经退出调度的线程加入延迟回收队列。
 *
 * 插入操作在带中断保护的自旋锁内完成。SMP/Smart 下随后释放 system_sem，使专用
 * 系统线程尽快执行回收；信号量计数也能保留“在系统线程尚未等待前已经入队”的
 * 通知。
 *
 * @param thread 已停止运行且不会再次进入就绪队列的线程。
 *
 * @note 调用者必须保证该线程尚未在队列中，且其栈/控制块仍然有效。
 */
void rt_thread_defunct_enqueue(rt_thread_t thread)
{
    rt_base_t level;
    level = rt_spin_lock_irqsave(&_defunct_spinlock);
    rt_list_insert_after(&_rt_thread_defunct, &RT_THREAD_LIST_NODE(thread));
    rt_spin_unlock_irqrestore(&_defunct_spinlock, level);
#if defined(RT_USING_SMP) || defined(RT_USING_SMART)
    rt_sem_release(&system_sem);
#endif
}

/**
 * @brief 从延迟回收队列原子取出一个线程。
 *
 * 锁内只做链表摘除，所有可能耗时的销毁动作都留给锁外的 rt_defunct_execute()，
 * 从而缩短关中断和自旋锁临界区。
 *
 * @return 一个待回收线程；队列为空时返回 RT_NULL。
 */
rt_thread_t rt_thread_defunct_dequeue(void)
{
    rt_base_t   level;
    rt_thread_t thread = RT_NULL;
    rt_list_t  *l      = &_rt_thread_defunct;

    level = rt_spin_lock_irqsave(&_defunct_spinlock);
    if (!rt_list_isempty(l))
    {
        thread = RT_THREAD_LIST_NODE_ENTRY(l->next);
        rt_list_remove(&RT_THREAD_LIST_NODE(thread));
    }
    rt_spin_unlock_irqrestore(&_defunct_spinlock, level);

    return thread;
}

/**
 * @brief 回收当前队列中的全部僵尸线程。
 *
 * 对每个线程按以下顺序处理：销毁关联模块（若有）、释放信号资源、保存 cleanup
 * 指针、把静态线程对象从对象系统分离、执行用户 cleanup，最后仅对动态线程释放
 * 内存保护描述、栈和对象控制块。必须先保存 cleanup，因为对象分离/删除会改变对象
 * 生命周期；必须在释放栈和控制块之前执行回调，以保证回调还能查看线程信息。
 *
 * 本函数不持有僵尸队列锁调用用户回调，但执行者可能是 UP 空闲线程或专用低优先级
 * 系统线程，因此 cleanup 必须短小且不可阻塞，也不能尝试重新启动已退出线程。
 */
void rt_defunct_execute(void)
{
    /* 循环到队列为空；执行期间新入队的线程也会在本轮继续被处理。 */
    while (1)
    {
        rt_thread_t thread;
        rt_bool_t   object_is_systemobject;
        void (*cleanup)(struct rt_thread *tid);

#ifdef RT_USING_MODULE
        struct rt_dlmodule *module = RT_NULL;
#endif
        /* 锁内仅摘下一个节点，后续昂贵操作全部在锁外完成。 */
        thread = rt_thread_defunct_dequeue();
        if (thread == RT_NULL)
        {
            break;
        }

#ifdef RT_USING_MODULE
        module = (struct rt_dlmodule *)thread->parent.module_id;
        if (module)
        {
            dlmodule_destroy(module);
        }
#endif

#ifdef RT_USING_SIGNALS
        rt_thread_free_sig(thread);
#endif

        /* 先保存回调指针，防止后续对象生命周期操作使其不可再访问。 */
        cleanup = thread->cleanup;

        /* 静态线程属于系统对象：只从对象容器分离，不能释放其静态内存。 */
        object_is_systemobject = rt_object_is_systemobject((rt_object_t)thread);
        if (object_is_systemobject == RT_TRUE)
        {
            /* 分离后，该线程不再能被对象查找接口找到。 */
            rt_object_detach((rt_object_t)thread);
        }

        /* 在控制块仍有效时调用一次线程清理钩子。 */
        if (cleanup != RT_NULL)
        {
            cleanup(thread);
        }

#ifdef RT_USING_HEAP
#ifdef RT_USING_MEM_PROTECTION
        if (thread->mem_regions != RT_NULL)
        {
            RT_KERNEL_FREE(thread->mem_regions);
        }
#endif
        /* 动态线程才拥有由内核堆分配、需要释放的栈和对象控制块。 */
        if (object_is_systemobject == RT_FALSE)
        {
            /* 硬件栈保护配置保存原始 stack_buf，否则直接释放 stack_addr。 */
#ifdef RT_USING_HW_STACK_GUARD
            RT_KERNEL_FREE(thread->stack_buf);
#else
            RT_KERNEL_FREE(thread->stack_addr);
#endif
            /* 最后删除线程对象本身；此后 thread 指针立即失效。 */
            rt_object_delete((rt_object_t)thread);
        }
#endif
    }
}

#if defined(RT_USING_SMP) || defined(RT_USING_SMART)
static void rt_thread_system_entry(void *parameter)
{
    RT_UNUSED(parameter);

    /* 每次入队都会释放一次信号量；被唤醒后批量清空队列。 */
    while (1)
    {
        int ret = rt_sem_take(&system_sem, RT_WAITING_FOREVER);
        if (ret != RT_EOK)
        {
            rt_kprintf("failed to sem_take() error %d\n", ret);
            RT_ASSERT(0);
        }
        rt_defunct_execute();
    }
}
#endif

void rt_thread_defunct_init(void)
{
    RT_ASSERT(RT_THREAD_PRIORITY_MAX > 2);

    rt_spin_lock_init(&_defunct_spinlock);

#if defined(RT_USING_SMP) || defined(RT_USING_SMART)
    rt_sem_init(&system_sem, "defunct", 0, RT_IPC_FLAG_FIFO);

    /* 创建倒数第二低优先级的回收线程，既不抢占正常工作，也高于空闲线程。 */
    rt_thread_init(&rt_system_thread,
                   "tsystem",
                   rt_thread_system_entry,
                   RT_NULL,
                   rt_system_stack,
                   sizeof(rt_system_stack),
                   RT_THREAD_PRIORITY_MAX - 2,
                   32);
    /* 加入就绪队列；首次运行后通常阻塞在 system_sem 上。 */
    rt_thread_startup(&rt_system_thread);
#endif
}
