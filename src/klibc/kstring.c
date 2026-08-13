/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 修改记录：
 * 日期           作者         说明
 * 2024-03-10     Meco Man     首个版本
 */

/**
 * @file kstring.c
 * @brief RT-Thread 内核使用的内存块和字符串基础操作。
 *
 * 初学者可以把本文件看成内核自己的 `<string.h>` 适配层。每个接口可能有三种来源：
 *
 * 1. 配置为 USER 版本时，本文件不定义该函数，由用户或 CPU 端口提供；
 * 2. 配置为 LIBC 版本时，薄封装转调工具链 C 库；
 * 3. 否则使用本文件中的 tiny 或通用实现。
 *
 * 因此，阅读函数体时必须先看条件编译。不同后端在优化方式、极端参数和高位字符
 * 排序等细节上可能存在差异。这里的函数通常处于内核底层，不负责检查空指针、
 * 缓冲区大小或地址可访问性；调用者必须先保证参数合法。除 rt_memmove() 外，复制
 * 接口也不应依赖源区与目标区重叠时的行为。
 */

#include <rtthread.h>

#if defined(RT_KLIBC_USING_LIBC_MEMSET) || \
    defined(RT_KLIBC_USING_LIBC_MEMCPY) || \
    defined(RT_KLIBC_USING_LIBC_MEMMOVE) || \
    defined(RT_KLIBC_USING_LIBC_MEMCMP) || \
    defined(RT_KLIBC_USING_LIBC_STRSTR) || \
    defined(RT_KLIBC_USING_LIBC_STRNCPY) || \
    defined(RT_KLIBC_USING_LIBC_STRCPY) || \
    defined(RT_KLIBC_USING_LIBC_STRNCMP) || \
    defined(RT_KLIBC_USING_LIBC_STRCMP) || \
    defined(RT_KLIBC_USING_LIBC_STRLEN)
#include <string.h>
#endif

/**
 * @brief 用指定值填充一段连续内存。
 *
 * @param s 待填充内存块的起始地址；必须至少有 @p count 个可写字节。
 *
 * @param c 填充值。接口以 int 传入，实现按 unsigned char 的值逐字符写入；
 *          因而实际使用的是目标平台一个“字符单元”能够表示的低位值。
 *
 * @param count 要填充的字符单元数。
 *
 * @return 原样返回 @p s，便于把调用结果继续传给其他表达式。
 *
 * @note 通用实现先判断地址对齐和长度：对齐的大块按机器字批量写入，头尾或短块
 *       按字符写入。它不分配内存，也不做边界检查。
 */
#ifndef RT_KLIBC_USING_USER_MEMSET
void *rt_memset(void *s, int c, size_t count)
{
#if defined(RT_KLIBC_USING_LIBC_MEMSET)
    return memset(s, c, count);
#elif defined(RT_KLIBC_USING_TINY_MEMSET)
    char *xs = (char *)s;

    while (count--)
        *xs++ = c;

    return s;
#else

#define LBLOCKSIZE      (sizeof(rt_ubase_t))
#define UNALIGNED(X)    ((long)X & (LBLOCKSIZE - 1))
#define TOO_SMALL(LEN)  ((LEN) < LBLOCKSIZE)

    unsigned int i = 0;
    char *m = (char *)s;
    unsigned long buffer = 0;
    unsigned long *aligned_addr = RT_NULL;
    unsigned char d = (unsigned int)c & (unsigned char)(-1);  /* 先转成无符号字符，避免有符号扩展；
                                与 char 是 8 位还是 16 位无关。 */

    RT_ASSERT(LBLOCKSIZE == 2 || LBLOCKSIZE == 4 || LBLOCKSIZE == 8);

    if (!TOO_SMALL(count) && !UNALIGNED(s))
    {
        /* 到达这里说明长度足够大，并且起始地址已按机器字对齐。 */
        aligned_addr = (unsigned long *)s;

        /* 把 d 复制到 buffer 的每个字符位置，构造一个“每字节都相同”的机器字，
         * 后续便可一次写入一个机器字来加速大块填充。
         */
        for (i = 0; i < LBLOCKSIZE; i++)
        {
            *(((unsigned char *)&buffer)+i) = d;
        }

        while (count >= LBLOCKSIZE * 4)
        {
            *aligned_addr++ = buffer;
            *aligned_addr++ = buffer;
            *aligned_addr++ = buffer;
            *aligned_addr++ = buffer;
            count -= 4 * LBLOCKSIZE;
        }

        while (count >= LBLOCKSIZE)
        {
            *aligned_addr++ = buffer;
            count -= LBLOCKSIZE;
        }

        /* 剩余不足一个机器字的尾部交给下面的逐字符循环处理。 */
        m = (char *)aligned_addr;
    }

    while (count--)
    {
        *m++ = (char)d;
    }

    return s;

#undef LBLOCKSIZE
#undef UNALIGNED
#undef TOO_SMALL
#endif /* RT_KLIBC_USING_LIBC_MEMSET */
}
#endif /* RT_KLIBC_USING_USER_MEMSET */
RTM_EXPORT(rt_memset);

