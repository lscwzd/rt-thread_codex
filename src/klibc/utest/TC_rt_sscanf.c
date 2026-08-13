/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 修改记录：
 * 日期           作者         说明
 * 2025-01-04     Meco Man     首个版本
 */

/**
 * @file TC_rt_sscanf.c
 * @brief 验证 rt_sscanf() 的字符、字符串、整数、浮点、指针和字符集合解析。
 *
 * 每个子用例构造一个固定输入串，检查返回的“成功赋值项目数”和目标变量内容。
 * 格式串中的普通空白、转换前自动跳白、字段宽度、`*` 抑制赋值及长度修饰符分别
 * 覆盖。测试目标数组都由调用者预先提供；真实代码使用 `%s`/`%[...]` 时还必须给
 * 出足够小的宽度以防缓冲区溢出。
 *
 * 内置 rt_vsscanf 与 libc 后端存在边界差异：空输入上的首次数字转换在本测试中
 * 期望 -1；另外当前内置实现的 `%*d` 路径存在工作指针未可靠初始化的缺口，因此
 * suppression 用例也承担暴露该回归的作用，不能把偶然通过视为接口安全保证。
 */

#include <rtklibc.h>
#include "utest.h"

static void TC_rt_sscanf_char(void)
{
    /* %c 不自动跳过输入空白；格式串中的字面空格负责越过两个字符之间的空白。 */
    const char str[] = "A B";
    char a, b;
    rt_sscanf(str, "%c %c", &a, &b);
    uassert_true(a == 'A' && b == 'B');
    /* 输入指针手工移动到空格后的 B，再验证单独一个 %c。 */
    rt_sscanf(str + 2, "%c", &b);
    uassert_true(b == 'B');
}

static void TC_rt_sscanf_basic_int(void)
{
    /* 十进制整数应完成一次赋值，并得到完整数值。 */
    const char str[] = "12345";
    int value;
    int result = rt_sscanf(str, "%d", &value);
    uassert_int_equal(result, 1);
    uassert_int_equal(value, 12345);
}

static void TC_rt_sscanf_basic_float(void)
{
    /* 无长度修饰符的 %f 目标是 float *。 */
    const char str[] = "123.45";
    float value;
    int result = rt_sscanf(str, "%f", &value);
    uassert_int_equal(result, 1);
    uassert_float_equal(value, 123.45);
}

static void TC_rt_sscanf_basic_string(void)
{
    /* %s 自动跳过前导空白，并在下一个空白前停止；逗号属于普通字符。 */
    const char str[] = "Hello, World!";
    char buffer[20];
    int result = rt_sscanf(str, "%s", buffer);
    uassert_int_equal(result, 1);
    uassert_str_equal(buffer, "Hello,");
}

static void TC_rt_sscanf_string_with_space(void)
{
    /* `%*s` 消耗第一个单词但不赋值，格式空白再吞掉任意多个空格。 */
    const char str[] = "Hello   World";
    char a[20];
    rt_sscanf(str, "%*s %s", a);
    uassert_str_equal(a, "World");
}

static void TC_rt_sscanf_basic_char(void)
{
    /* 单字符输入返回一次赋值，目标不会被自动追加字符串终止符。 */
    const char str[] = "A";
    char value;
    int result = rt_sscanf(str, "%c", &value);
    uassert_int_equal(result, 1);
    uassert_int_equal(value, 'A');
}

static void TC_rt_sscanf_hex_1(void)
{
    /* %x 接受 0x 前缀和大写十六进制数字。 */
    const char str[] = "0x1A3F";
    int value;
    int result = rt_sscanf(str, "%x", &value);
    uassert_int_equal(result, 1);
    uassert_int_equal(value, 0x1A3F);
}

static void TC_rt_sscanf_hex_2(void)
{
    /* 连续两次 %x 同时覆盖 0x 与 0X 前缀。 */
    const char str[] = "0x1A 0XFF";
    int a, b;
    rt_sscanf(str, "%x %x", &a, &b);
    uassert_true(a == 0x1A && b == 0XFF);
}

