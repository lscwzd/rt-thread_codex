/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2025-09-03     Rbb666         the first version for mempool utest
 * 2025-11-30     westcity-YOLO  Add standardized utest documentation block
 */

/**
 * @file mempool_tc.c
 * @brief 验证固定块内存池的静态/动态生命周期、容量边界和重复使用稳定性。
 *
 * 内存池把一段连续存储切成等长块，分配/释放时间可预测，适合实时系统。本测试用
 * 80 字节有效载荷、32 个块建立静态池；MEMPOOL_SIZE 还为每块预留内核自由链指针
 * 的开销。动态池由 rt_mp_create() 同样建立 32 个块。
 *
 * 七个子场景依次检查元数据、三块分配/归还、耗尽后的非阻塞失败、释放 RT_NULL
 * 的幂等性，以及 100 轮“全部取出再全部归还”。block_free_count 的断言同时验证
 * 自由链计数没有泄漏。测试项为 `core.mempool`；这里没有测等待者阻塞/唤醒时序。
 */

#include <rtthread.h>
#include <stdlib.h>
#include "utest.h"

#define MEMPOOL_BLOCK_SIZE  80
#define MEMPOOL_BLOCK_COUNT 32
#define MEMPOOL_SIZE        (MEMPOOL_BLOCK_SIZE + sizeof(rt_uint8_t *)) * MEMPOOL_BLOCK_COUNT

/* 静态池的后备存储，生命周期覆盖整个测试套件。 */
static rt_uint8_t        mempool_static[MEMPOOL_SIZE];
/* 静态池控制块：detach 只注销对象，不释放上述数组。 */
static struct rt_mempool mp_static;
/* 动态池句柄：控制块和后备存储都由 create/delete 管理。 */
static rt_mp_t           mp_dynamic;

/** 初始化静态池，并核对名称、总块数和初始自由块数。 */
static void test_mp_static_init(void)
{
    rt_err_t err;

    err = rt_mp_init(&mp_static, "mp_static", &mempool_static[0], sizeof(mempool_static), MEMPOOL_BLOCK_SIZE);
    uassert_true(err == RT_EOK);
    uassert_str_equal(mp_static.parent.name, "mp_static");
    uassert_true(mp_static.block_total_count == MEMPOOL_BLOCK_COUNT);
    uassert_true(mp_static.block_free_count == MEMPOOL_BLOCK_COUNT);
}

/** 创建动态池并验证 create 根据参数正确填写关键元数据。 */
static void test_mp_dynamic_create(void)
{
    mp_dynamic = rt_mp_create("mp_dynamic", MEMPOOL_BLOCK_COUNT, MEMPOOL_BLOCK_SIZE);
    uassert_not_null(mp_dynamic);
    uassert_str_equal(mp_dynamic->parent.name, "mp_dynamic");
    uassert_true(mp_dynamic->block_total_count == MEMPOOL_BLOCK_COUNT);
    uassert_true(mp_dynamic->block_free_count == MEMPOOL_BLOCK_COUNT);
}

/**
 * @brief 对静态池执行三次非阻塞分配和逆向资源恢复检查。
 *
 * timeout=0 表示池空时立即失败，本场景池未空，所以三个指针都必须非空。自由计数
 * 应从 32 降到 29，再在逐块 free 后恢复 32，证明节点能重新接回自由链。
 */
static void test_mp_static_alloc_free(void)
{
    void *block1, *block2, *block3;

    /* 连续取出三个互不重叠的固定块。 */
    block1 = rt_mp_alloc(&mp_static, 0);
    uassert_not_null(block1);

    block2 = rt_mp_alloc(&mp_static, 0);
    uassert_not_null(block2);

    block3 = rt_mp_alloc(&mp_static, 0);
    uassert_not_null(block3);

    /* 每次成功分配恰好消耗一个自由节点。 */
    uassert_true(mp_static.block_free_count == MEMPOOL_BLOCK_COUNT - 3);

    /* 归还顺序不影响最终容量恢复。 */
    rt_mp_free(block1);
    rt_mp_free(block2);
    rt_mp_free(block3);

    /* 所有块归还后不应出现计数泄漏。 */
    uassert_true(mp_static.block_free_count == MEMPOOL_BLOCK_COUNT);
}

/** 对动态池重复相同的三块分配/归还验证，区分对象所有权而不改变块算法。 */
static void test_mp_dynamic_alloc_free(void)
{
    void *block1, *block2, *block3;

    /* 动态控制块使用与静态池相同的非阻塞分配接口。 */
    block1 = rt_mp_alloc(mp_dynamic, 0);
    uassert_not_null(block1);

    block2 = rt_mp_alloc(mp_dynamic, 0);
    uassert_not_null(block2);

    block3 = rt_mp_alloc(mp_dynamic, 0);
    uassert_not_null(block3);

    /* 三次成功分配后剩余总数减三。 */
    uassert_true(mp_dynamic->block_free_count == MEMPOOL_BLOCK_COUNT - 3);

    /* rt_mp_free 从块头信息找到所属池，无需额外传池句柄。 */
    rt_mp_free(block1);
    rt_mp_free(block2);
    rt_mp_free(block3);

    /* 动态池容量完全恢复。 */
    uassert_true(mp_dynamic->block_free_count == MEMPOOL_BLOCK_COUNT);
}