/**
 * @brief 把一段内存从源地址复制到目标地址。
 *
 * @param dst 目标内存起始地址；必须至少有 @p count 个可写字符单元。
 *
 * @param src 源内存起始地址；必须至少有 @p count 个可读字符单元。
 *
 * @param count 复制长度。
 *
 * @return 原样返回 @p dst。
 *
 * @warning 公共接口按 memcpy 语义使用：源区和目标区重叠时应改用 rt_memmove()。
 *          tiny 后端虽然碰巧按地址选择方向，但调用者不应依赖某个后端的额外行为。
 * @note 通用实现只在两端均按 long 对齐且长度足够大时采用机器字展开复制；否则
 *       直接逐字符复制。这里没有缓存维护或 DMA 同步，相关工作由调用者负责。
 */
#ifndef RT_KLIBC_USING_USER_MEMCPY
void *rt_memcpy(void *dst, const void *src, size_t count)
{
#if defined(RT_KLIBC_USING_LIBC_MEMCPY)
    return memcpy(dst, src, count);
#elif defined(RT_KLIBC_USING_TINY_MEMCPY)
    char *tmp = (char *)dst, *s = (char *)src;
    size_t len = 0;

    if (tmp <= s || tmp > (s + count))
    {
        while (count--)
            *tmp ++ = *s ++;
    }
    else
    {
        for (len = count; len > 0; len --)
            tmp[len - 1] = s[len - 1];
    }

    return dst;
#else

#define UNALIGNED(X, Y) \
    (((long)X & (sizeof (long) - 1)) | ((long)Y & (sizeof (long) - 1)))
#define BIGBLOCKSIZE    (sizeof (long) << 2)
#define LITTLEBLOCKSIZE (sizeof (long))
#define TOO_SMALL(LEN)  ((LEN) < BIGBLOCKSIZE)

    char *dst_ptr = (char *)dst;
    char *src_ptr = (char *)src;
    long *aligned_dst = RT_NULL;
    long *aligned_src = RT_NULL;
    size_t len = count;

    /* 长度太小，或者源、目标任一地址未按 long 对齐时，直接走末尾的逐字符循环。 */
    if (!TOO_SMALL(len) && !UNALIGNED(src_ptr, dst_ptr))
    {
        aligned_dst = (long *)dst_ptr;
        aligned_src = (long *)src_ptr;

        /* 每轮展开复制 4 个 long，减少循环判断开销。 */
        while (len >= BIGBLOCKSIZE)
        {
            *aligned_dst++ = *aligned_src++;
            *aligned_dst++ = *aligned_src++;
            *aligned_dst++ = *aligned_src++;
            *aligned_dst++ = *aligned_src++;
            len -= BIGBLOCKSIZE;
        }

        /* 不足 4 个 long 后，继续每次复制 1 个 long。 */
        while (len >= LITTLEBLOCKSIZE)
        {
            *aligned_dst++ = *aligned_src++;
            len -= LITTLEBLOCKSIZE;
        }

        /* 把指针换回字符指针，处理不足一个 long 的尾部。 */
        dst_ptr = (char *)aligned_dst;
        src_ptr = (char *)aligned_src;
    }

    while (len--)
        *dst_ptr++ = *src_ptr++;

    return dst;
#undef UNALIGNED
#undef BIGBLOCKSIZE
#undef LITTLEBLOCKSIZE
#undef TOO_SMALL
#endif /* RT_KLIBC_USING_LIBC_MEMCPY */
}
#endif /* RT_KLIBC_USING_USER_MEMCPY */
RTM_EXPORT(rt_memcpy);

