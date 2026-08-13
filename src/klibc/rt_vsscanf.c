/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 修改记录：
 * 日期           作者                说明
 * 2024-11-24     Meco Man            移植到 Klibc
 * 2025-01-04     Meco Man            采用 Phoenix 版本
 */

/*
 * Copyright 2017, 2022-2023 Phoenix Systems
 * Author: Adrian Kepka, Gerard Swiderski
 */

#include <rtthread.h>

/**
 * @file rt_vsscanf.c
 * @brief 从内存字符串解析格式化数据的内置 vsscanf 后端。
 *
 * 主流程分为两层：rt_vsscanf() 为 `%[...]` 的字符集合表申请 256 字节临时内存；
 * scanf_parse() 再同步推进格式串和输入串，按转换说明符写入 va_list 指向的目标。
 * 每个转换都经历“解析抑制/长度/宽度 → 判断是否跳过空白 → 收集词法单元 → 转换并
 * 赋值”。整数先收集到 32 字节局部缓冲区，再调用 strtoll/strtoull；浮点直接调用
 * strtof/strtod/strtold。
 *
 * 本实现依赖工具链的 `<stdlib.h>` 和 `<ctype.h>`，并且无条件使用 rt_malloc()，所以
 * 内置后端实际要求启用系统堆。它不是中断上下文友好的纯栈函数：是否允许在中断中
 * 分配还受 RT_USING_HEAP_ISR 和堆锁策略约束。无堆系统应改选 libc 后端。
 *
 * 返回语义与部分 libc 存在一个重要差异：第一次数字或浮点转换匹配不到输入时，
 * 此实现可能返回 -1，而标准库通常把普通“匹配失败”记为 0。调用者若跨后端运行，
 * 应把非正值统一视为“没有完成任何赋值”，不要只判断是否等于 0。
 */

#include <stdlib.h> /* 提供 strtof、strtod、strtold、strtoll 和 strtoull。 */
#include <ctype.h>  /* 提供 isspace，用于格式串和输入串的空白规则。 */
#include <stdarg.h> /* 提供 va_list 及 va_arg。 */

#define FORMAT_NIL_STR     "(nil)"
#define FORMAT_NIL_STR_LEN (sizeof(FORMAT_NIL_STR) - 1)

#define LONG       0x01   /* l：整数为 long，浮点目标为 double */
#define LONGDOUBLE 0x02   /* L：浮点目标为 long double */
#define SHORT      0x04   /* h：整数目标为 short */
#define SUPPRESS   0x08   /* *：只匹配和消耗输入，不执行赋值 */
#define POINTER    0x10   /* p：按十六进制解析并写入 void * */
#define NOSKIP     0x20   /* [ 或 c：转换前不自动跳过空白 */
#define LONGLONG   0x400  /* ll：long long；也接受旧式 q/j */
#define PTRDIFF    0x800  /* t：目标为 ptrdiff_t */
#define SHORTSHORT 0x4000 /* hh：目标为 char */
#define UNSIGNED   0x8000 /* o、u、p、x、X：按无符号整数转换 */

#define SIGNOK     0x40  /* 当前仍允许读取开头的正负号 */
#define NDIGITS    0x80  /* 到目前为止还没有读到有效数字 */
#define PFXOK      0x100 /* 当前仍允许读取十六进制 0x 前缀 */
#define NZDIGITS   0x200 /* 到目前为止还没有读到非零数字 */

#define CT_CHAR    0 /* %c：原样字符 */
#define CT_CCL     1 /* %[...]：字符集合 */
#define CT_STRING  2 /* %s：非空白字符串 */
#define CT_INT     3 /* %d、%i、%o、%u、%p、%x、%X：整数 */
#define CT_FLOAT   4 /* %a、%e、%f、%g 及其大写形式：浮点 */
#define CT_NONE    5 /* 不进入通用转换分支，例如 %n */

/**
 * @brief 解析 `%[...]` 中的字符集合，生成 256 项查找表。
 *
 * @param tab 输出表；下标是 unsigned char 值，非零表示该字符允许匹配。
 * @param fmt 指向左方括号之后的第一个字符。
 * @return 指向闭合 `]` 之后的位置；若格式串提前结束，则指向终止符。
 *
 * @details 开头 `^` 表示取反。普通字符直接置位；`a-z` 形式在右端不小于左端时
 *          展开为闭区间。开头出现的 `]` 可作为集合成员，而不是立即结束集合。
 */

