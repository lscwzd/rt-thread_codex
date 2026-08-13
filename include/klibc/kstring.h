/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期 作者备注
 * 2024-09-22 Meco Man 第一版
 */

/**
 * @file kstring.h
 * @brief 内核使用的字节内存和 NUL 结尾字符串操作。
 *
 * 这些例程以稳定的 RT-Thread 名称提供类似 libc 的行为，使常用内核代码不依赖某个
 * 特定的应用 C 库。对于支持这些选择的操作，Kconfig 可选择用户提供、libc、tiny 或
 * RT-Thread 通用实现；并非每个函数都有完全相同的可选后端。例如，只有部分函数有
 * tiny 实现，rt_strdup() 还依赖堆支持。用户替换实现必须遵守此头文件的约定，因为
 * 调用者无法区分当前选中的后端。
 *
 * 除 rt_strdup() 外，这些函数既不分配内存，也不获取内核对象。它们不会验证指针、
 * 对象大小或地址空间。调用者拥有所有输入/输出区域，并必须在整个操作期间保证其可访问。
 * 字节计数和字符串容量的单位都是字节，而不是多字节编码中的字符数。
 */

#ifndef __RT_KSTRING_H__
#define __RT_KSTRING_H__

/* 提供定宽/原生宽度类型、size_t、RT_NULL 以及 C/C++ 可移植性定义。 */
#include <rttypes.h>