static void TC_rt_sscanf_oct_1(void)
{
    /* %o 按八进制解释字符 0755。 */
    const char str[] = "0755";
    int value;
    int result = rt_sscanf(str, "%o", &value);
    uassert_int_equal(result, 1);
    uassert_int_equal(value, 0755);
}

static void TC_rt_sscanf_oct_2(void)
{
    /* 格式中的空白分隔两个八进制转换。 */
    const char str[] = "012 077";
    int a, b;
    rt_sscanf(str, "%o %o", &a, &b);
    uassert_true(a == 012 && b == 077);
}

static void TC_rt_sscanf_multiple_args(void)
{
    /* 一次调用按 va_list 顺序写入整数和字符串，返回赋值数 2。 */
    const char str[] = "123 Hello";
    int int_value;
    char str_value[20];
    int result = rt_sscanf(str, "%d %s", &int_value, str_value);
    uassert_int_equal(result, 2);
    uassert_int_equal(int_value, 123);
    uassert_str_equal(str_value, "Hello");
}

static void TC_rt_sscanf_pointer(void)
{
    /* %p 把十六进制文本转换为 void *；测试值刻意保持在常见指针可表示范围。 */
    const char str[] = "0x12345678";
    void *ptr;
    int result = rt_sscanf(str, "%p", &ptr);
    uassert_int_equal(result, 1);
    uassert_ptr_equal(ptr, (void *)0x12345678);
}

static void TC_rt_sscanf_width_specifier(void)
{
    /* 字段宽度 4 限制本次整数最多消费前四个数字。 */
    const char str[] = "123456789";
    int value;
    int result = rt_sscanf(str, "%4d", &value);
    uassert_int_equal(result, 1);
    uassert_int_equal(value, 1234);
}

static void TC_rt_sscanf_suppression(void)
{
    /* `%*d` 应消费 123 但不计入赋值数，随后 %d 把 456 写入唯一目标。 */
    const char str[] = "123 456";
    int second_value;
    int result = rt_sscanf(str, "%*d %d", &second_value);
    uassert_int_equal(result, 1);
    uassert_int_equal(second_value, 456);
}

static void TC_rt_sscanf_match_set(void)
{
    /* 正向集合 [a-z] 连续接收小写字母，遇到数字停止并追加 '\0'。 */
    const char str[] = "abc123";
    char buffer[10] = {0};
    int result = rt_sscanf(str, "%[a-z]", buffer);
    uassert_int_equal(result, 1);
    uassert_str_equal(buffer, "abc");
}

static void TC_rt_sscanf_match_set_negated(void)
{
    /* `^` 取反：接收所有非数字字符，直到首个数字。 */
    const char str[] = "abc123";
    char buffer[10];
    int result = rt_sscanf(str, "%[^0-9]", buffer);
    uassert_int_equal(result, 1);
    uassert_str_equal(buffer, "abc");
}

static void TC_rt_sscanf_match_set_range(void)
{
    /* 组合集合中的连字符边界行为由解析器规则决定，本例期望整个字面串均被接收。 */
    const char str[] = "a-zA-Z";
    char buffer[10];
    int result = rt_sscanf(str, "%[a-z-A-Z]", buffer);
    uassert_int_equal(result, 1);
    uassert_str_equal(buffer, "a-zA-Z");
}

static void TC_rt_sscanf_whitespace_skip(void)
{
    /* 数字转换在没有 NOSKIP 标志时自动跳过任意数量的前导空白。 */
    const char str[] = "   12345";
    int value;
    int result = rt_sscanf(str, "%d", &value);
    uassert_int_equal(result, 1);
    uassert_int_equal(value, 12345);
}

static void TC_rt_sscanf_unsigned_int(void)
{
    /* %u 目标为 unsigned int *，覆盖 32 位无符号最大值。 */
    const char str[] = "4294967295";
    unsigned int value;
    int result = rt_sscanf(str, "%u", &value);
    uassert_int_equal(result, 1);
    uassert_int_equal(value, 4294967295U);
}

