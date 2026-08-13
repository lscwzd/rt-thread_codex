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
 * @file TC_rt_memcmp.c
 * @brief 验证 rt_memcmp() 的逐字节相等性、首差顺序和长度边界。
 *
 * rt_memcmp() 不理解 int、float 或结构体，它只把对象表示看作 unsigned char
 * 序列。本组测试用字符串、数组和结构体覆盖不同长度，并验证只比较指定的 count
 * 个字节。注意：对整数/浮点/结构体原始表示的“大小关系”具有端序、浮点编码和
 * 填充字节依赖；这类用例适合当前目标上的实现回归，不代表可移植的数值排序规则。
 */

#include <rtklibc.h>
#include <utest.h>

static void TC_rt_memcmp_str(void)
{
    const char* s = "abc 123";

    uassert_int_equal(rt_memcmp("abc", "abc", 4), 0);
    uassert_int_equal(rt_memcmp(s, "abc", 3), 0);
    uassert_int_equal(rt_memcmp("abc", s, 3), 0);

    /* 故意比较超过共同前缀的长度，确认第 4 个字符差异决定正负结果。 */
    uassert_value_greater(rt_memcmp(s, "abc", 6), 0);
    uassert_value_less(rt_memcmp("abc", s, 6), 0);
}

static void TC_rt_memcmp_int_array(void)
{
    /* 完全相同数组应相等；末元素不同应在其对象表示的首差字节处分出次序。 */
    int arr1[] = {1, 2, 3, 4, 5};
    int arr2[] = {1, 2, 3, 4, 5};
    int arr3[] = {1, 2, 3, 4, 6};

    uassert_int_equal(rt_memcmp(arr1, arr2, sizeof(arr1)), 0);
    uassert_value_less(rt_memcmp(arr1, arr3, sizeof(arr1)), 0);
    uassert_value_greater(rt_memcmp(arr3, arr1, sizeof(arr1)), 0);
}

static void TC_rt_memcmp_float_array(void)
{
    /* 测的是浮点数组的位表示，不是容差浮点比较；NaN/不同零表示不在本例范围。 */
    float arr1[] = {1.0f, 2.0f, 3.0f};
    float arr2[] = {1.0f, 2.0f, 3.0f};
    float arr3[] = {1.0f, 2.0f, 3.1f};

    uassert_int_equal(rt_memcmp(arr1, arr2, sizeof(arr1)), 0);
    uassert_value_less(rt_memcmp(arr1, arr3, sizeof(arr1)), 0);
    uassert_value_greater(rt_memcmp(arr3, arr1, sizeof(arr1)), 0);
}

typedef struct {
    int id;
    float value;
} Item;

static void TC_rt_memcmp_struct_array(void)
{
    /* 简单结构可能含填充；聚合初始化通常清晰，但跨 ABI 不应依赖比较值的正负号。 */
    Item arr1[] = {{1, 1.0f}, {2, 2.0f}};
    Item arr2[] = {{1, 1.0f}, {2, 2.0f}};
    Item arr3[] = {{1, 1.0f}, {2, 2.1f}};

    uassert_int_equal(rt_memcmp(arr1, arr2, sizeof(arr1)), 0);
    uassert_value_less(rt_memcmp(arr1, arr3, sizeof(arr1)), 0);
    uassert_value_greater(rt_memcmp(arr3, arr1, sizeof(arr1)), 0);
}

typedef struct {
    int id;
    float value;
    char name[10];
} MixedItem;

static void TC_rt_memcmp_mixed_array(void)
{
    /* 混合字段使对齐/填充更明显，本例验证当前构建下相同对象表示能被识别。 */
    MixedItem arr1[] = {{1, 1.0f, "item1"}, {2, 2.0f, "item2"}};
    MixedItem arr2[] = {{1, 1.0f, "item1"}, {2, 2.0f, "item2"}};
    MixedItem arr3[] = {{1, 1.0f, "item1"}, {2, 2.1f, "item2"}};

    uassert_int_equal(rt_memcmp(arr1, arr2, sizeof(arr1)), 0);
    uassert_value_less(rt_memcmp(arr1, arr3, sizeof(arr1)), 0);
    uassert_value_greater(rt_memcmp(arr3, arr1, sizeof(arr1)), 0);
}