#ifdef __cplusplus
/* 向 C++ 组件公开名称不改编的同一组 C 符号。 */
extern "C" {
#endif

/**
 * @brief 用同一个字节重复填充一段内存。
 *
 * @p c 会先转换为 `unsigned char`，再从 @p s 开始写入前 @p n 个 C 字节。即使目标
 * 的 `unsigned char` 宽于八位，这一规则仍成立。API 不要求调用者处理对齐；选中的优化
 * 后端会在内部处理。
 *
 * @param s 可写区域起始地址；当 n 非零时，该区域至少为 @p n 字节。
 * @param c 重复写入前会转换为 `unsigned char` 的整数。
 * @param n 要写入的字节数；零表示不写入任何字节。
* @return 原始 @p s 指针，可用于链式调用。
 */
void *rt_memset(void *s, int c, size_t n);

/**
 * @brief 复制一段不重叠的字节区域。
 *
 * 会恰好从 @p src 向 @p dest 复制 @p n 字节。按照 memcpy 的约定，源和目标不得
 * 重叠，即使某个 tiny/用户后端碰巧能够容忍重叠也一样。区域可能相交时应使用 rt_memmove()。
 *
 * @param dest 至少为 @p n 字节的可写目标区域起始地址。
 * @param src 至少为 @p n 字节的可读源区域起始地址。
 * @param n 要复制的字节数；不涉及 NUL 或字符串语义。
* @return 原始 @p dest 指针。
 */
void *rt_memcpy(void *dest, const void *src, size_t n);

/**
 * @brief 即使源和目标重叠，也能正确复制字节区域。
 *
 * 其结果如同先把全部 @p n 个源字节复制到临时存储，再写入 @p dest；实现通常通过选择
 * 正向或反向遍历来达到这一效果，无需分配内存。
 *
 * @param dest至少 @p n 字节的可写目标区域的开始。
 * @param src至少 @p n 字节的可读源区域的开始。
 * @param n要移动的字节数。
 * @return原来的@p dest指针。
 */
void *rt_memmove(void *dest, const void *src, size_t n);

/**
 * @brief按字典顺序比较两个字节区域。
 *
 * 字节被解释为无符号值。  比较从一开始就停止
* 不同字节或@p计数字节之后；嵌入的零没有特殊的
 * 意义。
 *
 * @param cs至少 @p 计数字节的第一个可读区域。
 * @param ct至少 @p 计数字节的第二个可读区域。
 * @param count考虑的最大/确切字节数。
 * @return小于、等于或大于零的值，根据
 *         @p cs 中的第一个不同字节小于、等于或大于
 *         比 @p ct 中的相应字节要大。  不依赖于大小。
 */
int rt_memcmp(const void *cs, const void *ct, size_t count);

/**
 * @brief分配并返回字符串的独立副本。
 *
 * 仅当启用 `RT_USING_HEAP` 时，才会链接内置定义。  它
 * 使用 rt_malloc() 分配 `rt_strlen(s) + 1` 字节，复制终止
 * NUL，并将新分配的所有权转移给调用者。  这
 * 调用者最终必须使用 rt_free() 释放非 NULL 结果，并且必须
 * 遵守配置的堆的线程/中断上下文限制。
 *
* @param s 可读的以 NUL 结尾的源字符串；它必须保持有效直到
 *        复制完成。
 * @return新分配的可写副本，如果分配失败则为 RT_NULL。
 */
char *rt_strdup(const char *s);

/**
 * @brief确定受上限限制的字符串长度。
 *
 * 返回的计数不包括终止 NUL 并且永远不会超过
 * @p 最大长度。  返回等于 @p maxlen 并不能证明存储是
 * 终止。  请注意当前的内置后端怪癖：其循环测试 `*s`
 * 在测试边界之前，它可以读取 `s[maxlen]` 处的字节，甚至可以读取
 * 零界仍然评估 `*s`。  在该实施得到纠正之前，
* 通过该位置提供可读字节；用户提供的后端可以
 * 有不同的评价细节。
 *
 * @param s检查其前缀的字符串/缓冲区。
 * @param maxlen报告的最大长度。
 * @return第一个 NUL 或 @p maxlen 之前的非 NUL 字节数。
 */
size_t rt_strnlen(const char *s, size_t maxlen);

/**
 * @brief查找一个字符串在另一个字符串中第一次出现的位置。
 *
 * 两个输入必须以 NUL 端接。  空的 @p str2 在开头匹配
 * @p str1。  尽管输入是 const 限定的，但历史 API
* 返回一个可变指针到@p str1；通过它修改才有效
 * 如果原始存储实际上是可写的。
 *
 * @param str1NUL 终止的 haystack 字符串。
 * @param str2NUL 端接针串。
 * @return指向 @p str1 中第一个匹配字节的指针，如果不存在，则指向 RT_NULL。
 */
char *rt_strstr(const char *str1, const char *str2);

/**
 * @brief比较两个以 NUL 结尾且不带 ASCII 字母大小写的字符串。
 *
 * 内置实现仅折叠字节 `A` 到 `Z` 到 `a`
*通过`z`；它与区域设置无关并且不执行 Unicode 或
 * 多字节大小写折叠。  用户替换应该记录更广泛的内容
 * 它引入的区域设置行为。
 *
 * @param a第一个可读的以 NUL 结尾的字符串。
 * @param b第二个可读的 NUL 终止字符串。
 * @return根据之前折叠的 @p a 排序，为负、零或正，
 *         等于或折叠后排序 @p b．  不依赖于大小。
 */
int rt_strcasecmp(const char *a, const char *b);

/**
 * @brief将完整的 NUL 终止字符串复制到调用者拥有的存储中。
 *
 * 目标必须至少具有 `rt_strlen(src) + 1` 可写字节。
* 没有容量参数或截断；空间不足溢出
 * 目的地。  源和目的地不得重叠。
 *
 * @param dst可写目标足够大，可容纳源字节和最终 NUL。
 * @param src可读的以 NUL 结尾的源字符串。
 * @return原来的@p dst指针。
 */
char *rt_strcpy(char *dst, const char *src);

/**
 * @brief使用与 strncpy 兼容的填充语义最多复制 @p n 个字节。
 *
 * 如果 @p src 在 @p n 字节之前结束，则 @p dest 的剩余部分填充为
 * NUL 字节。  如果源长度至少为 @p n，则恰好为 @p n 非 NUL
 * 字节可以被复制，并且目的地*不是* NUL 终止的。  这是
 * 固定字段复制而不是保证终止助手。  地区
 * 不得重叠。
 *
 * @param dest至少 @p n 字节的可写目标区域。
 * @param src可读的 NUL 终止源，或至少可读 @p n
 *        当不存在较早的 NUL 时字节。
 * @param n目标字段大小和复制的最大字节数。
 * @return原来的@p dest指针。
 */
char *rt_strncpy(char *dest, const char *src, size_t n);

/**
 * @brief比较两个以 NUL 结尾的字符串的至多 @p 计数字节。
 *
 * 比较在第一个不同字节、NUL 或计数限制处停止。
 * 零计数不比较任何字符并返回相等性。  对于普通的
 * ASCII 文本，非零结果的符号给出了词汇顺序。  内置的
 * 后端减去普通的 `char` 值，因此字节顺序等于或高于 0x80
* 可能因字符符号或 libc/用户后端而异；平等（零）
 * 仍然是对任意高位数据的便携式测试。
 *
 * @param cs第一个字符串，通过比较的前缀可读。
 * @param ct第二个字符串，通过比较的前缀可读。
 * @param count考虑的最大字节数。
 * @return所选后端排序的负数、零数或正数。
 */
int rt_strncmp(const char *cs, const char *ct, size_t count);

/**
 * @brief按字典顺序比较两个完整的 NUL 终止字符串。
 *
 * @param cs第一个可读的以 NUL 结尾的字符串。
 * @param ct第二个可读的 NUL 终止字符串。
 * 对于普通的 ASCII 文本，该符号给出了词汇顺序。  内置后端
 * 减去普通的 `char` 值，因此高位字节排序可能会因目标而异
* 字符符号和后端；使用零结果来实现可移植相等。
 *
 * @return所选后端排序的负数、零数或正数。
 */
int rt_strcmp(const char *cs, const char *ct);

/**
 * @brief返回字符串终止点 NUL 之前的字节数。
 *
 * 不包括终止符。  没有扫描绑定：如果 @p src 不是
 * NUL-在可读内存内终止，函数读取超出对象范围
 * 并且行为是未定义的。  结果计算字节数，而不是编码字符。
 *
 * @param src可读的 NUL 终止字符串。
 * @return终止符之前的非 NUL 字节数。
 */
size_t rt_strlen(const char *src);

#ifdef __cplusplus
}
#endif

#endif
