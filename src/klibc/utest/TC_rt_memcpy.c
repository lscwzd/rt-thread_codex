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
 * @file TC_rt_memcpy.c
 * @brief 验证 rt_memcpy() 在各种源/目标对齐和长度下的复制结果。
 *
 * 512 字节夹具在每个测试套件开始时分配，划分为源、实际目标和期望目标三个区域。
 * test_align() 在三个区域之外保留哨兵填充值，所以既能验证请求区内容，也能发现
 * 前后边界被误写。测试只覆盖不重叠复制；重叠场景属于 rt_memmove() 的职责。
 */

#include <rtthread.h>
#include <utest.h>

#define N 80 /**< Define the constant N for buffer size as 80 */
#define TEST_BUF_SIZE 512 /**< Define the constant TEST_BUF_SIZE as 512 */
static char *buf; /**< 三个测试区域共用的 512 字节堆缓冲区。 */

/** 测试套件初始化：申请共享缓冲区；断言保证后续用例不会解引用空指针。 */
static rt_err_t utest_tc_init(void)
{
    buf = rt_malloc(TEST_BUF_SIZE * sizeof(char)); /**< Allocate memory for the buffer */
    uassert_not_null(buf);
    return RT_EOK;
}

/** 测试套件清理：所有子用例运行结束后释放共享缓冲区。 */
static rt_err_t utest_tc_cleanup(void)
{
    rt_free(buf);
    return RT_EOK;
}

/**
 * @brief 以指定源/目标偏移和长度执行一次复制校验。
 * @param dalign 目标区相对其基址的对齐偏移。
 * @param salign 源区相对其基址的对齐偏移。
 * @param len 复制长度。
 */
static void test_align(unsigned dalign, unsigned salign, size_t len)
{
    char *src = (char *)RT_ALIGN((rt_ubase_t)buf, 64); /**< Source buffer starting address, 64-byte aligned */
    char *dst = (char *)RT_ALIGN(((rt_ubase_t)buf + 128), 64); /**< Destination buffer starting address, 64-byte aligned from buf+128 */
    char *want = (char *)RT_ALIGN(((rt_ubase_t)buf + 256), 64); /**< Expected result buffer starting address, 64-byte aligned from buf+256 */
    char *p; /**< Pointer to receive the return value of rt_memcpy */
    unsigned i;

    /** 源偏移加长度不得越过源测试区。 */
    uassert_false(salign + len > N);
    /** 目标偏移加长度不得越过目标测试区。 */
    uassert_false(dalign + len > N);

    /** 用不同哨兵字符初始化实际目标、源和期望目标。 */
    for(i = 0; i < N; i++)
    {
        src[i] = '#';
        dst[i] = want[i] = ' ';
    }

    /** 在源与期望区相同窗口写入测试字节，建立逐字节真值。 */
    for(i = 0; i < len; i++)
    {
        src[salign + i] = want[dalign + i] = (char)('0' + i);
    }

    /** 调用被测函数，只复制选定窗口。 */
    p = rt_memcpy(dst + dalign, src + salign, len);

    /** memcpy 必须原样返回目标窗口起始指针。 */
    uassert_ptr_equal(p, dst + dalign);

    /** 比较完整目标区域，从而同时发现窗口内部错误和窗口外越界写。 */
    for(i = 0; i < N; i++)
    {
        uassert_int_equal(dst[i], want[i]);
    }
}

/**
 * @brief 遍历常见对齐偏移，并覆盖从零长度到跨越多个机器字的长度组合。
 */
static void TC_rt_memcpy_align(void)
{
    for(unsigned i = 0; i < 16; i++) /**< Iterate over source alignment offsets from 0 to 15 */
    {
        for(unsigned j = 0; j < 16; j++) /**< Iterate over destination alignment offsets from 0 to 15 */
        {
            for(size_t k = 0; k < 64; k++) /**< Iterate over data lengths from 0 to 63 */
            {
                test_align(i, j, k); /**< Call the test_align function */
            }
        }
    }
}

static void TC_rt_memcpy_str(void)
{
    /* 连同 '\0' 复制普通字符串，验证结果可立即作为 C 字符串使用。 */
    const char src[] = "Hello, memcpy!";
    char dest[20] = {0};
    rt_memcpy(dest, src, sizeof(src));
    uassert_true(rt_strcmp(src, dest) == 0);
}

static void utest_do_tc(void)
{
    UTEST_UNIT_RUN(TC_rt_memcpy_str);
    UTEST_UNIT_RUN(TC_rt_memcpy_align);
}

UTEST_TC_EXPORT(utest_do_tc, "core.klibc.rt_memcpy", utest_tc_init, utest_tc_cleanup, 1000);
