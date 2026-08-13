/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2006-03-14     Bernard      初始版本
 * 2006-04-21     Bernard      将调度器锁改为中断锁
 * 2006-05-18     Bernard      修复对象初始化问题
 * 2006-08-03     Bernard      增加 hook 支持
 * 2007-01-28     Bernard      将 RT_OBJECT_Class_Static 重命名为 RT_Object_Class_Static
 * 2010-10-26     yi.qiu       为 rt_object_allocate 和 rt_object_free 增加模块支持
 * 2017-12-10     Bernard      增加 object_info 枚举
 * 2018-01-25     Bernard      修复启用 MODULE 时的对象查找问题
 * 2022-01-07     Gabriel      将 __on_rt_xxxxx_hook 移入 object.c
 * 2023-09-15     xqyjlj       优化 rt_hw_interrupt_disable/enable
 * 2023-11-17     xqyjlj       增加进程组和会话支持
 */

#include <rtthread.h>
#include <rthw.h>

/**
 * @file object.c
 * @brief RT-Thread 内核对象的统一登记、查找、遍历和生命周期管理实现。
 *
 * RT-Thread 把线程、信号量、互斥量、事件、邮箱、消息队列、定时器、设备等
 * 都设计为“通用对象头 `struct rt_object` + 各类型私有字段”。通用对象头保存
 * 类型、名称、标志和链表节点；本文件按对象类型维护一张
 * `rt_object_information` 表，其中每一项记录对象类、该类对象链、对象大小以及
 * 保护链表的自旋锁。这样调试器、shell 和内核公共代码便可用相同方式枚举对象。
 *
 * 对象生命周期分为两类：
 *
 * - 静态对象：存储由调用者提供，`rt_object_init()` 登记时在类型中加入
 *   `RT_Object_Class_Static`，结束时 `rt_object_detach()` 只注销、不释放内存。
 * - 动态对象：`rt_object_allocate()` 按类型表中的大小从内核堆分配并登记，
 *   `rt_object_delete()` 注销后释放内存。两套结束接口不可混用。
 *
 * 若启用模块并且当前线程属于动态模块，新对象会挂到该模块的私有对象链，
 * 而不是全局类型链，便于卸载模块时统一清理。全局遍历和按名查找只遍历类型
 * 容器的全局链，并不会自动搜索各模块的私有链。
 *
 * 每个类型链由其 `spinlock` 保护，锁操作同时保存并关闭本地中断。特别注意：
 * `rt_object_for_each()` 在持锁期间调用用户迭代回调，所以回调必须短小，不能
 * 阻塞，也不能调用会获取同一类型对象锁的创建、删除、查找或遍历操作。
 */

#ifdef RT_USING_MODULE
#include <dlmodule.h>
#endif /* RT_USING_MODULE */

#ifdef RT_USING_SMART
#include <lwp.h>
#endif

#define DBG_TAG           "kernel.obj"
#define DBG_LVL           DBG_ERROR
#include <rtdbg.h>

struct rt_custom_object
{
    struct rt_object parent;       /**< 必须位于首字段，使其可转换为通用对象指针。 */
    rt_err_t (*destroy)(void *);   /**< 可选的用户数据销毁回调，在对象内存释放前调用。 */
    void *data;                    /**< 内核不解释的用户数据，由创建者和 destroy 管理。 */
};

/*
 * 该内部枚举只用于给 `_object_container[]` 分配稳定下标，并不是公开的对象类型。
 * 条目随配置宏增减，最后的 Unknown 同时充当数组长度和查找失败哨兵。
 */