/**
 * @brief 验证静态池完全耗尽时第 33 次非阻塞分配返回 RT_NULL。
 *
 * blocks 数组保存每个成功指针用于清理；即使额外分配正确失败，也必须归还前 32 块，
 * 避免影响后续压力测试。
 */
static void test_mp_boundary_alloc_exceed(void)
{
    void *blocks[MEMPOOL_BLOCK_COUNT];
    void *extra_block;
    int   i;

    /* 精确取出池声明的全部块。 */
    for (i = 0; i < MEMPOOL_BLOCK_COUNT; i++)
    {
        blocks[i] = rt_mp_alloc(&mp_static, 0);
        uassert_not_null(blocks[i]);
    }

    /* 元数据应明确报告池空。 */
    uassert_true(mp_static.block_free_count == 0);

    /* timeout=0 禁止阻塞，没有自由块时必须立即返回空指针。 */
    extra_block = rt_mp_alloc(&mp_static, 0);
    uassert_null(extra_block);

    /* 清理本场景持有的每一块。 */
    for (i = 0; i < MEMPOOL_BLOCK_COUNT; i++)
    {
        rt_mp_free(blocks[i]);
    }

    /* 为下一子测试恢复干净夹具。 */
    uassert_true(mp_static.block_free_count == MEMPOOL_BLOCK_COUNT);
}

/** 验证 rt_mp_free(RT_NULL) 是安全空操作，不改变池计数。 */
static void test_mp_boundary_free_invalid(void)
{
    /* API 应允许通用清理代码无条件释放可空指针。 */
    rt_mp_free(RT_NULL);

    /* 空操作不能凭空增加自由块。 */
    uassert_true(mp_static.block_free_count == MEMPOOL_BLOCK_COUNT);
}

/**
 * @brief 连续 100 轮耗尽/恢复静态池，暴露自由链断链或重复计数问题。
 *
 * 每轮所有分配都必须成功、池空计数必须为 0，全部归还后必须恢复 32。该压力测试
 * 检查确定性状态恢复，不把执行耗时作为断言指标。
 */
static void test_mp_stress_alloc_free(void)
{
    void *blocks[MEMPOOL_BLOCK_COUNT];
    int   i, j;

    for (j = 0; j < 100; j++) /* 重复 100 轮相同生命周期。 */
    {
        /* 本轮取空自由链。 */
        for (i = 0; i < MEMPOOL_BLOCK_COUNT; i++)
        {
            blocks[i] = rt_mp_alloc(&mp_static, 0);
            uassert_not_null(blocks[i]);
        }

        /* 全部块均由 blocks 数组持有。 */
        uassert_true(mp_static.block_free_count == 0);

        /* 逐一归还，下一轮会再次复用这些节点。 */
        for (i = 0; i < MEMPOOL_BLOCK_COUNT; i++)
        {
            rt_mp_free(blocks[i]);
        }

        /* 每轮都必须恢复初始不变量。 */
        uassert_true(mp_static.block_free_count == MEMPOOL_BLOCK_COUNT);
    }
}

static rt_err_t utest_tc_init(void)
{
    /* 对象由前两个子测试建立，套件级初始化无需额外动作。 */
    return RT_EOK;
}

static rt_err_t utest_tc_cleanup(void)
{
    /* 静态池只注销控制块，mempool_static 仍由本文件拥有。 */
    rt_mp_detach(&mp_static);

    /* 动态池存在时释放其控制块和后备存储。 */
    if (mp_dynamic != RT_NULL)
    {
        rt_mp_delete(mp_dynamic);
    }

    return RT_EOK;
}

static void testcase(void)
{
    /* 生命周期测试必须先创建对象，清理统一由 utest_tc_cleanup 完成。 */
    UTEST_UNIT_RUN(test_mp_static_init);
    UTEST_UNIT_RUN(test_mp_dynamic_create);
    UTEST_UNIT_RUN(test_mp_static_alloc_free);
    UTEST_UNIT_RUN(test_mp_dynamic_alloc_free);
    UTEST_UNIT_RUN(test_mp_boundary_alloc_exceed);
    UTEST_UNIT_RUN(test_mp_boundary_free_invalid);
    UTEST_UNIT_RUN(test_mp_stress_alloc_free);
}

UTEST_TC_EXPORT(testcase, "core.mempool", utest_tc_init, utest_tc_cleanup, 10);
