/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 修改记录：
 * 日期           作者         说明
 * 2024-11-19     Meco Man     首个版本
 */

/**
 * @file rt_vsnprintf_tiny.c
 * @brief 面向资源受限系统的精简格式化输出后端。
 *
 * 本实现只处理字符、字符串、指针以及二/八/十/十六进制整数，目标是以较小代码
 * 体积满足内核日志的常见需求。它会始终计算“完整结果本应具有的长度”，但只在
 * `[buf, buf + size)` 范围内写入，并在 size 大于 0 时保证末尾有 '\0'。
 *
 * 与完整 C printf 相比，这个 tiny 后端有意省略浮点、`%n` 等功能，并且个别宽度和
 * 精度细节并不完全遵循 ISO C。需要完整格式行为时应在配置中选择标准或 libc
 * 后端。所有辅助函数只解析格式串或写调用者缓冲区，不分配堆内存。
 */

#include <rtthread.h>

#define _ISDIGIT(c)  ((unsigned)((c) - '0') < 10)

/**
 * @brief 对无符号整数做一次“除基数并取余”。
 *
 * @param n 输入/输出整数：返回前被更新为原值除以 @p base 的商。
 *
 * @param base 进制基数；本文件实际传入 2、8、10 或 16。
 *
 * @return 原值除以 @p base 的余数，即当前最低位数字。
 *
 * @details print_number() 反复调用本函数，从低位到高位提取数字。是否支持
 *          long long 由配置决定；此函数名保留自传统内核 printf 实现。
 */
#ifdef RT_KLIBC_USING_VSNPRINTF_LONGLONG
rt_inline int divide(unsigned long long *n, int base)
#else
rt_inline int divide(unsigned long *n, int base)
#endif /* RT_KLIBC_USING_VSNPRINTF_LONGLONG */
{
    int res;

    /* 把取余和更新商集中在一处，便于端口或编译器针对无硬件除法的 CPU 优化。 */
#ifdef RT_KLIBC_USING_VSNPRINTF_LONGLONG
    res = (int)((*n) % base);
    *n = (long long)((*n) / base);
#else
    res = (int)((*n) % base);
    *n = (long)((*n) / base);
#endif

    return res;
}

/**
 * @brief 从格式串当前位置读取连续十进制数字。
 *
 * @param s 指向“格式串游标”的指针；返回后游标停在第一个非数字字符。
 * @return 解析得到的非负整数。这里不检测 int 溢出，格式串应来自可信代码。
 */
rt_inline int skip_atoi(const char **s)
{
    int i = 0;
    while (_ISDIGIT(**s))
        i = i * 10 + *((*s)++) - '0';

    return i;
}

#define ZEROPAD     (1 << 0)    /* 字段不足时用 '0' 填充 */
#define SIGN        (1 << 1)    /* 按有符号整数解释参数 */
#define PLUS        (1 << 2)    /* 正数也显示 '+' */
#define SPACE       (1 << 3)    /* 无正负号时在正数前放空格 */
#define LEFT        (1 << 4)    /* 在字段宽度内左对齐 */
#define SPECIAL     (1 << 5)    /* 输出 0、0x 或 0b 进制前缀 */
#define LARGE       (1 << 6)    /* 十六进制使用大写 A～F */

/**
 * @brief 把一个整数按照给定进制、宽度和标志追加到输出游标。
 *
 * @param buf 当前逻辑写入位置。
 * @param end 可写区域末尾（不含）；即使达到末尾，逻辑游标仍继续前进以统计总长度。
 * @param num 待格式化数值。若 SIGN 置位，会依据 qualifier 转回相应有符号宽度。
 * @param base 进制，当前支持 2、8、10、16。
 * @param qualifier 整数长度修饰符：h、l、L 或默认宽度。
 * @param s 最小字段宽度，负值表示未指定。
 * @param precision 最小数字位数；负值表示未指定。
 * @param type ZEROPAD、SIGN、PLUS、SPACE、LEFT、SPECIAL、LARGE 的组合。
 * @return 更新后的逻辑输出游标，可能超过 @p end，但超过部分不会被解引用。
 *
 * @details 先确定符号和前缀，再把数值按“低位在前”写入临时数组；真正输出时反向
 *          读取数组，得到正常数字顺序。字段宽度依次扣除符号、进制前缀、精度补零
 *          和数字本身，剩余位置按对齐方向补空格或零。
 */