enum rt_object_info_type
{
    RT_Object_Info_Thread = 0,                         /**< 线程对象容器。 */
#ifdef RT_USING_SEMAPHORE
    RT_Object_Info_Semaphore,                          /**< 信号量对象容器。 */
#endif
#ifdef RT_USING_MUTEX
    RT_Object_Info_Mutex,                              /**< 互斥量对象容器。 */
#endif
#ifdef RT_USING_EVENT
    RT_Object_Info_Event,                              /**< 事件对象容器。 */
#endif
#ifdef RT_USING_MAILBOX
    RT_Object_Info_MailBox,                            /**< 邮箱对象容器。 */
#endif
#ifdef RT_USING_MESSAGEQUEUE
    RT_Object_Info_MessageQueue,                       /**< 消息队列对象容器。 */
#endif
#ifdef RT_USING_MEMHEAP
    RT_Object_Info_MemHeap,                            /**< memheap 内存堆对象容器。 */
#endif
#ifdef RT_USING_MEMPOOL
    RT_Object_Info_MemPool,                            /**< 固定块内存池对象容器。 */
#endif
#ifdef RT_USING_DEVICE
    RT_Object_Info_Device,                             /**< 设备对象容器。 */
#endif
    RT_Object_Info_Timer,                              /**< 定时器对象容器。 */
#ifdef RT_USING_MODULE
    RT_Object_Info_Module,                             /**< 动态模块对象容器。 */
#endif
#ifdef RT_USING_HEAP
    RT_Object_Info_Memory,                             /**< 小内存管理器对象容器。 */
#endif
#ifdef RT_USING_SMART
    RT_Object_Info_Channel,                            /**< Smart IPC 通道对象容器。 */
    RT_Object_Info_ProcessGroup,                       /**< 进程组对象容器。 */
    RT_Object_Info_Session,                            /**< 会话对象容器。 */
#endif
#ifdef RT_USING_HEAP
    RT_Object_Info_Custom,                             /**< 携带用户数据的自定义对象容器。 */
#endif
    RT_Object_Info_Unknown,                            /**< 未知类型哨兵，同时等于容器数组长度。 */
};

/* 让指定容器的双向循环链表头在静态初始化阶段指向自身。 */
#define _OBJ_CONTAINER_LIST_INIT(c)     \
    {&(_object_container[c].object_list), &(_object_container[c].object_list)}

static struct rt_object_information _object_container[RT_Object_Info_Unknown] =
{
    /* 每项依次给出：公开对象类、全局对象链表头、实例大小、链表自旋锁。 */
    /* 初始化线程对象容器。 */
    {RT_Object_Class_Thread, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Thread), sizeof(struct rt_thread), RT_SPINLOCK_INIT},
#ifdef RT_USING_SEMAPHORE
    /* 初始化信号量对象容器。 */
    {RT_Object_Class_Semaphore, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Semaphore), sizeof(struct rt_semaphore), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_MUTEX
    /* 初始化互斥量对象容器。 */
    {RT_Object_Class_Mutex, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Mutex), sizeof(struct rt_mutex), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_EVENT
    /* 初始化事件对象容器。 */
    {RT_Object_Class_Event, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Event), sizeof(struct rt_event), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_MAILBOX
    /* 初始化邮箱对象容器。 */
    {RT_Object_Class_MailBox, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_MailBox), sizeof(struct rt_mailbox), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_MESSAGEQUEUE
    /* 初始化消息队列对象容器。 */
    {RT_Object_Class_MessageQueue, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_MessageQueue), sizeof(struct rt_messagequeue), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_MEMHEAP
    /* 初始化 memheap 对象容器。 */
    {RT_Object_Class_MemHeap, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_MemHeap), sizeof(struct rt_memheap), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_MEMPOOL
    /* 初始化固定块内存池对象容器。 */
    {RT_Object_Class_MemPool, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_MemPool), sizeof(struct rt_mempool), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_DEVICE
    /* 初始化设备对象容器。 */
    {RT_Object_Class_Device, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Device), sizeof(struct rt_device), RT_SPINLOCK_INIT},
#endif
    /* 初始化定时器对象容器。 */
    {RT_Object_Class_Timer, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Timer), sizeof(struct rt_timer), RT_SPINLOCK_INIT},
#ifdef RT_USING_MODULE
    /* 初始化动态模块对象容器。 */
    {RT_Object_Class_Module, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Module), sizeof(struct rt_dlmodule), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_HEAP
    /* 初始化小内存管理器对象容器。 */
    {RT_Object_Class_Memory, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Memory), sizeof(struct rt_memory), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_SMART
    /* 初始化 Smart 进程相关对象容器。 */
    {RT_Object_Class_Channel, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Channel), sizeof(struct rt_channel), RT_SPINLOCK_INIT},
    {RT_Object_Class_ProcessGroup, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_ProcessGroup), sizeof(struct rt_processgroup), RT_SPINLOCK_INIT},
    {RT_Object_Class_Session, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Session), sizeof(struct rt_session), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_HEAP
    {RT_Object_Class_Custom, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Custom), sizeof(struct rt_custom_object), RT_SPINLOCK_INIT},
#endif
};

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
static void (*rt_object_attach_hook)(struct rt_object *object);
static void (*rt_object_detach_hook)(struct rt_object *object);
void (*rt_object_trytake_hook)(struct rt_object *object);
void (*rt_object_take_hook)(struct rt_object *object);
void (*rt_object_put_hook)(struct rt_object *object);