typedef struct {
    int id;
    float score;
} Student;

typedef struct {
    Student students[3];
    char className[10];
} Class;

static void TC_rt_memcmp_nested_struct_array(void)
{
    /* 嵌套结构整体相等时结果为 0；任一成员改变后只要求结果非 0。 */
    Class class1 = {
        .students = {{1, 90.5}, {2, 85.0}, {3, 92.0}},
        .className = "ClassA"
    };

    Class class2 = {
        .students = {{1, 90.5}, {2, 85.0}, {3, 92.0}},
        .className = "ClassA"
    };

    Class class3 = {
        .students = {{1, 90.5}, {2, 85.1}, {3, 92.0}},
        .className = "ClassA"
    };

    uassert_int_equal(rt_memcmp(&class1, &class2, sizeof(Class)), 0);
    uassert_int_not_equal(rt_memcmp(&class1, &class3, sizeof(Class)), 0);
}

static void TC_rt_memcmp_partial_match(void)
{
    /* 差异位于第 14 字节：比较前 13 字节相等，比较完整数组则必须不等。 */
    char arr1[] = "abcdefghijklmnopqrstuvwxyz";
    char arr2[] = "abcdefghijklmxyznopqrstuvw";

    uassert_int_equal(rt_memcmp(arr1, arr2, 13), 0);
    uassert_int_not_equal(rt_memcmp(arr1, arr2, sizeof(arr1)), 0);
}

#define LARGE_ARRAY_SIZE 500

static void TC_rt_memcmp_large_array(void)
{
    /* 大块堆数组覆盖优化实现的机器字批处理循环，并在末元素制造差异。 */
    int *arr1 = rt_calloc(LARGE_ARRAY_SIZE, sizeof(int));
    int *arr2 = rt_calloc(LARGE_ARRAY_SIZE, sizeof(int));

    uassert_not_null(arr1);
    uassert_not_null(arr2);

    for (int i = 0; i < LARGE_ARRAY_SIZE; i++) {
        arr1[i] = i;
        arr2[i] = i;
    }

    uassert_int_equal(rt_memcmp(arr1, arr2, LARGE_ARRAY_SIZE * sizeof(int)), 0);
    arr2[LARGE_ARRAY_SIZE - 1] = LARGE_ARRAY_SIZE;

    uassert_value_less(rt_memcmp(arr1, arr2, LARGE_ARRAY_SIZE * sizeof(int)), 0);
    uassert_value_greater(rt_memcmp(arr2, arr1, LARGE_ARRAY_SIZE * sizeof(int)), 0);

    rt_free(arr1);
    rt_free(arr2);
}

static void utest_do_tc(void)
{
    /* UTEST_UNIT_RUN 会独立记录每个子用例的断言和失败位置。 */
    UTEST_UNIT_RUN(TC_rt_memcmp_str);
    UTEST_UNIT_RUN(TC_rt_memcmp_int_array);
    UTEST_UNIT_RUN(TC_rt_memcmp_float_array);
    UTEST_UNIT_RUN(TC_rt_memcmp_struct_array);
    UTEST_UNIT_RUN(TC_rt_memcmp_mixed_array);
    UTEST_UNIT_RUN(TC_rt_memcmp_nested_struct_array);
    UTEST_UNIT_RUN(TC_rt_memcmp_partial_match);
    UTEST_UNIT_RUN(TC_rt_memcmp_large_array);
}

UTEST_TC_EXPORT(utest_do_tc, "core.klibc.rt_memcmp", RT_NULL, RT_NULL, 1000);