/**
 * @brief 安全移动一段内存，允许源区和目标区相互重叠。
 *
 * @param dest 目标内存起始地址；必须至少有 @p n 个可写字符单元。
 *
 * @param src 源内存起始地址；必须至少有 @p n 个可读字符单元。
 *
 * @param n 移动长度。
 *
 * @return 原样返回 @p dest。
 *
 * @details 当目标起点落在源区内部且位于源起点之后时，从尾部向前复制，避免尚未
 *          读取的数据被覆盖；其余情况从头向后复制。该函数不检查地址越界。
 */
#ifndef RT_KLIBC_USING_USER_MEMMOVE
void *rt_memmove(void *dest, const void *src, size_t n)
{
#ifdef RT_KLIBC_USING_LIBC_MEMMOVE
    return memmove(dest, src, n);
#else
    char *tmp = (char *)dest, *s = (char *)src;

    if (s < tmp && tmp < s + n)
    {
        tmp += n;
        s += n;

        while (n--)
            *(--tmp) = *(--s);
    }
    else
    {
        while (n--)
            *tmp++ = *s++;
    }

    return dest;
#endif /* RT_KLIBC_USING_LIBC_MEMMOVE */
}
#endif /* RT_KLIBC_USING_USER_MEMMOVE */
RTM_EXPORT(rt_memmove);

/**
 * @brief 按无符号字符逐个比较两段内存。
 *
 * @param cs 第一段内存，至少包含 @p count 个可读字符单元。
 *
 * @param ct 第二段内存，至少包含 @p count 个可读字符单元。
 *
 * @param count 最大比较长度。
 *
 * @return 首个不同字符的差值：小于 0 表示 @p cs 较小，大于 0 表示 @p cs
 *         较大，等于 0 表示指定长度内完全相同。
 */
#ifndef RT_KLIBC_USING_USER_MEMCMP
int rt_memcmp(const void *cs, const void *ct, size_t count)
{
#ifdef RT_KLIBC_USING_LIBC_MEMCMP
    return memcmp(cs, ct, count);
#else
    const unsigned char *su1 = RT_NULL, *su2 = RT_NULL;
    int res = 0;

    for (su1 = (const unsigned char *)cs, su2 = (const unsigned char *)ct; 0 < count; ++su1, ++su2, count--)
        if ((res = *su1 - *su2) != 0)
            break;

    return res;
#endif /* RT_KLIBC_USING_LIBC_MEMCMP */
}
#endif /* RT_KLIBC_USING_USER_MEMCMP */
RTM_EXPORT(rt_memcmp);

/**
 * @brief 在字符串 @p s1 中查找子串 @p s2 第一次出现的位置。
 *
 * @param s1 被搜索的、以 '\0' 结尾的字符串。
 *
 * @param s2 要查找的、以 '\0' 结尾的子串。
 *
 * @return 找到时返回 @p s1 内首个匹配位置；未找到时返回 RT_NULL。空子串返回
 *         @p s1 本身。
 *
 * @note 内置实现先计算两串长度，再在每个候选起点调用 rt_memcmp()；它偏重代码
 *       简洁而非复杂模式匹配算法的性能。
 */
#ifndef RT_KLIBC_USING_USER_STRSTR
char *rt_strstr(const char *s1, const char *s2)
{
#ifdef RT_KLIBC_USING_LIBC_STRSTR
    return strstr(s1, s2);
#else
    int l1 = 0, l2 = 0;

    l2 = rt_strlen(s2);
    if (!l2)
    {
        return (char *)s1;
    }

    l1 = rt_strlen(s1);
    while (l1 >= l2)
    {
        l1 --;
        if (!rt_memcmp(s1, s2, l2))
        {
            return (char *)s1;
        }

        s1 ++;
    }

    return RT_NULL;
#endif /* RT_KLIBC_USING_LIBC_STRSTR */
}
#endif /* RT_KLIBC_USING_USER_STRSTR */
RTM_EXPORT(rt_strstr);