/**
 * @addtogroup group_hook
 * @{
 */

/**
 * @brief 设置对象即将挂入内核对象系统时调用的 attach hook。
 *
 * `rt_object_init()` 和 `rt_object_allocate()` 在对象基本字段已经初始化、但尚未
 * 获取类型容器锁和插入链表之前调用该钩子。因此钩子可以读取类型、名称和
 * 标志，但此刻其他执行流还不能通过全局对象链找到它。钩子运行上下文就是
 * 创建者上下文，且本函数只替换一个全局函数指针，不提供注册队列或同步等待。
 *
 * @param hook 新钩子；传入 `RT_NULL` 表示关闭该观察点。
 */
void rt_object_attach_sethook(void (*hook)(struct rt_object *object))
{
    rt_object_attach_hook = hook;
}

/**
 * @brief 设置对象即将从内核对象系统注销时调用的 detach hook。
 *
 * `rt_object_detach()` 和 `rt_object_delete()` 在对象类型仍有效、对象仍位于登记
 * 链表中、且尚未获取容器锁时调用该钩子。动态删除路径在钩子返回之后才释放
 * 内存。钩子应当只做短小观察，不能假设对象在回调返回后仍然存在。
 *
 * @param hook 新钩子；传入 `RT_NULL` 清除钩子。
 */
void rt_object_detach_sethook(void (*hook)(struct rt_object *object))
{
    rt_object_detach_hook = hook;
}

/**
 * @brief 设置 IPC 对象“尝试获取”时调用的 try-take hook。
 *
 * “获取”的含义随对象类型而变：线程尝试占用信号量或互斥量、接收事件、从
 * 邮箱取邮件、从消息队列取消息。该钩子通常位于具体 IPC 操作的早期，成功与
 * 否尚未确定；它是在何种锁和上下文下执行由对应 IPC 实现决定，故实现必须
 * 不阻塞，并避免重入同一 IPC 对象。
 *
 * @param hook 新钩子；传入 `RT_NULL` 清除钩子。
 */
void rt_object_trytake_sethook(void (*hook)(struct rt_object *object))
{
    rt_object_trytake_hook = hook;
}

/**
 * @brief 设置对象已经成功“获取/启动”时调用的 take hook。
 *
 * 对 IPC 对象表示获取或接收已成功，对定时器表示已经进入启动流程。调用点的
 * 持锁状态由各子系统决定，例如定时器 take hook 在定时器表锁内调用。hook
 * 适合跟踪和统计，不应改变对象状态或执行可能睡眠的操作。
 *
 * @param hook 新钩子；传入 `RT_NULL` 清除钩子。
 */
void rt_object_take_sethook(void (*hook)(struct rt_object *object))
{
    rt_object_take_hook = hook;
}

/**
 * @brief 设置 IPC 对象释放/发送或定时器停止时调用的 put hook。
 *
 * “put”包括释放信号量/互斥量、发送事件或消息，以及停止定时器。具体调用点
 * 可能持有对象自旋锁并关闭本地中断，所以钩子必须非阻塞、不可递归操作同一
 * 对象。该接口直接替换全局函数指针。
 *
 * @param hook 新钩子；传入 `RT_NULL` 清除钩子。
 */
void rt_object_put_sethook(void (*hook)(struct rt_object *object))
{
    rt_object_put_hook = hook;
}