static char *print_number(char *buf,
                          char *end,
#ifdef RT_KLIBC_USING_VSNPRINTF_LONGLONG
                          unsigned long long  num,
#else
                          unsigned long  num,
#endif /* RT_KLIBC_USING_VSNPRINTF_LONGLONG */
                          int   base,
                          int   qualifier,
                          int   s,
                          int   precision,
                          int   type)
{
    char c = 0, sign = 0;
#ifdef RT_KLIBC_USING_VSNPRINTF_LONGLONG
    char tmp[64] = {0};
#else
    char tmp[32] = {0};
#endif /* RT_KLIBC_USING_VSNPRINTF_LONGLONG */
    int precision_bak = precision;
    const char *digits = RT_NULL;
    static const char small_digits[] = "0123456789abcdef";
    static const char large_digits[] = "0123456789ABCDEF";
    int i = 0;
    int size = 0;

    size = s;

    digits = (type & LARGE) ? large_digits : small_digits;
    if (type & LEFT)
    {
        type &= ~ZEROPAD;
    }

    c = (type & ZEROPAD) ? '0' : ' ';

    /* 按长度修饰符选择有符号解释宽度，并把负数转换为绝对值及 '-' 前缀。 */
    sign = 0;
    if (type & SIGN)
    {
        switch (qualifier)
        {
        case 'h':
            if ((rt_int16_t)num < 0)
            {
                sign = '-';
                num = (rt_uint16_t)-num;
            }
            break;
        case 'L':
        case 'l':
            if ((long)num < 0)
            {
                sign = '-';
                num = (unsigned long)-num;
            }
            break;
        case 0:
        default:
            if ((rt_int32_t)num < 0)
            {
                sign = '-';
                num = (rt_uint32_t)-num;
            }
            break;
        }

        if (sign != '-')
        {
            if (type & PLUS)
            {
                sign = '+';
            }
            else if (type & SPACE)
            {
                sign = ' ';
            }
        }
    }

    if (type & SPECIAL)
    {
        if (base == 2 || base == 16)
        {
            size -= 2;
        }
        else if (base == 8)
        {
            size--;
        }
    }

    i = 0;
    if (num == 0)
    {
        tmp[i++] = '0';
    }
    else
    {
        while (num != 0)
            tmp[i++] = digits[divide(&num, base)];
    }

    if (i > precision)
    {
        precision = i;
    }
    size -= precision;

    if (!(type & (ZEROPAD | LEFT)))
    {
        if ((sign) && (size > 0))
        {
            size--;
        }

        while (size-- > 0)
        {
            if (buf < end)
            {
                *buf = ' ';
            }

            ++ buf;
        }
    }

    if (sign)
    {
        if (buf < end)
        {
            *buf = sign;
        }
        -- size;
        ++ buf;
    }

    if (type & SPECIAL)
    {
        if (base == 2)
        {
            if (buf < end)
                *buf = '0';
            ++ buf;
            if (buf < end)
                *buf = 'b';
            ++ buf;
        }
        else if (base == 8)
        {
            if (buf < end)
                *buf = '0';
            ++ buf;
        }
        else if (base == 16)
        {
            if (buf < end)
            {
                *buf = '0';
            }

            ++ buf;
            if (buf < end)
            {
                *buf = type & LARGE ? 'X' : 'x';
            }
            ++ buf;
        }
    }

    /* 非左对齐时，在数字前补齐剩余字段宽度；ZEROPAD 决定补零还是空格。 */
    if (!(type & LEFT))
    {
        while (size-- > 0)
        {
            if (buf < end)
            {
                *buf = c;
            }

            ++ buf;
        }
    }

    while (i < precision--)
    {
        if (buf < end)
        {
            *buf = '0';
        }

        ++ buf;
    }

    /* 临时数组中的数字顺序相反，因此从末尾向前写入最终缓冲区。 */
    while (i-- > 0 && (precision_bak != 0))
    {
        if (buf < end)
        {
            *buf = tmp[i];
        }

        ++ buf;
    }

    while (size-- > 0)
    {
        if (buf < end)
        {
            *buf = ' ';
        }

        ++ buf;
    }

    return buf;
}