static const unsigned char *__sccl(char *tab, const unsigned char *fmt)
{
    int c, n, v;

    c = *fmt++;
    if (c == '^') {
        v = 1;
        c = *fmt++;
    }
    else {
        v = 0;
    }

    rt_memset(tab, (uint8_t)v, 256);

    if (c == 0) {
        return (fmt - 1);
    }

    v = 1 - v;
    tab[c] = v;
    for (;;) {
        n = *fmt++;
        switch (n) {

            case 0:
                return (fmt - 1);

            case '-':
                n = *fmt;
                if ((n == ']') || (n < c)) {
                    c = '-';
                    tab[c] = v;
                    break;
                }
                fmt++;

                do {
                    tab[++c] = v;
                } while (c < n);
                c = n;
                break;

            case ']':
                return (fmt);

            default:
                c = n;
                tab[c] = v;
                break;
        }
    }
}

/**
 * @brief 完成 vsscanf 的核心解析。
 *
 * @param ccltab 调用者提供的 256 字节字符集合工作表。
 * @param inp 输入 C 字符串。
 * @param inr 输出剩余输入字符数；进入函数后会先初始化为 rt_strlen(inp)。
 * @param fmt0 格式字符串。
 * @param ap 目标指针组成的可变参数列表。
 * @return 已成功赋值的参数个数；第一次转换前输入耗尽或某些转换匹配失败时返回 -1。
 *
 * nassigned 只统计真正赋值的项目，带 `*` 的抑制项和 `%n` 不计入；nconversions
 * 记录已经完成的转换，用于区分首次失败；nread 记录格式解析到当前位置消耗的输入
 * 数量，供 `%n` 写回。注意当前“抑制赋值的整数”路径不向局部 buf 写字符，却仍在
 * 后面通过 p 检查最后字符并计算长度；p 可能未初始化或沿用旧值，这是现有实现的
 * 配置缺口。使用内置后端时不要使用 `%*d` 等抑制数字转换。任何目标缓冲区容量也
 * 都由格式宽度和调用者保证。
 */