/** @} group_hook */
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_object_management
 * @{
 */

/**
 * @brief 根据公开对象类型查找对应的对象容器描述符。
 *
 * 查找前会清除 `RT_Object_Class_Static` 位，所以静态与动态对象共享同一个类型
 * 容器。函数只读取启动时构造好的静态表，不需要加锁。
 *
 * @param type `RT_Object_Class_Thread`、`RT_Object_Class_Semaphore` 等公开类型，
 *             可以带静态标志位。
 * @return 找到时返回对应的全局描述符；配置未启用或类型未知时返回 `RT_NULL`。
 */
struct rt_object_information *
rt_object_get_information(enum rt_object_class_type type)
{
    int index;

    type = (enum rt_object_class_type)(type & ~RT_Object_Class_Static);

    for (index = 0; index < RT_Object_Info_Unknown; index ++)
        if (_object_container[index].type == type) return &_object_container[index];

    return RT_NULL;
}
RTM_EXPORT(rt_object_get_information);

/**
 * @brief 统计指定类型的全局对象链当前包含多少个对象。
 *
 * 遍历期间持有该类型容器的自旋锁并关闭本地中断，因此得到的是临界区内一致
 * 的瞬时数量。启用模块时，挂在各模块私有链的对象不计入此结果。
 *
 * @param type 要统计的公开对象类型。
 * @return 全局链对象数量；类型无效或未编译时返回 0。
 */
int rt_object_get_length(enum rt_object_class_type type)
{
    int count = 0;
    rt_base_t level;
    struct rt_list_node *node = RT_NULL;
    struct rt_object_information *information = RT_NULL;

    information = rt_object_get_information((enum rt_object_class_type)type);
    if (information == RT_NULL) return 0;

    level = rt_spin_lock_irqsave(&(information->spinlock));
    rt_list_for_each(node, &(information->object_list))
    {
        count ++;
    }
    rt_spin_unlock_irqrestore(&(information->spinlock), level);

    return count;
}
RTM_EXPORT(rt_object_get_length);

/**
 * @brief 把指定类型全局链中的对象指针复制到调用者数组。
 *
 * 复制时持有类型容器锁，因此链表不会在中途变化；但解锁后对象仍可能被并发
 * 删除，函数不会为返回的裸指针增加引用计数。调用者若要继续解引用，必须用
 * 更高层生命周期规则保证对象仍存活。模块私有对象不在复制范围内。
 *
 * @param type 要枚举的公开对象类型。
 *
 * @param pointers 接收对象指针的数组，容量至少为 `maxlen`。
 *
 * @param maxlen 最多复制的指针个数；小于等于 0 时不访问数组。
 * @return 实际复制的对象指针个数；类型无效时返回 0。
 */
int rt_object_get_pointers(enum rt_object_class_type type, rt_object_t *pointers, int maxlen)
{
    int index = 0;
    rt_base_t level;

    struct rt_object *object;
    struct rt_list_node *node = RT_NULL;
    struct rt_object_information *information = RT_NULL;

    if (maxlen <= 0) return 0;

    information = rt_object_get_information(type);
    if (information == RT_NULL) return 0;

    level = rt_spin_lock_irqsave(&(information->spinlock));
    /* 在同一个锁周期内按链表顺序快照最多 maxlen 个裸指针。 */
    rt_list_for_each(node, &(information->object_list))
    {
        object = rt_list_entry(node, struct rt_object, list);

        pointers[index] = object;
        index ++;

        if (index >= maxlen) break;
    }
    rt_spin_unlock_irqrestore(&(information->spinlock), level);

    return index;
}
RTM_EXPORT(rt_object_get_pointers);

/**
 * @brief 初始化并登记一个使用调用者自备存储的静态内核对象。
 *
 * 控制流为：查找类型容器；调试配置下检查同一地址未重复登记；写入静态类型和
 * 名称；在未持容器锁时调用 attach hook；最后加锁，把对象挂入当前模块私有链
 * 或全局类型链。函数只初始化通用对象头，各具体类型还必须继续初始化私有字段。
 *
 * @param object 指向有效且足够大的对象存储；不能为 `RT_NULL`、野指针或仍在
 *               登记链中的对象。存储由调用者负责，之后用 `rt_object_detach()`。
 *
 * @param type `rt_object_class_type` 中的具体类，不应由调用者附加 Static 位；
 *             本函数会自动加入 `RT_Object_Class_Static`。
 *
 * @param name 可选对象名。`RT_NAME_MAX > 0` 时最多复制 `RT_NAME_MAX - 1` 字节并
 *             强制补 `\0`，过长名称会记录错误但仍截断；为 0 时名称指针直接
 *             保存，调用者必须保证字符串生命周期覆盖对象生命周期。实现不检查
 *             名称唯一性，同类对象可以重名，按名查找只会返回链表中先遇到者。
 */
void rt_object_init(struct rt_object         *object,
                    enum rt_object_class_type type,
                    const char               *name)
{
    rt_base_t level;
    rt_size_t obj_name_len;
#ifdef RT_DEBUGING_ASSERT
    struct rt_list_node *node = RT_NULL;
#endif /* RT_DEBUGING_ASSERT */
    struct rt_object_information *information;
#ifdef RT_USING_MODULE
    struct rt_dlmodule *module = dlmodule_self();
#endif /* RT_USING_MODULE */

    /* 类型必须在当前配置构造的容器表中存在。 */
    information = rt_object_get_information(type);
    RT_ASSERT(information != RT_NULL);

#ifdef RT_DEBUGING_ASSERT
    /* 调试构建下扫描登记链，尽早发现对同一对象地址重复初始化。 */

    /* 锁住该类型全局链；模块私有链的重复地址不会被这段扫描覆盖。 */
    level = rt_spin_lock_irqsave(&(information->spinlock));
    /* 若发现完全相同的对象地址，断言失败而不是重复插入链表节点。 */
    for (node  = information->object_list.next;
            node != &(information->object_list);
            node  = node->next)
    {
        struct rt_object *obj;

        obj = rt_list_entry(node, struct rt_object, list);
        RT_ASSERT(obj != object);
    }
    /* 恢复本地中断状态并释放容器锁。 */
    rt_spin_unlock_irqrestore(&(information->spinlock), level);
#endif /* RT_DEBUGING_ASSERT */

    /* 初始化通用对象头；Static 位表示内存不归对象系统释放。 */
    object->type = type | RT_Object_Class_Static;
#if RT_NAME_MAX > 0
    if (name)
    {
        obj_name_len = rt_strlen(name);
        if(obj_name_len > RT_NAME_MAX - 1)
        {
            LOG_E("Object name %s exceeds RT_NAME_MAX=%d, consider increasing RT_NAME_MAX.", name, RT_NAME_MAX);
        }
        rt_strncpy(object->name, name, RT_NAME_MAX - 1);
        object->name[RT_NAME_MAX - 1] = '\0';
    }
    else
    {
        object->name[0] = '\0';
    }
#else
    object->name = name;
#endif

    /* 此时字段可读但对象尚未上链，且未持有 information->spinlock。 */
    RT_OBJECT_HOOK_CALL(rt_object_attach_hook, (object));

    level = rt_spin_lock_irqsave(&(information->spinlock));

#ifdef RT_USING_MODULE
    if (module)
    {
        rt_list_insert_after(&(module->object_list), &(object->list));
        object->module_id = (void *)module;
    }
    else
#endif /* RT_USING_MODULE */
    {
        /* 非模块对象进入该类型的全局双向循环链表。 */
        rt_list_insert_after(&(information->object_list), &(object->list));
    }
    rt_spin_unlock_irqrestore(&(information->spinlock), level);
}

/**
 * @brief 从对象系统注销静态对象，但不释放调用者提供的存储。
 *
 * detach hook 在对象仍有原类型、仍位于链表且未持容器锁时执行。随后函数锁住
 * 类型容器，从对象当前所在链（全局链或模块私有链）移除节点，最后把类型设为
 * Null。调用者必须先完成具体对象的停止/唤醒等待者等清理；本函数只处理通用头。
 *
 * @param object 要注销的静态对象。
 */
void rt_object_detach(rt_object_t object)
{
    rt_base_t level;
    struct rt_object_information *information;

    /* 具体上层 detach API 负责确保它确实是静态对象。 */
    RT_ASSERT(object != RT_NULL);

    RT_OBJECT_HOOK_CALL(rt_object_detach_hook, (object));

    information = rt_object_get_information((enum rt_object_class_type)object->type);
    RT_ASSERT(information != RT_NULL);

    level = rt_spin_lock_irqsave(&(information->spinlock));
    /* `object->list` 自身携带前后链接，所以模块私有链也可在这里直接移除。 */
    rt_list_remove(&(object->list));
    rt_spin_unlock_irqrestore(&(information->spinlock), level);

    object->type = RT_Object_Class_Null;
}

#ifdef RT_USING_HEAP
/**
 * @brief 从内核堆分配、清零并登记一个动态内核对象。
 *
 * 分配大小来自对应容器的 `object_size`，因此调用者不必自行计算各配置下结构体
 * 尺寸。内存清零后设置动态类型和名称，在未持容器锁时调用 attach hook，最后
 * 插入当前模块私有链或全局类型链。本函数只能在线程上下文调用，因为堆分配
 * 以及后续使用不适合中断上下文。
 *
 * @param type 具体公开对象类型，不得带 `RT_Object_Class_Static`。
 *
 * @param name 可选名称。固定数组配置下复制并截断；指针名称配置下只保存指针，
 *             调用者负责其生命周期。名称不强制唯一。
 *
 * @return 成功返回已登记且清零的对象；堆内存不足返回 `RT_NULL`。
 */
rt_object_t rt_object_allocate(enum rt_object_class_type type, const char *name)
{
    struct rt_object *object;
    rt_base_t level;
    rt_size_t obj_name_len;
    struct rt_object_information *information;
#ifdef RT_USING_MODULE
    struct rt_dlmodule *module = dlmodule_self();
#endif /* RT_USING_MODULE */

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* 容器提供该类对象在当前编译配置下的准确大小。 */
    information = rt_object_get_information(type);
    RT_ASSERT(information != RT_NULL);

    object = (struct rt_object *)RT_KERNEL_MALLOC(information->object_size);
    if (object == RT_NULL)
    {
        /* 分配失败不创建半初始化对象，也不会调用 attach hook。 */
        return RT_NULL;
    }

    /* 私有字段先清零，之后由具体类型的 create API 继续初始化。 */
    rt_memset(object, 0x0, information->object_size);

    /* 动态对象不设置 Static 位，delete 会据此允许释放。 */
    object->type = type;

    /* 通用标志的默认值为 0，具体子系统随后可覆盖。 */
    object->flag = 0;

#if RT_NAME_MAX > 0
    if (name)
    {
        obj_name_len = rt_strlen(name);
        if(obj_name_len > RT_NAME_MAX - 1)
        {
            LOG_E("Object name %s exceeds RT_NAME_MAX=%d, consider increasing RT_NAME_MAX.", name, RT_NAME_MAX);
        }
        rt_strncpy(object->name, name, RT_NAME_MAX - 1);
        object->name[RT_NAME_MAX - 1] = '\0';
    }
    else
    {
        object->name[0] = '\0';
    }
#else
    object->name = name;
#endif

    /* hook 看到完整通用头，但对象尚未出现在任何登记链中。 */
    RT_OBJECT_HOOK_CALL(rt_object_attach_hook, (object));

    level = rt_spin_lock_irqsave(&(information->spinlock));

#ifdef RT_USING_MODULE
    if (module)
    {
        rt_list_insert_after(&(module->object_list), &(object->list));
        object->module_id = (void *)module;
    }
    else
#endif /* RT_USING_MODULE */
    {
        /* 没有当前模块时，登记到相应类型的全局对象链。 */
        rt_list_insert_after(&(information->object_list), &(object->list));
    }
    rt_spin_unlock_irqrestore(&(information->spinlock), level);

    return object;
}

/**
 * @brief 注销并释放一个由 `rt_object_allocate()` 创建的动态对象。
 *
 * detach hook 在未持容器锁、对象尚在链中且类型仍有效时调用。之后加锁移除
 * 链表节点，解锁后把类型改为 Null，最终归还内核堆。函数不认识各类型私有
 * 资源，所以必须由具体的 delete API 先停止对象并唤醒等待者。
 *
 * @param object 要删除的动态对象；静态对象会触发断言，必须改用 detach。
 */
void rt_object_delete(rt_object_t object)
{
    rt_base_t level;
    struct rt_object_information *information;

    /* 明确拒绝静态对象，防止释放栈、全局区或调用者管理的内存。 */
    RT_ASSERT(object != RT_NULL);
    RT_ASSERT(!(object->type & RT_Object_Class_Static));

    RT_OBJECT_HOOK_CALL(rt_object_detach_hook, (object));


    information = rt_object_get_information((enum rt_object_class_type)object->type);
    RT_ASSERT(information != RT_NULL);

    level = rt_spin_lock_irqsave(&(information->spinlock));

    /* 可从全局链或模块私有链直接摘除该节点。 */
    rt_list_remove(&(object->list));

    rt_spin_unlock_irqrestore(&(information->spinlock), level);

    /* 在释放前清除类型，便于调试阶段发现悬空对象的错误使用。 */
    object->type = RT_Object_Class_Null;

    /* 此后 object 指针失效，任何观察者都不能再解引用。 */
    RT_KERNEL_FREE(object);
}
#endif /* RT_USING_HEAP */

/**
 * @brief 判断对象是否采用静态存储生命周期。
 *
 * 这里的“system object”仅表示类型字段带 `RT_Object_Class_Static`，也就是对象
 * 存储不由内核对象分配器释放；它不表示对象一定由内核自身创建。
 *
 * @param object 要检查的非空对象。
 *
 * @return 带 Static 位返回 `RT_TRUE`，动态对象返回 `RT_FALSE`。
 */
rt_bool_t rt_object_is_systemobject(rt_object_t object)
{
    /* 空指针属于编程错误，由断言处理。 */
    RT_ASSERT(object != RT_NULL);

    if (object->type & RT_Object_Class_Static)
        return RT_TRUE;

    return RT_FALSE;
}

/**
 * @brief 返回去除 `RT_Object_Class_Static` 标志后的实际对象类。
 *
 * @param object 要查询的非空对象。
 *
 * @return 线程、信号量、定时器等基础类型值；不会包含 Static 位。
 */
rt_uint8_t rt_object_get_type(rt_object_t object)
{
    /* 空指针属于编程错误，由断言处理。 */
    RT_ASSERT(object != RT_NULL);

    return object->type & ~RT_Object_Class_Static;
}

/**
 * @brief 在指定类型的全局对象链上逐个调用迭代回调。
 *
 * 整个遍历期间一直持有类型容器的自旋锁并关闭本地中断，保证链表不会在迭代
 * 中改变。这也意味着 `iter` 必须非常短小、不得阻塞、不得删除/创建同类型
 * 对象，也不得递归调用本函数或 `rt_object_find()`，否则可能死锁。模块私有链
 * 不在遍历范围内。
 *
 * 回调返回 `RT_EOK` 时继续；返回正数表示“正常提前结束”，对外转换为
 * `RT_EOK`；返回负数表示错误并原样返回。这一约定供按名查找快速停止扫描。
 *
 * @param type 要遍历的公开对象类型。
 * @param iter 每个对象调用一次的非空回调。
 * @param data 原样传入回调的用户上下文。
 * @return 完整遍历或正常提前结束返回 `RT_EOK`；类型无效返回 `-RT_EINVAL`；
 *         回调的负错误值原样返回。
 * @note 只能在线程上下文调用；调试配置下会检查当前不在中断中。
 */
rt_err_t rt_object_for_each(rt_uint8_t type, rt_object_iter_t iter, void *data)
{
    struct rt_object *object = RT_NULL;
    struct rt_list_node *node = RT_NULL;
    struct rt_object_information *information = RT_NULL;
    rt_base_t level;
    rt_err_t error;

    information = rt_object_get_information((enum rt_object_class_type)type);

    /* 配置中不存在的类型没有容器可遍历。 */
    if (information == RT_NULL)
    {
        return -RT_EINVAL;
    }

    /* 迭代回调是用户代码，明确禁止从中断上下文进入。 */
    RT_DEBUG_NOT_IN_INTERRUPT;

    /* 锁跨越所有回调调用，回调必须遵守上述不可重入约束。 */
    level = rt_spin_lock_irqsave(&(information->spinlock));

    /* 依次调用回调，允许其用正数正常提前结束或用负数报告错误。 */
    rt_list_for_each(node, &(information->object_list))
    {
        object = rt_list_entry(node, struct rt_object, list);
        if ((error = iter(object, data)) != RT_EOK)
        {
            rt_spin_unlock_irqrestore(&(information->spinlock), level);

            return error >= 0 ? RT_EOK : error;
        }
    }

    rt_spin_unlock_irqrestore(&(information->spinlock), level);

    return RT_EOK;
}

struct _obj_find_param
{
    const char *match_name; /**< 调用者希望查找的名称。 */
    rt_object_t matched_obj; /**< 命中时保存对象，否则保持 `RT_NULL`。 */
};

/**
 * @brief `rt_object_find()` 使用的内部迭代回调。
 *
 * 对象名称存储空间有限，因此先按与对象登记相同的规则截断输入名，再进行完整
 * 字符串比较。命中后返回正数 1，利用 `rt_object_for_each()` 的“正常提前结束”
 * 约定停止扫描。调用时仍持有类型容器锁。
 */
static rt_err_t _match_name(struct rt_object *obj, void *data)
{
    struct _obj_find_param *param = data;
    const char *name = param->match_name;
    char truncated_name[RT_NAME_MAX];

    /* 把查询字符串截断到对象内部实际可保存的长度。 */
    rt_strncpy(truncated_name, name, RT_NAME_MAX - 1);
    truncated_name[RT_NAME_MAX - 1] = '\0';

    if (rt_strcmp(obj->name, truncated_name) == 0)
    {
        param->matched_obj = obj;

        /* 正数只请求提前结束，不会被外层当作错误。 */
        return 1;
    }

    return RT_EOK;
}

/**
 * @brief 在指定类型的全局对象链中查找名称匹配的第一个对象。
 *
 * 查找通过 `rt_object_for_each()` 完成，所以比较期间持有类型容器锁并关闭本地
 * 中断。返回的是无引用计数保护的裸指针：函数返回后对象可能被其他线程删除，
 * 调用者需要依靠具体子系统的生命周期或额外同步保证其有效。重名时返回链表
 * 中先遇到的对象；模块私有链中的对象不会被找到。
 *
 * @param name 非空目标名称；会按 `RT_NAME_MAX - 1` 截断后比较。
 *
 * @param type 要搜索的公开对象类型。
 *
 * @return 命中的对象裸指针；参数无效或没有匹配项时返回 `RT_NULL`。
 * @note 只能在线程上下文调用。
 */
rt_object_t rt_object_find(const char *name, rt_uint8_t type)
{
    struct _obj_find_param param =
    {
        .match_name = name,
        .matched_obj = RT_NULL,
    };

    /* 在进入持锁遍历前拒绝空名称和不存在的类型。 */
    if (name == RT_NULL || rt_object_get_information(type) == RT_NULL)
        return RT_NULL;

    /* 与通用遍历接口一致，禁止中断上下文。 */
    RT_DEBUG_NOT_IN_INTERRUPT;

    rt_object_for_each(type, _match_name, &param);
    return param.matched_obj;
}

/**
 * @brief 把指定对象的名称复制到调用者缓冲区并保证以 `\0` 结尾。
 *
 * 本函数不获取对象容器锁，也不增加引用计数；调用者必须保证对象在复制期间
 * 存活。若目标缓冲区比名称短，结果会截断。
 *
 * @param object 名称来源对象。
 * @param name 接收 C 字符串的缓冲区。
 * @param name_size 缓冲区总字节数，至少为 1。
 *
 * @return 任一参数无效返回 `-RT_EINVAL`，复制成功返回 `RT_EOK`。
 */
rt_err_t rt_object_get_name(rt_object_t object, char *name, rt_uint8_t name_size)
{
    rt_err_t result = -RT_EINVAL;
    if ((object != RT_NULL) && (name != RT_NULL) && (name_size != 0U))
    {
        const char *obj_name = object->name;
        rt_strncpy(name, obj_name, (rt_size_t)name_size);
        /* 即使源名称过长，也强制让目标缓冲区成为合法 C 字符串。 */
        name[name_size - 1] = '\0';
        result = RT_EOK;
    }

    return result;
}

#ifdef RT_USING_HEAP
/**
 * @brief 创建一个携带任意用户数据和可选销毁回调的动态自定义对象。
 *
 * 通用对象由内核堆分配；`data` 指向的内容不复制，所有权约定由调用者决定。
 * 销毁时若 `data_destroy` 非空会先调用它，然后无论其返回值如何都删除容器。
 *
 * @param name 自定义对象名称。
 * @param data 与对象绑定、内核不解释的用户指针。
 * @param data_destroy 可选用户数据清理函数，接收 `data` 并返回状态。
 *
 * @return 成功返回自定义对象的通用句柄；分配失败返回 `RT_NULL`。
 * @note 依赖内核堆，只能在线程上下文调用。
 */

rt_object_t rt_custom_object_create(const char *name, void *data, rt_err_t (*data_destroy)(void *))
{
    struct rt_custom_object *cobj = RT_NULL;

    cobj = (struct rt_custom_object *)rt_object_allocate(RT_Object_Class_Custom, name);
    if (!cobj)
    {
        return RT_NULL;
    }
    cobj->destroy = data_destroy;
    cobj->data = data;
    return (struct rt_object *)cobj;
}

/**
 * @brief 调用用户清理函数，然后删除动态自定义对象容器。
 *
 * 只有类型精确等于动态 `RT_Object_Class_Custom` 才执行销毁；静态位或其他类型
 * 都会返回默认错误。清理回调返回错误也不会阻止对象容器被删除，因此调用者
 * 在函数返回后不能继续使用 `obj`。
 *
 * @param obj 要销毁的自定义对象。
 * @return 有清理回调时返回其状态；无回调、空指针或类型错误时返回 -1。
 * @note 释放堆内存，只能在线程上下文调用。
 */
rt_err_t rt_custom_object_destroy(rt_object_t obj)
{
    rt_err_t ret = -1;

    struct rt_custom_object *cobj = (struct rt_custom_object *)obj;

    if (obj && obj->type == RT_Object_Class_Custom)
    {
        if (cobj->destroy)
        {
            ret = cobj->destroy(cobj->data);
        }
        rt_object_delete(obj);
    }
    return ret;
}
#endif

/** @} group_object_management */
