/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 修改记录：
 * 日期           作者               说明
 * 2020-05-06     Phillip Johnston   首个版本
 * 2024-12-24     Meco Man           移植到 Utest
 */

/**
 * @file TC_rt_memset.c
 * @brief 验证 rt_memset() 的地址对齐、长度边界、返回值和填充值截断行为。
 *
 * 两个堆缓冲区分别保存实际结果和期望结果。test_align() 在待填充窗口两侧保留
 * 64 字节哨兵区，完整比较可发现实现为了机器字优化而误写头尾。测试长度会跨越
 * 常见字长边界，以覆盖逐字节和批量写两条路径。
 */

#include <rtthread.h>
#include <utest.h>

#define TEST_BUF_SIZE 400

static char *buf;
static char *buf2;

/** 为实际缓冲区和期望缓冲区分别申请固定大小空间。 */
static rt_err_t utest_tc_init(void)
{
    buf = rt_malloc(TEST_BUF_SIZE * sizeof(char));
    uassert_not_null(buf);
    buf2 = rt_malloc(TEST_BUF_SIZE * sizeof(char));
    uassert_not_null(buf2);
    return RT_EOK;
}

/** 释放两个共享缓冲区；它们在所有子用例之间复用。 */
static rt_err_t utest_tc_cleanup(void)
{
    rt_free(buf);
    rt_free(buf2);
    return RT_EOK;
}

static void test_align(int align, size_t len)
{
    /* 在两个缓冲区中选取同样偏移，建立“仅窗口内为目标值”的期望图。 */
    char *s = (char *)RT_ALIGN(((rt_ubase_t)buf + 64), 64) + align;
    char *want = (char *)RT_ALIGN(((rt_ubase_t)buf2 + 64), 64) + align;
    char *p;
    int i;

    uassert_false(len + 64 > (size_t)(buf + TEST_BUF_SIZE - s));
    uassert_false(len + 64 > (size_t)(buf2 + TEST_BUF_SIZE - want));

    for(i = 0; i < TEST_BUF_SIZE; i++)
    {
        buf[i] = buf2[i] = ' ';
    }

    for(i = 0; i < (int)len; i++)
    {
        want[i] = '#';
    }

    p = rt_memset(s, '#', len);

    uassert_ptr_equal(p, s);

    for(i = -64; i < (int)len + 64; i++)
    {
        uassert_int_equal(s[i], want[i]);
    }
}

static void TC_rt_memcpy_align(void)
{
    /* 历史函数名保留了 memcpy 字样，实际被测对象是 rt_memset()。 */
    for(int i = 0; i < 16; i++)
    {
        for(size_t j = 0; j < 200; j++)
        {
            test_align(i, j);
        }
    }
}

static void test_input(char c)
{
    /* 验证不同 char 值能逐字符写入前 10 个位置。 */
    rt_memset(buf, c, 10);
    for(int i = 0; i < 10; i++)
    {
        uassert_int_equal(buf[i], c);
    }
}

static void TC_rt_memcpy_input(void)
{
    /* 覆盖零、正值和最高位为 1 的字符值；函数名同样是历史遗留。 */
    test_input('c');
    test_input(0);
    test_input(-1);
    test_input(0xab);
    test_input((char)RT_UINT32_MAX);
    test_input((char)-RT_UINT32_MAX);
}

static void utest_do_tc(void)
{
    UTEST_UNIT_RUN(TC_rt_memcpy_align);
    UTEST_UNIT_RUN(TC_rt_memcpy_input);
}

UTEST_TC_EXPORT(utest_do_tc, "core.klibc.rt_memset", utest_tc_init, utest_tc_cleanup, 1000);