static void TC_rt_sscanf_long_long_int(void)
{
    /* ll 长度修饰符要求 long long *，覆盖 64 位有符号最大值。 */
    const char str[] = "9223372036854775807";
    long long value;
    int result = rt_sscanf(str, "%lld", &value);
    uassert_int_equal(result, 1);
    uassert_int_equal(value, 9223372036854775807LL);
}

static void TC_rt_sscanf_short_int(void)
{
    /* h 长度修饰符把转换结果写入 short *。 */
    const char str[] = "32767";
    short value;
    int result = rt_sscanf(str, "%hd", &value);
    uassert_int_equal(result, 1);
    uassert_int_equal(value, 32767);
}

static void TC_rt_sscanf_null_string(void)
{
    /* 空输入在第一次数字转换前已经耗尽，内置后端按输入失败返回 -1。 */
    const char str[] = "";
    int value;
    int result = rt_sscanf(str, "%d", &value);
    uassert_int_equal(result, -1);
}

/* 回归场景来源：https://github.com/RT-Thread/rt-thread/issues/9853。 */
static void TC_rt_sscanf_issue_9853(void)
{
    /* 先用取反集合跳过首个逗号前字段，再提取两个以逗号分隔的整数。 */
    int device_socket = 255;
    int bfsz = 255;
    const char str[] = "+MIPURC: \"rtcp\",0,240,HTTP/1.1 200 OK";
    rt_sscanf(str, "+MIPURC:%*[^,],%d,%d", &device_socket, (int *)&bfsz);
    uassert_int_equal(device_socket, 0);
    uassert_int_equal(bfsz, 240);
}

static void utest_do_tc(void)
{
    /* 每个 UTEST_UNIT_RUN 形成独立子用例，任一失败都能定位到具体格式能力。 */
    UTEST_UNIT_RUN(TC_rt_sscanf_char);
    UTEST_UNIT_RUN(TC_rt_sscanf_basic_int);
    UTEST_UNIT_RUN(TC_rt_sscanf_basic_float);
    UTEST_UNIT_RUN(TC_rt_sscanf_basic_string);
    UTEST_UNIT_RUN(TC_rt_sscanf_string_with_space);
    UTEST_UNIT_RUN(TC_rt_sscanf_basic_char);
    UTEST_UNIT_RUN(TC_rt_sscanf_hex_1);
    UTEST_UNIT_RUN(TC_rt_sscanf_hex_2);
    UTEST_UNIT_RUN(TC_rt_sscanf_oct_1);
    UTEST_UNIT_RUN(TC_rt_sscanf_oct_2);
    UTEST_UNIT_RUN(TC_rt_sscanf_multiple_args);
    UTEST_UNIT_RUN(TC_rt_sscanf_pointer);
    UTEST_UNIT_RUN(TC_rt_sscanf_width_specifier);
    UTEST_UNIT_RUN(TC_rt_sscanf_suppression);
    UTEST_UNIT_RUN(TC_rt_sscanf_match_set);
    UTEST_UNIT_RUN(TC_rt_sscanf_match_set_negated);
    UTEST_UNIT_RUN(TC_rt_sscanf_match_set_range);
    UTEST_UNIT_RUN(TC_rt_sscanf_whitespace_skip);
    UTEST_UNIT_RUN(TC_rt_sscanf_unsigned_int);
    UTEST_UNIT_RUN(TC_rt_sscanf_long_long_int);
    UTEST_UNIT_RUN(TC_rt_sscanf_short_int);
    UTEST_UNIT_RUN(TC_rt_sscanf_null_string);
    UTEST_UNIT_RUN(TC_rt_sscanf_issue_9853);
}

UTEST_TC_EXPORT(utest_do_tc, "core.klibc.rt_sscanf", RT_NULL, RT_NULL, 1000);