static int scanf_parse(char *ccltab, const char *inp, int *inr, char const *fmt0, va_list ap)
{
    const unsigned char *fmt = (const unsigned char *)fmt0;
    int c, n, flags, nassigned, nconversions, nread, base;
    rt_size_t width;
    char *p, *p0;
    char buf[32];

    static const short basefix[17] = { 10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

    *inr = rt_strlen(inp);

    /* 三个计数器含义不同：赋值数是返回值，转换数用于决定失败返回，读取数供 %n。 */
    nassigned = 0;
    nconversions = 0;
    nread = 0;
    base = 0;
    for (;;) {
        int convType = CT_NONE;
        c = *fmt++;
        if (c == '\0') {
            return (nassigned);
        }

        if (isspace(c) != 0) {
            /* 格式串中的任意一个空白匹配输入串中的零个或多个连续空白。 */
            while ((*inr > 0) && (isspace((int)*inp) != 0)) {
                nread++;
                (*inr)--;
                inp++;
            }
            continue;
        }

        if (c != '%') {
            /* 普通格式字符必须和输入当前位置逐字相等，否则发生匹配失败。 */
            if (*inr <= 0) {
                return (nconversions != 0 ? nassigned : -1);
            }

            if (*inp != c) {
                return nassigned;
            }

            nread++;
            (*inr)--;
            inp++;
            continue;
        }

        width = 0;
        flags = 0;
        for (;;) {
            c = *fmt++;
            if (c == '\0') {
                return nassigned;
            }

            if (c == '%') {
                /* `%%` 不取参数，只要求输入中也出现一个字面量 '%'。 */
                if (*inr <= 0) {
                    return (nconversions != 0 ? nassigned : -1);
                }

                if (*inp != c) {
                    return nassigned;
                }

                nread++;
                (*inr)--;
                inp++;
                break;
            }

            switch (c) {
                case '*':
                    flags |= SUPPRESS;
                    continue;

                case 'l':
                    if ((flags & LONG) != 0) {
                        flags &= ~LONG;
                        flags |= LONGLONG;
                    }
                    else {
                        flags |= LONG;
                    }
                    continue;

                case 'L':
                    flags |= LONGDOUBLE;
                    continue;

                case 'q':
                case 'j':
                    flags |= LONGLONG;
                    continue;

                case 't':
                    flags |= PTRDIFF;
                    continue;

                case 'z':
                    if (sizeof(rt_size_t) == sizeof(uint64_t)) {
                        flags |= LONGLONG;
                    }
                    continue;

                case 'h':
                    if ((flags & SHORT) != 0) {
                        flags &= ~SHORT;
                        flags |= SHORTSHORT;
                    }
                    else {
                        flags |= SHORT;
                    }
                    continue;

                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case '9':
                    width = width * 10 + c - '0';
                    continue;
                default:
                    break;
            }

            /* 长度、抑制和宽度解析完成，确定真正的转换类型及整数进制。 */
            switch (c) {
                case 'd':
                    convType = CT_INT;
                    base = 10;
                    break;

                case 'i':
                    convType = CT_INT;
                    base = 0;
                    break;

                case 'o':
                    convType = CT_INT;
                    flags |= UNSIGNED;
                    base = 8;
                    break;

                case 'u':
                    convType = CT_INT;
                    flags |= UNSIGNED;
                    base = 10;
                    break;

                case 'X':
                case 'x':
                    flags |= PFXOK; /* 允许输入带 0x/0X 前缀。 */
                    convType = CT_INT;
                    flags |= UNSIGNED;
                    base = 16;
                    break;

                case 'A':
                case 'E':
                case 'F':
                case 'G':
                case 'a':
                case 'e':
                case 'f':
                case 'g':
                    convType = CT_FLOAT;
                    break;


                case 's':
                    convType = CT_STRING;
                    break;

                case '[':
                    fmt = __sccl(ccltab, fmt);
                    flags |= NOSKIP;
                    convType = CT_CCL;
                    break;

                case 'c':
                    flags |= NOSKIP;
                    convType = CT_CHAR;
                    break;

                case 'p':
                    flags |= POINTER | PFXOK | UNSIGNED;
                    convType = CT_INT;
                    base = 16;
                    break;

                case 'n':
                    nconversions++;
                    if ((flags & SUPPRESS) != 0) {
                        break;
                    }
                    if ((flags & SHORTSHORT) != 0) {
                        *va_arg(ap, char *) = nread;
                    }
                    else if ((flags & SHORT) != 0) {
                        *va_arg(ap, short *) = nread;
                    }
                    else if ((flags & LONG) != 0) {
                        *va_arg(ap, long *) = nread;
                    }
                    else if ((flags & LONGLONG) != 0) {
                        *va_arg(ap, long long *) = nread;
                    }
                    else if ((flags & PTRDIFF) != 0) {
                        *va_arg(ap, ptrdiff_t *) = nread;
                    }
                    else {
                        *va_arg(ap, int *) = nread;
                    }
                    break;

                default:
                    /* 未识别的转换字符：停止解析并返回此前的赋值数。 */
                    return nassigned;
            }

            break;
        }

        if (convType == CT_NONE) {
            continue;
        }

        if (*inr <= 0) {
            return (nconversions != 0 ? nassigned : -1);
        }

        if ((flags & NOSKIP) == 0) {
            /* 除 %c 和 %[...] 外，转换前自动丢弃输入前导空白。 */
            while (isspace((int)*inp) != 0) {
                nread++;
                if (--(*inr) > 0) {
                    inp++;
                }
                else {
                    return (nconversions != 0 ? nassigned : -1);
                }
            }
        }

        /* 前置空白规则处理完毕，按照转换类型消耗输入并按需写回目标。 */
        switch (convType) {
            case CT_CHAR:
                /* %c 精确复制 width 个原始字符，不附加 '\0'；缺省宽度为 1。 */
                if (width == 0) {
                    width = 1;
                }

                if (*inr <= 0) {
                    return (nconversions != 0 ? nassigned : -1);
                }

                if (width > *inr) {
                    width = *inr;
                }

                if ((flags & SUPPRESS) == 0) {
                    rt_memcpy(va_arg(ap, char *), inp, width);
                    nassigned++;
                }

                *inr -= width;
                inp += width;
                nread += width;
                nconversions++;
                break;

            case CT_CCL:
                /* 查表连续接收集合内字符；非抑制模式在结果尾部附加 '\0'。 */
                if (width == 0) {
                    width = (rt_size_t)~0;
                }
                if ((flags & SUPPRESS) != 0) {
                    n = 0;
                    while (ccltab[(unsigned char)*inp] != 0) {
                        n++;
                        (*inr)--;
                        inp++;
                        if (--width == 0) {
                            break;
                        }
                        if (*inr <= 0) {
                            if (n == 0) {
                                return (nconversions != 0 ? nassigned : -1);
                            }
                            break;
                        }
                    }
                    if (n == 0) {
                        return nassigned;
                    }
                }
                else {
                    p0 = p = va_arg(ap, char *);
                    while (ccltab[(unsigned char)*inp] != 0) {
                        (*inr)--;
                        *p++ = *inp++;
                        if (--width == 0) {
                            break;
                        }
                        if (*inr <= 0) {
                            if (p == p0) {
                                return (nconversions != 0 ? nassigned : -1);
                            }
                            break;
                        }
                    }
                    n = p - p0;
                    if (n == 0) {
                        return nassigned;
                    }
                    *p = 0;
                    nassigned++;
                }
                nread += n;
                nconversions++;
                break;

            case CT_STRING:
                /* %s 读取到下一个空白为止，并在非抑制模式下写入终止符。 */
                if (width == 0) {
                    width = (rt_size_t)~0;
                }
                if ((flags & SUPPRESS) != 0) {
                    while (isspace((int)*inp) == 0) {
                        nread++;
                        (*inr)--;
                        inp++;
                        if (--width == 0) {
                            break;
                        }
                        if (*inr <= 0) {
                            break;
                        }
                    }
                }
                else {
                    p0 = p = va_arg(ap, char *);
                    while (isspace((int)*inp) == 0) {
                        (*inr)--;
                        *p++ = *inp++;
                        if (--width == 0) {
                            break;
                        }
                        if (*inr <= 0) {
                            break;
                        }
                    }
                    *p = 0;
                    nread += p - p0;
                    nassigned++;
                }
                nconversions++;
                continue;

            case CT_INT:
                /* 指针格式额外接受常见的 `(nil)` 文本作为空指针。 */
                if (((flags & POINTER) != 0) && ((*inr) >= FORMAT_NIL_STR_LEN) && (rt_strncmp(FORMAT_NIL_STR, inp, FORMAT_NIL_STR_LEN) == 0)) {
                    *va_arg(ap, void **) = RT_NULL;
                    nassigned++;
                    nconversions++;
                    nread += FORMAT_NIL_STR_LEN;
                    inp += FORMAT_NIL_STR_LEN;
                    (*inr) -= FORMAT_NIL_STR_LEN;
                    break;
                }

                if (--width > (sizeof(buf) - 2)) {
                    /* 非抑制整数必须装入 32 字节局部数组，保留符号和终止符空间。 */
                    width = sizeof(buf) - 2;
                }
                width++;

                if ((flags & SUPPRESS) != 0) {
                    width = ~0;
                }

                flags |= SIGNOK | NDIGITS | NZDIGITS;
                /* 逐字符验证符号、前缀和进制数字，遇到第一个不合法字符即停止。 */
                for (p = buf; width; width--) {
                    int ok = 0;
                    c = *inp;
                    switch (c) {
                        case '0':
                            if (base == 0) {
                                base = 8;
                                flags |= PFXOK;
                            }
                            if ((flags & NZDIGITS) != 0) {
                                flags &= ~(SIGNOK | NZDIGITS | NDIGITS);
                            }
                            else {
                                flags &= ~(SIGNOK | PFXOK | NDIGITS);
                            }
                            ok = 1;
                            break;

                        case '1':
                        case '2':
                        case '3':
                        case '4':
                        case '5':
                        case '6':
                        case '7':
                            base = basefix[base];
                            flags &= ~(SIGNOK | PFXOK | NDIGITS);
                            ok = 1;
                            break;

                        case '8':
                        case '9':
                            base = basefix[base];
                            if (base <= 8) {
                                break; /* 八进制中 8、9 非法，结束当前整数。 */
                            }
                            flags &= ~(SIGNOK | PFXOK | NDIGITS);
                            ok = 1;
                            break;

                        case 'A':
                        case 'B':
                        case 'C':
                        case 'D':
                        case 'E':
                        case 'F':
                        case 'a':
                        case 'b':
                        case 'c':
                        case 'd':
                        case 'e':
                        case 'f':
                            if (base <= 10) {
                                break;
                            }
                            flags &= ~(SIGNOK | PFXOK | NDIGITS);
                            ok = 1;
                            break;

                        case '+':
                        case '-':
                            if ((flags & SIGNOK) != 0) {
                                flags &= ~SIGNOK;
                                ok = 1;
                            }
                            break;

                        case 'x':
                        case 'X':
                            if (((flags & PFXOK) != 0) && (p == buf + 1)) {
                                base = 16; /* 对 %i 而言，0x 把自动进制切换为十六进制。 */
                                flags &= ~PFXOK;
                                ok = 1;
                            }
                            break;
                    }
                    if (!ok)
                        break;

                    if ((flags & SUPPRESS) == 0) {
                        *p++ = c;
                    }
                    if (--(*inr) > 0) {
                        inp++;
                    }
                    else {
                        break;
                    }
                }
                if ((flags & NDIGITS) != 0) {
                    return (nconversions != 0 ? nassigned : -1);
                }

                c = ((unsigned char *)p)[-1];
                if ((c == 'x') || (c == 'X')) {
                    --p;
                    inp--;
                    (*inr)++;
                }

                if ((flags & SUPPRESS) == 0) {
                    uint64_t res;

                    /* 词法检查已结束，再借助 libc 完成数值转换和目标宽度写回。 */
                    *p = 0;
                    if ((flags & UNSIGNED) == 0) {
                        res = strtoll(buf, (char **)RT_NULL, base);
                    }
                    else {
                        res = strtoull(buf, (char **)RT_NULL, base);
                    }
                    if ((flags & POINTER) != 0) {
                        *va_arg(ap, void **) = (void *)(unsigned long)res;
                    }
                    else if ((flags & SHORTSHORT) != 0) {
                        *va_arg(ap, char *) = res;
                    }
                    else if ((flags & SHORT) != 0) {
                        *va_arg(ap, short *) = res;
                    }
                    else if ((flags & LONG) != 0) {
                        *va_arg(ap, long *) = res;
                    }
                    else if ((flags & LONGLONG) != 0) {
                        *va_arg(ap, long long *) = res;
                    }
                    else if ((flags & PTRDIFF) != 0) {
                        *va_arg(ap, ptrdiff_t *) = res;
                    }
                    else {
                        *va_arg(ap, int *) = res;
                    }
                    nassigned++;
                }

                nread += p - buf;
                nconversions++;
                break;

            case CT_FLOAT: {
                /* 浮点使用联合体保存三种目标精度，再按长度修饰符选择 strto*。 */
                union {
                    float f;
                    double d;
                    long double ld;
                } res;

                const char *srcbuf = inp;
                if ((width != 0) && (width < *inr)) {
                    /* 当前局部缓冲区只支持最多 31 个字符的显式宽度浮点词法单元。 */
                    if (width > (sizeof(buf) - 1)) {
                        return (nconversions != 0 ? nassigned : -1);
                    }

                    rt_memcpy(buf, inp, width);
                    buf[width] = '\0';
                    srcbuf = buf;
                }

                int is_zero;
                if ((flags & LONGDOUBLE) != 0) {
                    res.ld = strtold(srcbuf, &p);
                    is_zero = res.ld == 0;
                }
                else if ((flags & LONG) != 0) {
                    res.d = strtod(srcbuf, &p);
                    is_zero = res.d == 0;
                }
                else {
                    res.f = strtof(srcbuf, &p);
                    is_zero = res.f == 0;
                }

                if (is_zero && (srcbuf == p)) {
                    return (nconversions != 0 ? nassigned : -1);
                }

                int consumed = p - srcbuf;
                *inr -= consumed;
                inp += consumed;
                nread += consumed;
                nconversions++;
                if ((flags & SUPPRESS) == 0) {
                    if ((flags & LONGDOUBLE) != 0) {
                        *va_arg(ap, long double *) = res.ld;
                    }
                    else if ((flags & LONG) != 0) {
                        *va_arg(ap, double *) = res.d;
                    }
                    else {
                        *va_arg(ap, float *) = res.f;
                    }

                    nassigned++;
                }

                break;
            }

            default:
                break;
        }
    }
    /* 外层无限循环只能通过上面的 return 退出，正常情况下不会到达这里。 */
}

/**
 * @brief 按 @p format 从字符串 @p str 中读取数据。
 *
 * @param str 以 '\0' 结尾的输入字符串。
 * @param format scanf 风格格式字符串。
 * @param ap 指向各输出对象的 va_list，类型必须和格式说明符完全匹配。
 * @return 成功赋值的项目数；工作表分配失败或首次转换的部分失败时返回 -1。
 *
 * @details 256 字节工作表只供 `%[...]` 使用，但为了保持核心解析函数简单，每次调用
 *          都会申请。解析结束后无论成功与否都会释放；分配失败则不会读取输入或参数。
 * @warning `%s` 和 `%[...]` 会在目标末尾写 '\0'；若格式中没有合适的最大宽度，
 *          本函数不知道目标数组大小，可能发生溢出。`%c` 不自动追加终止符。
 */
int rt_vsscanf(const char *str, const char *format, va_list ap)
{
    int ret, nremain;
    char *ccltab = rt_malloc(256);

    if (ccltab == RT_NULL) {
        return -1;
    }

    ret = scanf_parse(ccltab, str, &nremain, format, ap);
    rt_free(ccltab);

    return ret;
}
