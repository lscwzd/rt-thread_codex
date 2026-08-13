/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 修改记录：
 * 日期           作者         说明
 * 2024-12-25     Meco Man     首个版本
 */

/**
 * @file TC_rt_memmove.c
 * @brief 验证 rt_memmove() 的普通复制、双向重叠、零长度和空字符串行为。
 *
 * memmove 与 memcpy 的关键区别是重叠安全：目标落在源后方时必须从尾部向前复制，
 * 目标在源前方时可从头向后复制。零长度用例还验证实现不会解引用源/目标地址。
 */

#include <rtthread.h>
#include <utest.h>

/* 基本非重叠移动，并把字符串终止符一起复制。 */
static void TC_rt_memmove_basic(void)
{
    char src[] = "Hello";
    char dest[10] = {0};
    rt_memmove(dest, src, rt_strlen(src) + 1);
    uassert_str_equal(dest, "Hello");
}

/* 源在前、目标落入源区：必须逆向复制，不能覆盖尚未读取的源字节。 */
static void TC_rt_memmove_overlap_src_before(void)
{
    char buffer[] = "1234567890";
    rt_memmove(&buffer[3], buffer, 5);
    uassert_str_equal(buffer, "1231234590");
}

/* 源在后、目标在前：正向复制即可安全得到期望结果。 */
static void TC_rt_memmove_overlap_src_after(void)
{
    char buffer[] = "1234567890";
    rt_memmove(&buffer[2], &buffer[5], 5);
    uassert_str_equal(buffer, "1267890890");
}

/* 长度为 0 时目标内容保持不变。 */
static void TC_rt_memmove_zero_length(void)
{
    char src[] = "Hello";
    char dest[10] = "World";
    rt_memmove(dest, src, 0);
    uassert_str_equal(dest, "World");
}

/* 源和目标完全相同应成为无副作用操作。 */
static void TC_rt_memmove_same_location(void)
{
    char buffer[] = "Hello";
    rt_memmove(buffer, buffer, rt_strlen(buffer) + 1);
    uassert_str_equal(buffer, "Hello");
}

/* 长度为 0 时允许传入空源，因为循环不会解引用它。 */
static void TC_rt_memmove_null_src(void)
{
    char dest[10];
    rt_memset(dest, 'A', sizeof(dest));
    rt_memmove(dest, RT_NULL, 0); /* 不应崩溃，也不应修改目标。 */
    uassert_buf_equal(dest, "AAAAAAAAAA", 10);
}

/* 长度为 0 时同样不解引用空目标。 */
static void TC_rt_memmove_null_dest(void)
{
    char src[] = "Hello";
    rt_memmove(RT_NULL, src, 0); /* 不应崩溃，也不执行写入。 */
}

/*
 * 此历史用例请求长度超过 src 数组：memmove 会严格复制给定长度，不知道字符串
 * 边界，因此该调用在 C 语义上会读取 src 之外，属于未定义行为。它不能证明函数
 * “只复制到源字符串末尾”；保留该用例仅用于记录当前测试集合的既有覆盖。
 */
static void TC_rt_memmove_too_long(void)
{
    char src[] = "Short";
    char dest[10] = {0};
    rt_memmove(dest, src, sizeof(src) + 5); /* 被测函数仍会尝试复制完整请求长度。 */
    uassert_str_equal(dest, "Short");
    uassert_int_equal(dest[5], 0); /* 只检查终止符位置，不能证明源/目标均未越界。 */
}

/* 空字符串只需移动一个 '\0'，目标随即成为空 C 字符串。 */
static void TC_rt_memmove_empty_string(void)
{
    char src[] = "";
    char dest[10] = "Unchanged";
    rt_memmove(dest, src, rt_strlen(src) + 1);

    /* 目标首字符应为 '\0'；其后的旧内容不影响字符串比较。 */
    uassert_str_equal(dest, "");  /* 目标现在表示空字符串。 */
    uassert_int_equal(dest[0], '\0'); /* 显式验证首字符就是终止符。 */
}

/* 按固定顺序运行全部子用例，便于定位是哪一类地址关系失败。 */
static void utest_do_tc(void)
{
    UTEST_UNIT_RUN(TC_rt_memmove_basic);
    UTEST_UNIT_RUN(TC_rt_memmove_overlap_src_before);
    UTEST_UNIT_RUN(TC_rt_memmove_overlap_src_after);
    UTEST_UNIT_RUN(TC_rt_memmove_zero_length);
    UTEST_UNIT_RUN(TC_rt_memmove_same_location);
    UTEST_UNIT_RUN(TC_rt_memmove_null_src);
    UTEST_UNIT_RUN(TC_rt_memmove_null_dest);
    UTEST_UNIT_RUN(TC_rt_memmove_too_long);
    UTEST_UNIT_RUN(TC_rt_memmove_empty_string);
}

UTEST_TC_EXPORT(utest_do_tc, "core.klibc.rt_memmove", RT_NULL, RT_NULL, 1000);