#if (defined(__GNUC__) && !defined(__ARMCC_VERSION) /* GCC */) && (__GNUC__ >= 7)
/* GCC 7 及以上会诊断 switch 有意贯穿，局部关闭该诊断。 */
#pragma GCC diagnostic push
/* 下面若干 case 通过贯穿来叠加标志或共用处理路径。 */
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif /* (defined(__GNUC__) && !defined(__ARMCC_VERSION)) && (__GNUC__ >= 7 */
/**
 * @brief 按格式串和 va_list 生成有长度上限的字符串。
 *
 * @param buf 输出缓冲区。size 大于 0 时必须有效且至少可写 @p size 个字符。
 *
 * @param size 缓冲区总容量，包含结尾 '\0' 所占位置；为 0 时只计算长度，不写数据。
 *
 * @param fmt 以 '\0' 结尾的格式串。
 *
 * @param args 与格式说明符类型、顺序严格匹配的可变参数列表。
 *
 * @return 完整结果本应输出的字符数，不含结尾 '\0'。返回值可能大于或等于 size，
 *         此时缓冲区内容已被截断；这不是“实际写入字符数”。
 *
 * @details 解析顺序是：普通字符 → `%` 后标志 → 字段宽度 → 精度 → 长度修饰符 →
 *          转换字符。支持 `%c`、`%s`、`%p`、`%%`、`%b`、`%o`、`%x/%X`、
 *          `%d/%i/%u`；long long 支持由配置开启。浮点参数会被取出以保持 va_list
 *          游标同步，但只按未知格式原样输出 `%` 和转换字符，不进行浮点转换。
 *
 * @warning tiny 后端的字符串字段宽度会参与限制读取长度，精度为 0 时也不按标准
 *          printf 的方式截断字符串；不要依赖这些边界行为实现协议格式。
 */
int rt_vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
#ifdef RT_KLIBC_USING_VSNPRINTF_LONGLONG
    unsigned long long num = 0;
#else
    unsigned long num = 0;