/**
 * @brief 忽略 ASCII 英文字母大小写后比较两个字符串。
 *
 * @param a 第一个以 '\0' 结尾的字符串。
 *
 * @param b 第二个以 '\0' 结尾的字符串。
 *
 * @return 小于 0、等于 0 或大于 0，分别表示转换后的 @p a 小于、等于或大于
 *         @p b。
 *
 * @note 这里只折叠 'A'～'Z'，不处理区域设置、Unicode 或其他字符集规则。读取值
 *       先与 0xff 相与，以便稳定处理最高位为 1 的 8 位字符。
 */
#ifndef RT_KLIBC_USING_USER_STRCASECMP
int rt_strcasecmp(const char *a, const char *b)
{
    int ca = 0, cb = 0;

    do
    {
        ca = *a++ & 0xff;
        cb = *b++ & 0xff;
        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
    }
    while (ca == cb && ca != '\0');

    return ca - cb;
}
#endif /* RT_KLIBC_USING_USER_STRCASECMP */
RTM_EXPORT(rt_strcasecmp);

/**
 * @brief 最多复制 @p n 个字符，并按 strncpy 规则补零。
 *
 * @param dst 目标缓冲区，必须至少能写入 @p n 个字符。
 *
 * @param src 源字符串。
 *
 * @param n 目标缓冲区参与写入的最大长度。
 *
 * @return 原样返回 @p dst。
 *
 * @warning 若源串长度大于或等于 @p n，目标结果不会自动追加 '\0'；使用者不能
 *          把它无条件当作完整 C 字符串。若提前遇到 '\0'，剩余位置全部补零。
 */
#ifndef RT_KLIBC_USING_USER_STRNCPY
char *rt_strncpy(char *dst, const char *src, size_t n)
{
#ifdef RT_KLIBC_USING_LIBC_STRNCPY
    return strncpy(dst, src, n);
#else
    if (n != 0)
    {
        char *d = dst;
        const char *s = src;

        do
        {
            if ((*d++ = *s++) == 0)
            {
                /* 已复制终止符，把剩余的 n-1 个位置全部补成 '\0'。 */
                while (--n != 0)
                {
                    *d++ = 0;
                }

                break;
            }
        } while (--n != 0);
    }

    return (dst);
#endif /* RT_KLIBC_USING_LIBC_STRNCPY */
}
#endif /* RT_KLIBC_USING_USER_STRNCPY */
RTM_EXPORT(rt_strncpy);

/**
 * @brief 复制一个以 '\0' 结尾的字符串。
 *
 * @param dst 目标缓冲区，容量必须至少为 rt_strlen(src) + 1。
 *
 * @param src 源字符串。
 *
 * @return 原样返回 @p dst。
 *
 * @warning 接口不知道目标容量，也不支持重叠内存；容量不足会造成越界写。
 */
#ifndef RT_KLIBC_USING_USER_STRCPY
char *rt_strcpy(char *dst, const char *src)
{
#ifdef RT_KLIBC_USING_LIBC_STRCPY
    return strcpy(dst, src);
#else
    char *dest = dst;

    while (*src != '\0')
    {
        *dst = *src;
        dst++;
        src++;
    }

    *dst = '\0';
    return dest;
#endif /* RT_KLIBC_USING_LIBC_STRCPY */
}
#endif /* RT_KLIBC_USING_USER_STRCPY */
RTM_EXPORT(rt_strcpy);

/**
 * @brief 最多比较两个字符串的前 @p count 个字符。
 *
 * @param cs 第一个字符串。
 *
 * @param ct 第二个字符串。
 *
 * @param count 最大比较字符数；为 0 时不解引用字符串。
 *
 * @return 小于 0、等于 0 或大于 0，分别表示 @p cs 小于、等于或大于 @p ct。
 *
 * @warning 内置后端通过 char 的差值比较；当 char 默认为有符号且字符最高位为 1
 *          时，排序可能与按 unsigned char 比较的某些 libc 后端不同。内核代码若
 *          需要跨后端的二进制字节顺序，应使用 rt_memcmp()。
 */