#endif /* RT_KLIBC_USING_VSNPRINTF_LONGLONG */
    int i = 0, len = 0;
    char *str = RT_NULL, *end = RT_NULL, c = 0;
    const char *s = RT_NULL;

    rt_uint8_t base = 0;            /* 整数转换所用进制 */
    rt_uint8_t flags = 0;           /* 当前转换项的格式标志位 */
    rt_uint8_t qualifier = 0;       /* 整数长度修饰符 h、hh、l、ll 或 z */
    rt_int32_t field_width = 0;     /* 最小输出字段宽度，-1 表示未指定 */
    int precision = 0;              /* 整数最少位数或字符串最大长度的近似控制 */

    str = buf;
    end = buf + size;

    /* 若指针加法发生回绕，把 end 饱和到最大地址，避免后续边界比较方向颠倒。 */
    if (end < buf)
    {
        end  = ((char *) - 1);
        size = end - buf;
    }

    for (; *fmt ; ++fmt)
    {
        if (*fmt != '%')
        {
            if (str < end)
            {
                *str = *fmt;
            }

            ++ str;
            continue;
        }

        /* 读取可重复、顺序任意的 - + 空格 # 0 标志。 */
        flags = 0;

        while (1)
        {
            /* 第一次递增跨过 '%'，后续递增跨过已识别的标志。 */
            ++fmt;
            if (*fmt == '-') flags |= LEFT;
            else if (*fmt == '+') flags |= PLUS;
            else if (*fmt == ' ') flags |= SPACE;
            else if (*fmt == '#') flags |= SPECIAL;
            else if (*fmt == '0') flags |= ZEROPAD;
            else break;
        }

        /* 字段宽度可直接写数字，也可由 '*' 从下一个 int 参数取得。 */
        field_width = -1;
        if (_ISDIGIT(*fmt))
        {
            field_width = skip_atoi(&fmt);
        }
        else if (*fmt == '*')
        {
            ++fmt;
            /* 负宽度等价于正宽度加左对齐标志。 */
            field_width = va_arg(args, int);
            if (field_width < 0)
            {
                field_width = -field_width;
                flags |= LEFT;
            }
        }

        /* '.' 后的精度同样支持常量数字或 '*' 参数。 */
        precision = -1;
        if (*fmt == '.')
        {
            ++fmt;
            if (_ISDIGIT(*fmt))
            {
                precision = skip_atoi(&fmt);
            }
            else if (*fmt == '*')
            {
                ++fmt;
                /* 星号精度由下一个 int 参数提供。 */
                precision = va_arg(args, int);
            }
            if (precision < 0)
            {
                precision = 0;
            }
        }

        qualifier = 0; /* 解析整数长度修饰符。 */

        if (*fmt == 'h' || *fmt == 'l' ||
#ifdef RT_KLIBC_USING_VSNPRINTF_LONGLONG
            *fmt == 'L' ||
#endif /* RT_KLIBC_USING_VSNPRINTF_LONGLONG */
            *fmt == 'z')
        {
            qualifier = *fmt;
            ++fmt;
#ifdef RT_KLIBC_USING_VSNPRINTF_LONGLONG
            if (qualifier == 'l' && *fmt == 'l')
            {
                qualifier = 'L';
                ++fmt;
            }
#endif /* RT_KLIBC_USING_VSNPRINTF_LONGLONG */
            if (qualifier == 'h' && *fmt == 'h')
            {
                qualifier = 'H';
                ++fmt;
            }
        }

        /* 整数默认使用十进制，具体 case 可改成二、八或十六进制。 */
        base = 10;

        switch (*fmt)
        {
        case 'c':
            if (!(flags & LEFT))
            {
                while (--field_width > 0)
                {
                    if (str < end) *str = ' ';
                    ++ str;
                }
            }

            /* 可变参数中的 char 会发生整数提升，因此按 int 取出再截成 8 位。 */
            c = (rt_uint8_t)va_arg(args, int);
            if (str < end)
            {
                *str = c;
            }
            ++ str;

            /* LEFT 置位时，字段剩余宽度在字符之后补空格。 */
            while (--field_width > 0)
            {
                if (str < end) *str = ' ';
                ++ str;
            }
            continue;

        case 's':
            s = va_arg(args, char *);
            if (!s)
            {
                s = "(null)";
            }

            for (len = 0; (len != field_width) && (s[len] != '\0'); len++);

            if (precision > 0 && len > precision)
            {
                len = precision;
            }

            if (!(flags & LEFT))
            {
                while (len < field_width--)
                {
                    if (str < end) *str = ' ';
                    ++ str;
                }
            }

            for (i = 0; i < len; ++i)
            {
                if (str < end) *str = *s;
                ++ str;
                ++ s;
            }

            while (len < field_width--)
            {
                if (str < end) *str = ' ';
                ++ str;
            }
            continue;

        case 'p':
            if (field_width == -1)
            {
                field_width = sizeof(void *) << 1;
                field_width += 2; /* 默认指针宽度还要计入 `0x` 前缀。 */
                flags |= SPECIAL;
                flags |= ZEROPAD;
            }
            str = print_number(str, end, (unsigned long)va_arg(args, void *),
                               16, qualifier, field_width, precision, flags);
            continue;

        case '%':
            if (str < end)
            {
                *str = '%';
            }
            ++ str;
            continue;

        /* 整数转换项只在这里设定进制和标志，随后统一读取参数并调用 print_number()。 */
        case 'b':
            base = 2;
            break;
        case 'o':
            base = 8;
            break;

        case 'X':
            flags |= LARGE;
        case 'x':
            base = 16;
            break;

        case 'd':
        case 'i':
            flags |= SIGN;
        case 'u':
            break;

        case 'e':
        case 'E':
        case 'G':
        case 'g':
        case 'f':
        case 'F':
            va_arg(args, double);
        default:
            if (str < end)
            {
                *str = '%';
            }
            ++ str;

            if (*fmt)
            {
                if (str < end)
                {
                    *str = *fmt;
                }
                ++ str;
            }
            else
            {
                -- fmt;
            }
            continue;
        }

        if (qualifier == 'L')
        {
            num = va_arg(args, unsigned long long);
        }
        else if (qualifier == 'l')
        {
            num = va_arg(args, unsigned long);
        }
        else if (qualifier == 'H')
        {
            num = (rt_uint8_t)va_arg(args, rt_int32_t);
            if (flags & SIGN)
            {
                num = (rt_int8_t)num;
            }
        }
        else if (qualifier == 'h')
        {
            num = (rt_uint16_t)va_arg(args, rt_int32_t);
            if (flags & SIGN)
            {
                num = (rt_int16_t)num;
            }
        }
        else if (qualifier == 'z')
        {
            num = va_arg(args, size_t);
            if (flags & SIGN)
            {
                num = (rt_ssize_t)num;
            }
        }
        else
        {
            num = (rt_uint32_t)va_arg(args, unsigned long);
        }
        str = print_number(str, end, num, base, qualifier, field_width, precision, flags);
    }

    if (size > 0)
    {
        if (str < end)
        {
            *str = '\0';
        }
        else
        {
            end[-1] = '\0';
        }
    }

    /* 结尾 '\0' 不计入返回长度；str 始终表示完整结果的逻辑末尾。
    * ++str;
    */
    return str - buf;
}
#if (defined(__GNUC__) && !defined(__ARMCC_VERSION) /* GCC */) && (__GNUC__ >= 7)
#pragma GCC diagnostic pop /* 恢复贯穿诊断设置。 */
#endif /* (defined(__GNUC__) && !defined(__ARMCC_VERSION)) && (__GNUC__ >= 7 */