#ifndef RT_KLIBC_USING_USER_STRNCMP
int rt_strncmp(const char *cs, const char *ct, size_t count)
{
#ifdef RT_KLIBC_USING_LIBC_STRNCMP
    return strncmp(cs, ct, count);
#else
    signed char res = 0;

    while (count)
    {
        if ((res = *cs - *ct++) != 0 || !*cs++)
        {
            break;
        }

        count --;
    }

    return res;
#endif /* RT_KLIBC_USING_LIBC_STRNCMP */
}
#endif /* RT_KLIBC_USING_USER_STRNCMP */
RTM_EXPORT(rt_strncmp);

/**
 * @brief 比较两个以 '\0' 结尾的字符串。
 *
 * @param cs 第一个字符串。
 *
 * @param ct 第二个字符串。
 *
 * @return 小于 0、等于 0 或大于 0，分别表示 @p cs 小于、等于或大于 @p ct。
 *
 * @warning 与 rt_strncmp() 一样，内置后端对最高位为 1 的 char 的排序可能与 libc
 *          后端不同；相等性判断不受这一排序差异影响。
 */
#ifndef RT_KLIBC_USING_USER_STRCMP
int rt_strcmp(const char *cs, const char *ct)
{
#ifdef RT_KLIBC_USING_LIBC_STRCMP
    return strcmp(cs, ct);
#else
    while (*cs && *cs == *ct)
    {
        cs++;
        ct++;
    }

    return (*cs - *ct);
#endif /* RT_KLIBC_USING_LIBC_STRCMP */
}
#endif /* RT_KLIBC_USING_USER_STRCMP */
RTM_EXPORT(rt_strcmp);

/**
 * @brief 计算 C 字符串中终止符 '\0' 之前的字符数。
 *
 * @param s 以 '\0' 结尾的字符串。
 *
 * @return 不包含终止符的字符串长度。
 *
 * @warning 若可访问范围内不存在 '\0'，函数会继续越界读取；它也不接受空指针。
 */
#ifndef RT_KLIBC_USING_USER_STRLEN
size_t rt_strlen(const char *s)
{
#ifdef RT_KLIBC_USING_LIBC_STRLEN
    return strlen(s);
#else
    const char *sc = RT_NULL;
    for (sc = s; *sc != '\0'; ++sc);
    return sc - s;
#endif /* RT_KLIBC_USING_LIBC_STRLEN */
}
#endif /* RT_KLIBC_USING_USER_STRLEN */
RTM_EXPORT(rt_strlen);

/**
 * @brief 在给定上限附近计算字符串长度。
 *
 * @param s 待测字符串。
 *
 * @param maxlen 希望检查的最大字符数。
 *
 * @return 终止符位于限制范围内时返回其前面的字符数，否则通常返回 @p maxlen。
 *
 * @warning 当前内置实现的循环条件先读取 `*sc`，再检查距离是否达到 @p maxlen。
 *          因此 maxlen 为 0，或前 maxlen 个字符均非 '\0' 时，它仍会读取
 *          `s[maxlen]`。调用者必须保证该额外位置可读；这与标准 strnlen 不越过
 *          上限的约定存在差异，不能把本接口用于守卫不可访问的页边界。
 */
#ifndef RT_KLIBC_USING_USER_STRNLEN
size_t rt_strnlen(const char *s, size_t maxlen)
{
    const char *sc;
    for (sc = s; *sc != '\0' && (size_t)(sc - s) < maxlen; ++sc);
    return sc - s;
}
#endif /* RT_KLIBC_USING_USER_STRNLEN */
RTM_EXPORT(rt_strnlen);

#ifdef RT_USING_HEAP
/**
 * @brief 在系统堆上创建字符串副本。
 *
 * @param s 要复制的、以 '\0' 结尾的字符串。
 *
 * @return 成功时返回新分配字符串；内存不足时返回 RT_NULL。
 *
 * @note 返回内存的所有权交给调用者，使用结束后必须通过 rt_free() 释放。函数先
 *       计算长度并连同终止符一起复制，因此调用期间源字符串必须保持有效且不变。
 */
char *rt_strdup(const char *s)
{
    size_t len = rt_strlen(s) + 1;
    char *tmp = (char *)rt_malloc(len);

    if (!tmp)
    {
        return RT_NULL;
    }

    rt_memcpy(tmp, s, len);

    return tmp;
}
RTM_EXPORT(rt_strdup);
#endif /* RT_USING_HEAP */
