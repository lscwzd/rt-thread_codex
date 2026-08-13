/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 修改记录：
 * 日期           作者         说明
 * 2021-11-27     Meco Man     移植为功能完整的 rt_vsnprintf 后端
 * 2024-11-19     Meco Man     移入 Klibc
 */

/**
 * @author (c) Eyal Rozenberg <eyalroz1@gmx.com>
 *             2021-2022, Haifa, Palestine/Israel
 * @author (c) Marco Paland (info@paland.com)
 *             2014-2019, PALANDesign Hannover, Germany
 *
 * @note 其他贡献者见 https://github.com/eyalroz/printf/graphs/contributors。
 * 指数格式的原始实现由 Martijn Jasperse <m.jasperse@gmail.com> 贡献。
 *
 * @brief 面向资源受限嵌入式系统的独立 printf 家族实现。
 *
 * @note 实现不使用动态内存，也不依赖标准库格式化函数；所有可变状态均在调用栈或
 * 输出器对象中，因此不同调用之间可重入。若回调输出器指向共享设备，设备自身的
 * 并发保护仍由上层负责。
 *
 * @license The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <rtthread.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>

/**
 * @file rt_vsnprintf_std.c
 * @brief RT-Thread 内置的完整格式化输出后端。
 *
 * 文件采用“输出器（output gadget）+ 类型转换器 + 格式串解析器”三层结构：解析器
 * 识别每个 `%` 项，整数/浮点转换器生成字符，输出器统一负责写入上限、逻辑长度
 * 统计和结尾 '\0'。即使缓冲区太小，输出器的 pos 仍继续增长，所以最终返回的是
 * 完整结果所需长度，调用者可据此判断截断或重新分配缓冲区。
 */

/* 整数反向转换的栈缓冲区，必须容纳单个数字及精度产生的前导零。 */
#ifndef RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE
#define RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE    32
#endif

/* 单个十进制浮点片段使用的固定栈缓冲区，也要容纳精度补零。 */
#ifndef RT_KLIBC_USING_VSNPRINTF_DECIMAL_BUFFER_SIZE
#define RT_KLIBC_USING_VSNPRINTF_DECIMAL_BUFFER_SIZE    32
#endif

/* 启用普通十进制浮点转换说明符 %f、%F。 */
#ifndef RT_KLIBC_USING_VSNPRINTF_DECIMAL_SPECIFIERS
#define RT_KLIBC_USING_VSNPRINTF_DECIMAL_SPECIFIERS
#endif

/* 启用指数/自适应浮点转换说明符 %e、%g、%E、%G。 */
#ifndef RT_KLIBC_USING_VSNPRINTF_EXPONENTIAL_SPECIFIERS
#define RT_KLIBC_USING_VSNPRINTF_EXPONENTIAL_SPECIFIERS
#endif

/* 启用把已输出字符数写回指针的 %n；格式串不可信时应注意该写内存能力。 */
#ifndef RT_KLIBC_USING_VSNPRINTF_WRITEBACK_SPECIFIER
#define RT_KLIBC_USING_VSNPRINTF_WRITEBACK_SPECIFIER
#endif

/* 浮点默认精度；C 标准规定为 6。 */
#ifndef RT_KLIBC_USING_VSNPRINTF_FLOAT_PRECISION
#define RT_KLIBC_USING_VSNPRINTF_FLOAT_PRECISION  6
#endif

/*
 * 标准 %f 可能为极大整数部分输出数百个字符。为限制嵌入式实现的栈空间和执行时间，
 * 整数位超过此阈值的值会改用指数形式。
 */
#ifndef RT_KLIBC_USING_VSNPRINTF_MAX_INTEGRAL_DIGITS_FOR_DECIMAL
#define RT_KLIBC_USING_VSNPRINTF_MAX_INTEGRAL_DIGITS_FOR_DECIMAL 9
#endif

/*
 * 为 d/i/o/x/X/u/p 启用 long long 及 ll、z、t 长度修饰符。
 * 注意：这里的 L（long double）仍不受支持。
 */
#ifndef RT_KLIBC_USING_VSNPRINTF_LONGLONG
#define RT_KLIBC_USING_VSNPRINTF_LONGLONG
#endif

/* log10 近似所用泰勒展开项数，包含展开点处的零次项。项数越多通常越精确但更耗时。 */
#ifndef RT_KLIBC_USING_VSNPRINTF_LOG10_TAYLOR_TERMS
#define RT_KLIBC_USING_VSNPRINTF_LOG10_TAYLOR_TERMS 4
#endif

/* 调试构建或显式配置时，解析 `%` 项过程中持续检查格式串是否提前结束。 */
#if !defined(RT_KLIBC_USING_VSNPRINTF_CHECK_NUL_IN_FORMAT_SPECIFIER) || defined(RT_USING_DEBUG)
#define RT_KLIBC_USING_VSNPRINTF_CHECK_NUL_IN_FORMAT_SPECIFIER
#endif

#if RT_KLIBC_USING_VSNPRINTF_LOG10_TAYLOR_TERMS <= 1
#error "At least one non-constant Taylor expansion is necessary for the log10() calculation"
#endif

///////////////////////////////////////////////////////////////////////////////

#define PRINTF_PREFER_DECIMAL     false
#define PRINTF_PREFER_EXPONENTIAL true

/* 两级宏先展开“最大十进制整数位数”，再拼成形如 1e9 的浮点常量。 */
#define PRINTF_CONCATENATE(s1, s2) s1##s2
#define PRINTF_EXPAND_THEN_CONCATENATE(s1, s2) PRINTF_CONCATENATE(s1, s2)
#define PRINTF_FLOAT_NOTATION_THRESHOLD PRINTF_EXPAND_THEN_CONCATENATE(1e,RT_KLIBC_USING_VSNPRINTF_MAX_INTEGRAL_DIGITS_FOR_DECIMAL)

/* 单个转换项的内部状态位；解析器组合它们，具体转换器再消费。 */
#define FLAGS_ZEROPAD   (1U <<  0U)
#define FLAGS_LEFT      (1U <<  1U)
#define FLAGS_PLUS      (1U <<  2U)
#define FLAGS_SPACE     (1U <<  3U)
#define FLAGS_HASH      (1U <<  4U)
#define FLAGS_UPPERCASE (1U <<  5U)
#define FLAGS_CHAR      (1U <<  6U)
#define FLAGS_SHORT     (1U <<  7U)
#define FLAGS_INT       (1U <<  8U)
/* FLAGS_LONG 仅供 MSVC 风格整数说明符使用。 */
#define FLAGS_LONG      (1U <<  9U)
#define FLAGS_LONG_LONG (1U << 10U)
#define FLAGS_PRECISION (1U << 11U)
#define FLAGS_ADAPT_EXP (1U << 12U)
#define FLAGS_POINTER   (1U << 13U)
/* FLAGS_POINTER 与 FLAGS_HASH 都产生前缀，但指针格式还有固定语义。 */
#define FLAGS_SIGNED    (1U << 14U)
/* FLAGS_SIGNED 仅供 MSVC 风格整数说明符使用。 */

#ifdef RT_KLIBC_USING_VSNPRINTF_MSVC_STYLE_INTEGER_SPECIFIERS

#define FLAGS_INT8 FLAGS_CHAR

#if   (SHRT_MAX   == 32767LL)
#define FLAGS_INT16       FLAGS_SHORT
#elif (INT_MAX    == 32767LL)
#define FLAGS_INT16       FLAGS_INT
#elif (LONG_MAX   == 32767LL)
#define FLAGS_INT16       FLAGS_LONG
#elif (LLONG_MAX  == 32767LL)
#define FLAGS_INT16       FLAGS_LONG_LONG
#else
#error "No basic integer type has a size of 16 bits exactly"
#endif

#if   (SHRT_MAX   == 2147483647LL)
#define FLAGS_INT32       FLAGS_SHORT
#elif (INT_MAX    == 2147483647LL)
#define FLAGS_INT32       FLAGS_INT
#elif (LONG_MAX   == 2147483647LL)
#define FLAGS_INT32       FLAGS_LONG
#elif (LLONG_MAX  == 2147483647LL)
#define FLAGS_INT32       FLAGS_LONG_LONG
#else
#error "No basic integer type has a size of 32 bits exactly"
#endif

#if   (SHRT_MAX   == 9223372036854775807LL)
#define FLAGS_INT64       FLAGS_SHORT
#elif (INT_MAX    == 9223372036854775807LL)
#define FLAGS_INT64       FLAGS_INT
#elif (LONG_MAX   == 9223372036854775807LL)
#define FLAGS_INT64       FLAGS_LONG
#elif (LLONG_MAX  == 9223372036854775807LL)
#define FLAGS_INT64       FLAGS_LONG_LONG
#else
#error "No basic integer type has a size of 64 bits exactly"
#endif

#endif /* RT_KLIBC_USING_VSNPRINTF_MSVC_STYLE_INTEGER_SPECIFIERS */


typedef unsigned int printf_flags_t;

#define BASE_BINARY    2
#define BASE_OCTAL     8
#define BASE_DECIMAL  10
#define BASE_HEX      16

typedef uint8_t numeric_base_t;

#ifdef RT_KLIBC_USING_VSNPRINTF_LONGLONG
typedef unsigned long long printf_unsigned_value_t;
typedef long long          printf_signed_value_t;
#else
typedef unsigned long printf_unsigned_value_t;
typedef long          printf_signed_value_t;
#endif

/*
 * printf 家族返回 int，因此宽度、精度、输出位置等最终可报告的非负量统一用
 * unsigned int，而不是通常更宽的 size_t。入口处会把过大的缓冲区容量饱和到
 * INT_MAX，避免最终返回值无法表示。
 */
typedef unsigned int printf_size_t;
#define PRINTF_MAX_POSSIBLE_BUFFER_SIZE INT_MAX
  /* 严格说容量可再包含一个终止符位置，但返回长度本身仍不能超过 INT_MAX。 */

#if defined(RT_KLIBC_USING_VSNPRINTF_DECIMAL_SPECIFIERS) || defined(RT_KLIBC_USING_VSNPRINTF_EXPONENTIAL_SPECIFIERS)
#include <float.h>
#if FLT_RADIX != 2
/* cppcheck：这里有意用预处理错误拒绝非二进制浮点表示。 */
#error "Non-binary-radix floating-point types are unsupported."
#endif

#if DBL_MANT_DIG == 24

#define DOUBLE_SIZE_IN_BITS 32
typedef uint32_t double_uint_t;
#define DOUBLE_EXPONENT_MASK 0xFFU
#define DOUBLE_BASE_EXPONENT 127
#define DOUBLE_MAX_SUBNORMAL_EXPONENT_OF_10 -38
#define DOUBLE_MAX_SUBNORMAL_POWER_OF_10 1e-38

#elif DBL_MANT_DIG == 53

#define DOUBLE_SIZE_IN_BITS 64
typedef uint64_t double_uint_t;
#define DOUBLE_EXPONENT_MASK 0x7FFU
#define DOUBLE_BASE_EXPONENT 1023
#define DOUBLE_MAX_SUBNORMAL_EXPONENT_OF_10 -308
#define DOUBLE_MAX_SUBNORMAL_POWER_OF_10 ((double)1e-308L)

#else
#error "Unsupported double type configuration"
#endif
#define DOUBLE_STORED_MANTISSA_BITS (DBL_MANT_DIG - 1)

typedef union {
  double_uint_t U;
  double        F;
} double_with_bit_access;

/*
 * 把 double 装入联合体以读取 IEEE-754 位模式。单独使用辅助函数可兼容对复合字面量
 * 支持不一致的编译器，也方便把代码移植到较旧 C 或 C++ 环境。
 */
static inline double_with_bit_access get_bit_access(double x)
{
  double_with_bit_access dwba;
  dwba.F = x;
  return dwba;
}

static inline int get_sign_bit(double x)
{
  /* IEEE-754 符号位位于最高位。 */
  return (int) (get_bit_access(x).U >> (DOUBLE_SIZE_IN_BITS - 1));
}

static inline int get_exp2(double_with_bit_access x)
{
  /* 指数域紧邻符号位并采用偏置编码；掩码取出后减去偏置，得到近似二进制指数。 */
  return (int)((x.U >> DOUBLE_STORED_MANTISSA_BITS ) & DOUBLE_EXPONENT_MASK) - DOUBLE_BASE_EXPONENT;
}
#define PRINTF_ABS(_x) ( (_x) > 0 ? (_x) : -(_x) )

#endif /* 已启用任一种浮点转换 */

/*
 * 先转换到对应无符号类型再取得幅值，使 LONG_MIN/LLONG_MIN 也能表示；直接对最小
 * 有符号数做普通一元负号可能溢出并产生未定义行为。
 */
#define ABS_FOR_PRINTING(_x) ((printf_unsigned_value_t) ( (_x) > 0 ? (_x) : -((printf_signed_value_t)_x) ))

/*
 * 统一输出器：既可写固定缓冲区，也可逐字符调用回调，还可仅计数而丢弃输出。
 * 必须满足以下至少一项：max_chars 为 0、buffer 非空、function 非空。
 */
typedef struct {
  void (*function)(char c, void* extra_arg); /**< 可选逐字符输出回调。 */
  void* extra_function_arg;                 /**< 原样传给回调的用户上下文。 */
  char* buffer;                             /**< 回调为空时使用的目标缓冲区。 */
  printf_size_t pos;                        /**< 完整结果的逻辑位置，截断后仍递增。 */
  printf_size_t max_chars;                  /**< 最多可触达的缓冲区/回调字符数。 */
} output_gadget_t;

/*
 * 向输出器追加一个字符。格式化主体不会传入终止 '\0'；终止符由专用函数处理。
 * 无论容量是否已满都递增 pos，容量判断只决定是否真的写入。
 */
static inline void putchar_via_gadget(output_gadget_t* gadget, char c)
{
  printf_size_t write_pos = gadget->pos++;
    /* 始终递增逻辑位置，统计在没有 max_chars 限制时本应输出的字符数。 */
  if (write_pos >= gadget->max_chars) {
    return;
  }
  if (gadget->function != NULL) {
    /* 回调模式不额外过滤 '\0'；当前格式化主体保证不会传入。 */
    gadget->function(c, gadget->extra_function_arg);
  }
  else {
    /* 由输出器不变量可知，此时 buffer 必须非空。 */
    gadget->buffer[write_pos] = c;
  }
}

/* 缓冲区模式下在实际可写范围内追加或回退覆盖一个字符串终止符。 */
static inline void append_termination_with_gadget(output_gadget_t* gadget)
{
  if (gadget->function != NULL || gadget->max_chars == 0) {
    return;
  }
  if (gadget->buffer == NULL) {
    return;
  }
  printf_size_t null_char_pos = gadget->pos < gadget->max_chars ? gadget->pos : gadget->max_chars - 1;
  gadget->buffer[null_char_pos] = '\0';
}

static inline output_gadget_t discarding_gadget(void)
{
  output_gadget_t gadget;
  gadget.function = NULL;
  gadget.extra_function_arg = NULL;
  gadget.buffer = NULL;
  gadget.pos = 0;
  gadget.max_chars = 0;
  return gadget;
}

static inline output_gadget_t buffer_gadget(char* buffer, size_t buffer_size)
{
  printf_size_t usable_buffer_size = (buffer_size > PRINTF_MAX_POSSIBLE_BUFFER_SIZE) ?
    PRINTF_MAX_POSSIBLE_BUFFER_SIZE : (printf_size_t) buffer_size;
  output_gadget_t result = discarding_gadget();
  if (buffer != NULL) {
    result.buffer = buffer;
    result.max_chars = usable_buffer_size;
  }
  return result;
}

/*
 * 最多检查 maxsize 个字符的内部字符串长度函数，返回值不含 '\0'。这里所有消费者
 * 都使用 printf_size_t，故无需引入通常更宽的 size_t 返回类型。
 */
static inline printf_size_t strnlen_s_(const char* str, printf_size_t maxsize)
{
  const char* s;
  for (s = str; *s && maxsize--; ++s);
  return (printf_size_t)(s - str);
}


/* 判断字符是否为 ASCII 十进制数字 0～9。 */
static inline bool is_digit_(char ch)
{
  return (ch >= '0') && (ch <= '9');
}


/* 从格式串游标解析无符号十进制数；返回时游标停在首个非数字字符。 */
static printf_size_t atou_(const char** str)
{
  printf_size_t i = 0U;
  while (is_digit_(**str)) {
    i = i * 10U + (printf_size_t)(*((*str)++) - '0');
  }
  return i;
}


/*
 * 把逆序临时数组按正常顺序输出，并按宽度处理左右空格。整数提取数字时从最低位
 * 开始，因此 buf 需要逆序读取；前导零已由上层提前放入逆序数组。
 */
static void out_rev_(output_gadget_t* output, const char* buf, printf_size_t len, printf_size_t width, printf_flags_t flags)
{
  const printf_size_t start_pos = output->pos;

  /* 右对齐且不补零时，数字之前先补空格。 */
  if (!(flags & FLAGS_LEFT) && !(flags & FLAGS_ZEROPAD)) {
    for (printf_size_t i = len; i < width; i++) {
      putchar_via_gadget(output, ' ');
    }
  }

  /* 从临时数组末尾向前读，恢复人类阅读的高位到低位顺序。 */
  while (len) {
    putchar_via_gadget(output, buf[--len]);
  }

  /* 左对齐时，在数字之后补足字段宽度。 */
  if (flags & FLAGS_LEFT) {
    while (output->pos - start_pos < width) {
      putchar_via_gadget(output, ' ');
    }
  }
}


/*
 * 整数数字已逆序生成后，补齐精度、字段零、进制前缀和符号，再调用 out_rev_()。
 * 由于整个临时数组最终会反向输出，这里追加前缀时也按反序放置。
 */
static void print_integer_finalization(output_gadget_t* output, char* buf, printf_size_t len, bool negative, numeric_base_t base, printf_size_t precision, printf_size_t width, printf_flags_t flags)
{
  printf_size_t unpadded_len = len;

  /* 在逆序数组尾部追加的零，最终会成为数字前导零。 */
  {
    if (!(flags & FLAGS_LEFT)) {
      if (width && (flags & FLAGS_ZEROPAD) && (negative || (flags & (FLAGS_PLUS | FLAGS_SPACE)))) {
        width--;
      }
      while ((flags & FLAGS_ZEROPAD) && (len < width) && (len < RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE)) {
        buf[len++] = '0';
      }
    }

    while ((len < precision) && (len < RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE)) {
      buf[len++] = '0';
    }

    if (base == BASE_OCTAL && (len > unpadded_len)) {
      /* 八进制已有前导零时，# 模式要求已经满足，不必再追加一个 0。 */
      flags &= ~FLAGS_HASH;
    }
  }

  /* 处理 # 替代格式或指针格式所需的 0、0x、0X、0b 前缀。 */
  if (flags & (FLAGS_HASH | FLAGS_POINTER)) {
    if (!(flags & FLAGS_PRECISION) && len && ((len == precision) || (len == width))) {
      /* 必要时收回一两个仅用于宽度的补零，为最终前缀让出字段空间。 */
      if (unpadded_len < len) {
        len--; /* 八进制前缀只需要一个字符。 */
      }
      if (len && (base == BASE_HEX || base == BASE_BINARY) && (unpadded_len < len)) {
        len--; /* 十六进制或二进制前缀还需要第二个字符。 */
      }
    }
    if ((base == BASE_HEX) && !(flags & FLAGS_UPPERCASE) && (len < RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE)) {
      buf[len++] = 'x';
    }
    else if ((base == BASE_HEX) && (flags & FLAGS_UPPERCASE) && (len < RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE)) {
      buf[len++] = 'X';
    }
    else if ((base == BASE_BINARY) && (len < RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE)) {
      buf[len++] = 'b';
    }
    if (len < RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE) {
      buf[len++] = '0';
    }
  }

  if (len < RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE) {
    if (negative) {
      buf[len++] = '-';
    }
    else if (flags & FLAGS_PLUS) {
      buf[len++] = '+';  /* '+' 的优先级高于空格标志。 */
    }
    else if (flags & FLAGS_SPACE) {
      buf[len++] = ' ';
    }
  }

  out_rev_(output, buf, len, width, flags);
}

/*
 * 类似 itoa 的内部整数转换：反复除以进制取得最低位，写入逆序栈数组，再统一补齐
 * 符号、前缀、精度和宽度。negative 已由调用者从原有符号数中分离。
 */
static void print_integer(output_gadget_t* output, printf_unsigned_value_t value, bool negative, numeric_base_t base, printf_size_t precision, printf_size_t width, printf_flags_t flags)
{
  char buf[RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE];
  printf_size_t len = 0U;

  if (!value) {
    if ( !(flags & FLAGS_PRECISION) ) {
      buf[len++] = '0';
      flags &= ~FLAGS_HASH;
      /* 零值的普通/替代形式不再需要额外前缀；八进制的特殊零也已经生成。 */
    }
    else if (base == BASE_HEX) {
      flags &= ~FLAGS_HASH;
      /* 十六进制零值不输出额外 0x 前缀。 */
    }
  }
  else {
    do {
      const char digit = (char)(value % base);
      buf[len++] = (char)(digit < 10 ? '0' + digit : (flags & FLAGS_UPPERCASE ? 'A' : 'a') + digit - 10);
      value /= base;
    } while (value && (len < RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE));
  }

  print_integer_finalization(output, buf, len, negative, base, precision, width, flags);
}

#if defined(RT_KLIBC_USING_VSNPRINTF_DECIMAL_SPECIFIERS) || defined(RT_KLIBC_USING_VSNPRINTF_EXPONENTIAL_SPECIFIERS)

/* 按调用者给定精度拆开的 double；单看结构本身无法知道 fractional 的缩放倍数。 */
struct double_components {
  int_fast64_t integral;   /**< 整数部分的绝对值。 */
  int_fast64_t fractional; /**< 小数部分截断后乘以 10^precision 的整数值。 */
  bool is_negative;        /**< 原始数是否带负号，包括可识别的负零。 */
};

#define NUM_DECIMAL_DIGITS_IN_INT64_T 18
#define PRINTF_MAX_PRECOMPUTED_POWER_OF_10  NUM_DECIMAL_DIGITS_IN_INT64_T
static const double powers_of_10[NUM_DECIMAL_DIGITS_IN_INT64_T] = {
  1e00, 1e01, 1e02, 1e03, 1e04, 1e05, 1e06, 1e07, 1e08,
  1e09, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17
};

#define PRINTF_MAX_SUPPORTED_PRECISION NUM_DECIMAL_DIGITS_IN_INT64_T - 1


/*
 * 按指定十进制精度把有限 double 拆成整数部分和经缩放的小数部分，并完成舍入。
 * 精度本身不会在函数内修改；负号独立保存在结果结构中。
 */
static struct double_components get_components(double number, printf_size_t precision)
{
  struct double_components number_;
  number_.is_negative = get_sign_bit(number);
  double abs_number = (number_.is_negative) ? -number : number;
  number_.integral = (int_fast64_t)abs_number;
  double remainder = (abs_number - (double) number_.integral) * powers_of_10[precision];
  number_.fractional = (int_fast64_t)remainder;

  remainder -= (double) number_.fractional;

  if (remainder > 0.5) {
    ++number_.fractional;
    /* 处理舍入进位，例如 0.99 在精度 1 时应成为 1.0。 */
    if ((double) number_.fractional >= powers_of_10[precision]) {
      number_.fractional = 0;
      ++number_.integral;
    }
  }
  else if ((remainder == 0.5) && ((number_.fractional == 0U) || (number_.fractional & 1U))) {
    /* 正好位于中点时，根据末位奇偶及零值规则决定向上舍入。 */
    ++number_.fractional;
  }

  if (precision == 0U) {
    remainder = abs_number - (double) number_.integral;
    if ((!(remainder < 0.5) || (remainder > 0.5)) && (number_.integral & 1)) {
      /* 精度为 0 时执行偶数舍入：1.5→2，而 2.5→2。 */
      ++number_.integral;
    }
  }
  return number_;
}

#ifdef RT_KLIBC_USING_VSNPRINTF_EXPONENTIAL_SPECIFIERS
struct scaling_factor {
  double raw_factor; /**< 缩放因子的绝对数值。 */
  bool multiply;     /**< 为真时乘以 raw_factor，否则除以它。 */
};

/* 把缩放策略应用到数值；乘/除方向分开可降低极端 10 的幂直接溢出的机会。 */

static double apply_scaling(double num, struct scaling_factor normalization)
{
  return normalization.multiply ? num * normalization.raw_factor : num / normalization.raw_factor;
}

static double unapply_scaling(double normalized, struct scaling_factor normalization)
{
#if defined(__GNUC__) && !defined(__clang__) && !defined(__ARMCC_VERSION) /* GCC */
/* 规避 GCC 6 及更早版本的静态分析误报。 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  return normalization.multiply ? normalized / normalization.raw_factor : normalized * normalization.raw_factor;
#if defined(__GNUC__) && !defined(__clang__) && !defined(__ARMCC_VERSION) /* GCC */
#pragma GCC diagnostic pop
#endif
}

static struct scaling_factor update_normalization(struct scaling_factor sf, double extra_multiplicative_factor)
{
  struct scaling_factor result;
  if (sf.multiply) {
    result.multiply = true;
    result.raw_factor = sf.raw_factor * extra_multiplicative_factor;
  }
  else {
    int factor_exp2 = get_exp2(get_bit_access(sf.raw_factor));
    int extra_factor_exp2 = get_exp2(get_bit_access(extra_multiplicative_factor));

    /* 用指数绝对值较大的因子除以较小者，尽量让中间结果保持可表示。 */
    if (PRINTF_ABS(factor_exp2) > PRINTF_ABS(extra_factor_exp2)) {
      result.multiply = false;
      result.raw_factor = sf.raw_factor / extra_multiplicative_factor;
    }
    else {
      result.multiply = true;
      result.raw_factor = extra_multiplicative_factor / sf.raw_factor;
    }
  }
  return result;
}

static struct double_components get_normalized_components(bool negative, printf_size_t precision, double non_normalized, struct scaling_factor normalization, int floored_exp10)
{
  struct double_components components;
  components.is_negative = negative;
  double scaled = apply_scaling(non_normalized, normalization);

  bool close_to_representation_extremum = ( (-floored_exp10 + (int) precision) >= DBL_MAX_10_EXP - 1 );
  if (close_to_representation_extremum) {
    /*
     * 接近 double 表示极限时，不能再把精度对应的 10 的幂并入归一化因子，否则因子
     * 本身可能不可表示；此时退回普通拆分，牺牲部分额外有效位。
     */
    return get_components(negative ? -scaled : scaled, precision);
  }
  components.integral = (int_fast64_t) scaled;
  double remainder = non_normalized - unapply_scaling((double) components.integral, normalization);
  double prec_power_of_10 = powers_of_10[precision];
  struct scaling_factor account_for_precision = update_normalization(normalization, prec_power_of_10);
  double scaled_remainder = apply_scaling(remainder, account_for_precision);
  double rounding_threshold = 0.5;

  components.fractional = (int_fast64_t) scaled_remainder; /* 精度为 0 时应得到 0。 */
  scaled_remainder -= (double) components.fractional; /* 精度为 0 时余数保持原值。 */

  components.fractional += (scaled_remainder >= rounding_threshold);
  if (scaled_remainder == rounding_threshold) {
    /* 银行家舍入：正好在中点时向偶数靠拢，降低累计平均误差。 */
    components.fractional &= ~((int_fast64_t) 0x1);
  }
  /*
   * 修正小数舍入溢出，例如 0.99、精度 1 的临时结果 (0,10) 要变为 (1,0)。
   * 精度为 0 时 10^precision 为 1，这一步也会把舍入自然传递给整数部分。
   */
  if ((double) components.fractional >= prec_power_of_10) {
    components.fractional = 0;
    ++components.integral;
  }
  return components;
}
#endif /* RT_KLIBC_USING_VSNPRINTF_EXPONENTIAL_SPECIFIERS */

/*
 * 把已经拆开的十进制数写入逆序缓冲区：先小数、再小数点、再整数、最后符号。
 * %g/%G 可去除尾随零；# 标志则强制保留小数点及精度要求的零。
 */
static void print_broken_up_decimal(
  struct double_components number_, output_gadget_t* output, printf_size_t precision,
  printf_size_t width, printf_flags_t flags, char *buf, printf_size_t len)
{
  if (precision != 0U) {
    /* 小数部分以非负缩放整数处理。 */

    printf_size_t count = precision;

    /* %g/%G 未带 # 时去掉小数末尾无意义的零。 */
    if ((flags & FLAGS_ADAPT_EXP) && !(flags & FLAGS_HASH) && (number_.fractional > 0)) {
      while(true) {
        int_fast64_t digit = number_.fractional % 10U;
        if (digit != 0) {
          break;
        }
        --count;
        number_.fractional /= 10U;

      }
      /* 若去零后没有小数位，下面也不会输出小数点。 */
    }

    if (number_.fractional > 0 || !(flags & FLAGS_ADAPT_EXP) || (flags & FLAGS_HASH) ) {
      while (len < RT_KLIBC_USING_VSNPRINTF_DECIMAL_BUFFER_SIZE) {
        --count;
        buf[len++] = (char)('0' + number_.fractional % 10U);
        if (!(number_.fractional /= 10U)) {
          break;
        }
      }
      /* 小数有效位不足 precision 时补齐零。 */
      while ((len < RT_KLIBC_USING_VSNPRINTF_DECIMAL_BUFFER_SIZE) && (count > 0U)) {
        buf[len++] = '0';
        --count;
      }
      if (len < RT_KLIBC_USING_VSNPRINTF_DECIMAL_BUFFER_SIZE) {
        buf[len++] = '.';
      }
    }
  }
  else {
    if ((flags & FLAGS_HASH) && (len < RT_KLIBC_USING_VSNPRINTF_DECIMAL_BUFFER_SIZE)) {
      buf[len++] = '.';
    }
  }

  /* 整个缓冲区最终逆序输出，所以整数部分在小数部分之后写入临时数组。 */
  while (len < RT_KLIBC_USING_VSNPRINTF_DECIMAL_BUFFER_SIZE) {
    buf[len++] = (char)('0' + (number_.integral % 10));
    if (!(number_.integral /= 10)) {
      break;
    }
  }

  /* 右对齐且启用 0 标志时，用数字前导零补足字段宽度。 */
  if (!(flags & FLAGS_LEFT) && (flags & FLAGS_ZEROPAD)) {
    if (width && (number_.is_negative || (flags & (FLAGS_PLUS | FLAGS_SPACE)))) {
      width--;
    }
    while ((len < width) && (len < RT_KLIBC_USING_VSNPRINTF_DECIMAL_BUFFER_SIZE)) {
      buf[len++] = '0';
    }
  }

  if (len < RT_KLIBC_USING_VSNPRINTF_DECIMAL_BUFFER_SIZE) {
    if (number_.is_negative) {
      buf[len++] = '-';
    }
    else if (flags & FLAGS_PLUS) {
      buf[len++] = '+';  /* '+' 优先于空格标志。 */
    }
    else if (flags & FLAGS_SPACE) {
      buf[len++] = ' ';
    }
  }

  out_rev_(output, buf, len, width, flags);
}

/* 固定十进制浮点输出：先按精度拆分，再交给公共十进制片段输出器。 */
static void print_decimal_number(output_gadget_t* output, double number, printf_size_t precision, printf_size_t width, printf_flags_t flags, char* buf, printf_size_t len)
{
  struct double_components value_ = get_components(number, precision);
  print_broken_up_decimal(value_, output, precision, width, flags, buf, len);
}

#ifdef RT_KLIBC_USING_VSNPRINTF_EXPONENTIAL_SPECIFIERS

/* 只适用于 floor(x) 能由 int 表示的简化向下取整。 */
static int bastardized_floor(double x)
{
  if (x >= 0) { return (int) x; }
  int n = (int) x;
  return ( ((double) n) == x ) ? n : n-1;
}

/*
 * 近似计算正常、有限正数的十进制对数；调用者必须先排除 0、负数、NaN、无穷大和
 * 非规格化数。用途是估计指数格式的十进制指数，并非通用数学库 log10。
 */
static double log10_of_positive(double positive_number)
{
  /*
   * 算法参考 David Gay 的 dtoa。利用 log10(M×2^x)=log10(M)+x×log10(2)，
   * 把尾数归一化到 [1,2)，只需在中点 1.5 附近用少量泰勒项近似 log10(M)。
   */

  double_with_bit_access dwba = get_bit_access(positive_number);
  /* 取出带偏置修正后的二进制指数。 */
  int exp2 = get_exp2(dwba);
  /* 用基准指数替换原指数域，使 dwba.F 落入 [1,2)。 */
  dwba.U = (dwba.U & (((double_uint_t) (1) << DOUBLE_STORED_MANTISSA_BITS) - 1U)) |
           ((double_uint_t) DOUBLE_BASE_EXPONENT << DOUBLE_STORED_MANTISSA_BITS);
  double z = (dwba.F - 1.5);
  return (
    /* 以 1.5 为展开点的泰勒近似： */
    0.1760912590556812420           /* 第 0 项：ln(1.5) / ln(10) */
    + z     * 0.2895296546021678851 /* 第 1 项：(M-1.5)×2/3 / ln(10) */
#if RT_KLIBC_USING_VSNPRINTF_LOG10_TAYLOR_TERMS > 2
    - z*z   * 0.0965098848673892950 /* 第 2 项：(M-1.5)^2×2/9 / ln(10) */
#if RT_KLIBC_USING_VSNPRINTF_LOG10_TAYLOR_TERMS > 3
    + z*z*z * 0.0428932821632841311 /* 第 3 项：(M-1.5)^3×8/81 / ln(10) */
#endif
#endif
    /* 二进制指数经换底公式得到的精确线性项： */
    + exp2 * 0.30102999566398119521 /* exp2×log10(2) */
  );
}


static double pow10_of_int(int floored_exp10)
{
  /* 最小十进制指数单独返回预定义值，避免正常/非规格化边界附近的近似异常。 */
  if (floored_exp10 == DOUBLE_MAX_SUBNORMAL_EXPONENT_OF_10) {
    return DOUBLE_MAX_SUBNORMAL_POWER_OF_10;
  }
  /* 把 10^n 分解成 2^exp2×exp(z)，尽量避免直接幂运算的中间溢出。 */
  double_with_bit_access dwba;
  int exp2 = bastardized_floor(floored_exp10 * 3.321928094887362 + 0.5);
  const double z  = floored_exp10 * 2.302585092994046 - exp2 * 0.6931471805599453;
  const double z2 = z * z;
  dwba.U = ((double_uint_t)(exp2) + DOUBLE_BASE_EXPONENT) << DOUBLE_STORED_MANTISSA_BITS;
  /* 用连分式近似 exp(z)。 */
  dwba.F *= 1 + 2 * z / (2 - z + (z2 / (6 + (z2 / (10 + z2 / 14)))));
  return dwba.F;
}

static void print_exponential_number(output_gadget_t* output, double number, printf_size_t precision, printf_size_t width, printf_flags_t flags, char* buf, printf_size_t len)
{
  const bool negative = get_sign_bit(number);
  /* 提取十进制指数时，abs_number 会按 10 的幂归一化。 */
  double abs_number =  negative ? -number : number;

  int floored_exp10;
  bool abs_exp10_covered_by_powers_table;
  struct scaling_factor normalization;


  /* 估计 floor(log10(abs_number))，再用一次比较修正近似舍入误差。 */
  if (abs_number == 0.0) {
    /* 这里只特判 0.0/-0.0；更一般的非规格化数仍依赖后续近似路径。 */
    floored_exp10 = 0; /* 零无需归一化因子，也无需查询 10 的幂表。 */
  }
  else  {
    double exp10 = log10_of_positive(abs_number);
    floored_exp10 = bastardized_floor(exp10);
    double p10 = pow10_of_int(floored_exp10);
    /* 若近似得到的 10^指数反而大于原数，将指数向下修正一位。 */
    if (abs_number < p10) {
      floored_exp10--;
      p10 /= 10;
    }
    abs_exp10_covered_by_powers_table = PRINTF_ABS(floored_exp10) < PRINTF_MAX_PRECOMPUTED_POWER_OF_10;
    normalization.raw_factor = abs_exp10_covered_by_powers_table ? powers_of_10[PRINTF_ABS(floored_exp10)] : p10;
  }

  /*
   * 输出字段分成归一化十进制部分和指数部分。指数宽度为 0 表示不输出指数；十进制
   * 部分宽度为 0 则表示按实际需要输出，不施加宽度限制。
   */

  bool fall_back_to_decimal_only_mode = false;
  if (flags & FLAGS_ADAPT_EXP) {
    int required_significant_digits = (precision == 0) ? 1 : (int) precision;
    /* %g 在指数位于 [-4, 有效数字数) 时回退为类似 %f 的普通十进制形式。 */
    fall_back_to_decimal_only_mode = (floored_exp10 >= -4 && floored_exp10 < required_significant_digits);
    /* %g 的 precision 表示有效数字数；这里换算为小数点后的真实位数。 */
    int precision_ = fall_back_to_decimal_only_mode ?
                     (int) precision - 1 - floored_exp10 :
        (int) precision - 1; /* 指数形式中小数点前固定只有一位有效数字。 */
    precision = (precision_ > 0 ? (unsigned) precision_ : 0U);
    flags |= FLAGS_PRECISION;   /* 要求下层严格采用刚换算出的精度。 */
  }

  normalization.multiply = (floored_exp10 < 0 && abs_exp10_covered_by_powers_table);
  bool should_skip_normalization = (fall_back_to_decimal_only_mode || floored_exp10 == 0);
  struct double_components decimal_part_components =
    should_skip_normalization ?
    get_components(negative ? -abs_number : abs_number, precision) :
    get_normalized_components(negative, precision, abs_number, normalization, floored_exp10);

  /* 处理 9.99 舍入到 10.0/100.0 一类跨位进位，并同步修正指数和小数位数。 */
  if (fall_back_to_decimal_only_mode) {
    if ((flags & FLAGS_ADAPT_EXP) && floored_exp10 >= -1 && decimal_part_components.integral == powers_of_10[floored_exp10 + 1]) {
      floored_exp10++; /* 回退普通格式后该指数基本不再输出，但仍保持内部一致。 */
      precision--;
      /* 此时 fractional 按舍入逻辑应已经为 0。 */
    }
    /* 尚未单独处理只发生在小数部分内部的其他进位边界。 */
  }
  else {
    if (decimal_part_components.integral >= 10) {
      floored_exp10++;
      decimal_part_components.integral = 1;
      decimal_part_components.fractional = 0;
    }
  }

  /* 指数形如 E+07；两位指数占 4 字符，三位指数（double 最大约 307）占 5 字符。 */
  printf_size_t exp10_part_width = fall_back_to_decimal_only_mode ? 0U : (PRINTF_ABS(floored_exp10) < 100) ? 4U : 5U;

  printf_size_t decimal_part_width =
    ((flags & FLAGS_LEFT) && exp10_part_width) ?
      /* 左对齐意味着最终在指数之后补空格，十进制部分本身无需限制宽度。 */
      0U :
      /* 右对齐时先扣除指数部分，余下宽度交给十进制部分。 */
      ((width > exp10_part_width) ?
        /* 总宽度足够，同时也适用于已回退的 %f 形式。 */
        width - exp10_part_width :
        /* 总宽度连指数都容不下时，不截断数字，按所需长度完整计数输出。 */
        0U);

  const printf_size_t printed_exponential_start_pos = output->pos;
  print_broken_up_decimal(decimal_part_components, output, precision, decimal_part_width, flags, buf, len);

  if (! fall_back_to_decimal_only_mode) {
    putchar_via_gadget(output, (flags & FLAGS_UPPERCASE) ? 'E' : 'e');
    print_integer(output,
                  ABS_FOR_PRINTING(floored_exp10),
                  floored_exp10 < 0, 10, 0, exp10_part_width - 1,
                FLAGS_ZEROPAD | FLAGS_PLUS);
    if (flags & FLAGS_LEFT) {
      /* 左对齐：指数输出完毕后在右侧补空格。 */
      while (output->pos - printed_exponential_start_pos < width) {
        putchar_via_gadget(output, ' ');
      }
    }
  }
}
#endif  /* RT_KLIBC_USING_VSNPRINTF_EXPONENTIAL_SPECIFIERS */

static void print_floating_point(output_gadget_t* output, double value, printf_size_t precision, printf_size_t width, printf_flags_t flags, bool prefer_exponential)
{
  char buf[RT_KLIBC_USING_VSNPRINTF_DECIMAL_BUFFER_SIZE];
  printf_size_t len = 0U;

  /* 特殊值先处理。out_rev_() 要求输入逆序，所以 inf/nan 的字面量也反向存放。 */
  if (value != value) {
    out_rev_(output, "nan", 3, width, flags);
    return;
  }
  if (value < -DBL_MAX) {
    out_rev_(output, "fni-", 4, width, flags);
    return;
  }
  if (value > DBL_MAX) {
    out_rev_(output, (flags & FLAGS_PLUS) ? "fni+" : "fni", (flags & FLAGS_PLUS) ? 4U : 3U, width, flags);
    return;
  }

  if (!prefer_exponential &&
      ((value > PRINTF_FLOAT_NOTATION_THRESHOLD) || (value < -PRINTF_FLOAT_NOTATION_THRESHOLD))) {
    /*
     * 标准 %f 要求输出所有整数位，极端值可能需要数百字符并超过固定内部缓冲区。
     * 本实现超过配置阈值时改走指数格式；若编译时禁用指数支持，该值不会产生数字。
     */
#ifdef RT_KLIBC_USING_VSNPRINTF_EXPONENTIAL_SPECIFIERS
    print_exponential_number(output, value, precision, width, flags, buf, len);
#endif
    return;
  }

  /* 格式串未显式给出精度时采用配置的浮点默认值。 */
  if (!(flags & FLAGS_PRECISION)) {
    precision = RT_KLIBC_USING_VSNPRINTF_FLOAT_PRECISION;
  }

  /*
   * fractional 使用 int64 保存，最多直接计算 17 位小数；超出部分先作为逆序尾随零
   * 放进输出缓冲区，以维持请求的结果长度，再把实际计算精度降到可支持范围。
   */
  while ((len < RT_KLIBC_USING_VSNPRINTF_DECIMAL_BUFFER_SIZE) && (precision > PRINTF_MAX_SUPPORTED_PRECISION)) {
    buf[len++] = '0'; /* 只在输出长度层面满足额外精度，不增加数值有效信息。 */
    precision--;
  }

#ifdef RT_KLIBC_USING_VSNPRINTF_EXPONENTIAL_SPECIFIERS
  if (prefer_exponential)
    print_exponential_number(output, value, precision, width, flags, buf, len);
  else
#endif
    print_decimal_number(output, value, precision, width, flags, buf, len);
}

#endif  /* 已启用任一种浮点转换 */

/*
 * 连续读取 0、-、+、空格、# 标志，推进格式串游标并返回位集合。标志允许重复，
 * 重复置位没有额外效果；遇到第一个非标志字符停止。
 */
static printf_flags_t parse_flags(const char** format)
{
  printf_flags_t flags = 0U;
  do {
    switch (**format) {
      case '0': flags |= FLAGS_ZEROPAD; (*format)++; break;
      case '-': flags |= FLAGS_LEFT;    (*format)++; break;
      case '+': flags |= FLAGS_PLUS;    (*format)++; break;
      case ' ': flags |= FLAGS_SPACE;   (*format)++; break;
      case '#': flags |= FLAGS_HASH;    (*format)++; break;
      default : return flags;
    }
  } while (true);
}

/**
 * @brief 解析完整格式串并把转换结果送入统一输出器。
 *
 * @param output 已初始化的缓冲区、回调或丢弃输出器。
 * @param format printf 风格格式字符串。
 * @param args 与每个转换说明符严格匹配的参数列表。
 *
 * 每个转换项按 `%[flags][width][.precision][length]specifier` 解析。宽度和精度可由
 * `*` 从 int 参数取得；h/hh、l/ll、j、z、t 控制整数取参类型。普通字符直接输出。
 * 启用安全检查时，ADVANCE_IN_FORMAT_STRING 会在不完整的尾部 `%` 项处立即返回，
 * 避免越过格式串终止符。
 */
static inline void format_string_loop(output_gadget_t* output, const char* format, va_list args)
{
#ifdef RT_KLIBC_USING_VSNPRINTF_CHECK_NUL_IN_FORMAT_SPECIFIER
#define ADVANCE_IN_FORMAT_STRING(cptr_) do { (cptr_)++; if (!*(cptr_)) return; } while(0)
#else
#define ADVANCE_IN_FORMAT_STRING(cptr_) (cptr_)++
#endif


  while (*format)
  {
    if (*format != '%') {
      /* 普通内容字符不触碰 va_list，直接交给输出器。 */
      putchar_via_gadget(output, *format);
      format++;
      continue;
    }
    /* 开始解析转换项：%[标志][宽度][.精度][长度]转换字符。 */
    ADVANCE_IN_FORMAT_STRING(format);

    printf_flags_t flags = parse_flags(&format);

    /* 宽度为常量数字或 '*' 参数；负的星号宽度表示左对齐。 */
    printf_size_t width = 0U;
    if (is_digit_(*format)) {
      width = (printf_size_t) atou_(&format);
    }
    else if (*format == '*') {
      const int w = va_arg(args, int);
      if (w < 0) {
        flags |= FLAGS_LEFT;    /* 负宽度改成绝对值，并把填充方向反转为右侧。 */
        width = (printf_size_t)-w;
      }
      else {
        width = (printf_size_t)w;
      }
      ADVANCE_IN_FORMAT_STRING(format);
    }

    /* 点号后的精度为数字或 '*' 参数；负/零星号精度在此实现中归为 0。 */
    printf_size_t precision = 0U;
    if (*format == '.') {
      flags |= FLAGS_PRECISION;
      ADVANCE_IN_FORMAT_STRING(format);
      if (is_digit_(*format)) {
        precision = (printf_size_t) atou_(&format);
      }
      else if (*format == '*') {
        const int precision_ = va_arg(args, int);
        precision = precision_ > 0 ? (printf_size_t) precision_ : 0U;
        ADVANCE_IN_FORMAT_STRING(format);
      }
    }

    /* 解析长度修饰符，记录随后 va_arg 应取出的整数宽度。 */
    switch (*format) {
#ifdef RT_KLIBC_USING_VSNPRINTF_MSVC_STYLE_INTEGER_SPECIFIERS
      case 'I' : {
        ADVANCE_IN_FORMAT_STRING(format);
        /* MSVC 风格 I8/I16/I32/I64：贪婪读取位数并映射到实际基础整数类型。 */
        switch(*format) {
          case '8':               flags |= FLAGS_INT8;
            ADVANCE_IN_FORMAT_STRING(format);
            break;
          case '1':
            ADVANCE_IN_FORMAT_STRING(format);
          if (*format == '6') { format++; flags |= FLAGS_INT16; }
            break;
          case '3':
            ADVANCE_IN_FORMAT_STRING(format);
            if (*format == '2') { ADVANCE_IN_FORMAT_STRING(format); flags |= FLAGS_INT32; }
            break;
          case '6':
            ADVANCE_IN_FORMAT_STRING(format);
            if (*format == '4') { ADVANCE_IN_FORMAT_STRING(format); flags |= FLAGS_INT64; }
            break;
          default: break;
        }
        break;
      }
#endif
      case 'l' :
        flags |= FLAGS_LONG;
        ADVANCE_IN_FORMAT_STRING(format);
        if (*format == 'l') {
          flags |= FLAGS_LONG_LONG;
          ADVANCE_IN_FORMAT_STRING(format);
        }
        break;
      case 'h' :
        flags |= FLAGS_SHORT;
        ADVANCE_IN_FORMAT_STRING(format);
        if (*format == 'h') {
          flags |= FLAGS_CHAR;
          ADVANCE_IN_FORMAT_STRING(format);
        }
        break;
      case 't' :
        flags |= (sizeof(ptrdiff_t) == sizeof(long) ? FLAGS_LONG : FLAGS_LONG_LONG);
        ADVANCE_IN_FORMAT_STRING(format);
        break;
      case 'j' :
        flags |= (sizeof(intmax_t) == sizeof(long) ? FLAGS_LONG : FLAGS_LONG_LONG);
        ADVANCE_IN_FORMAT_STRING(format);
        break;
      case 'z' :
        flags |= (sizeof(size_t) == sizeof(long) ? FLAGS_LONG : FLAGS_LONG_LONG);
        ADVANCE_IN_FORMAT_STRING(format);
        break;
      default:
        break;
    }

    /* 长度和格式参数已就绪，按最终转换字符取参并输出。 */
    switch (*format) {
      case 'd' :
      case 'i' :
      case 'u' :
      case 'x' :
      case 'X' :
      case 'o' :
      case 'b' : {

        if (*format == 'd' || *format == 'i') {
          flags |= FLAGS_SIGNED;
        }

        numeric_base_t base;
        if (*format == 'x' || *format == 'X') {
          base = BASE_HEX;
        }
        else if (*format == 'o') {
          base =  BASE_OCTAL;
        }
        else if (*format == 'b') {
          base =  BASE_BINARY;
        }
        else {
          base = BASE_DECIMAL;
          flags &= ~FLAGS_HASH; /* 十进制整数没有 # 替代前缀。 */
        }

        if (*format == 'X') {
          flags |= FLAGS_UPPERCASE;
        }

        format++;
        /* 整数显式精度优先于 0 填充标志。 */
        if (flags & FLAGS_PRECISION) {
          flags &= ~FLAGS_ZEROPAD;
        }

        if (flags & FLAGS_SIGNED) {
          /* 有符号 d/i（以及可选 MSVC 位宽格式）：分离符号后按无符号幅值打印。 */

          if (flags & FLAGS_LONG_LONG) {
#ifdef RT_KLIBC_USING_VSNPRINTF_LONGLONG
            const long long value = va_arg(args, long long);
            print_integer(output, ABS_FOR_PRINTING(value), value < 0, base, precision, width, flags);
#endif
          }
          else if (flags & FLAGS_LONG) {
            const long value = va_arg(args, long);
            print_integer(output, ABS_FOR_PRINTING(value), value < 0, base, precision, width, flags);
          }
          else {
            /*
             * char/short 作为可变参数会先发生整数提升，所以必须按 int 取出，再按长度
             * 标志截回目标宽度；直接用 va_arg(args, short) 是错误的。
             */
            const int value =
              (flags & FLAGS_CHAR) ? (signed char) va_arg(args, int) :
              (flags & FLAGS_SHORT) ? (short int) va_arg(args, int) :
              va_arg(args, int);
            print_integer(output, ABS_FOR_PRINTING(value), value < 0, base, precision, width, flags);
          }
        }
        else {
          /* 无符号 u/x/X/o/b 不接受正号或前导空格。 */

          flags &= ~(FLAGS_PLUS | FLAGS_SPACE);

          if (flags & FLAGS_LONG_LONG) {
#ifdef RT_KLIBC_USING_VSNPRINTF_LONGLONG
            print_integer(output, (printf_unsigned_value_t) va_arg(args, unsigned long long), false, base, precision, width, flags);
#endif
          }
          else if (flags & FLAGS_LONG) {
            print_integer(output, (printf_unsigned_value_t) va_arg(args, unsigned long), false, base, precision, width, flags);
          }
          else {
            const unsigned int value =
              (flags & FLAGS_CHAR) ? (unsigned char)va_arg(args, unsigned int) :
              (flags & FLAGS_SHORT) ? (unsigned short int)va_arg(args, unsigned int) :
              va_arg(args, unsigned int);
            print_integer(output, (printf_unsigned_value_t) value, false, base, precision, width, flags);
          }
        }
        break;
      }
#ifdef RT_KLIBC_USING_VSNPRINTF_DECIMAL_SPECIFIERS
      case 'f' :
      case 'F' :
        if (*format == 'F') flags |= FLAGS_UPPERCASE;
        print_floating_point(output, va_arg(args, double), precision, width, flags, PRINTF_PREFER_DECIMAL);
        format++;
        break;
#endif
#ifdef RT_KLIBC_USING_VSNPRINTF_EXPONENTIAL_SPECIFIERS
      case 'e':
      case 'E':
      case 'g':
      case 'G':
        if ((*format == 'g')||(*format == 'G')) flags |= FLAGS_ADAPT_EXP;
        if ((*format == 'E')||(*format == 'G')) flags |= FLAGS_UPPERCASE;
        print_floating_point(output, va_arg(args, double), precision, width, flags, PRINTF_PREFER_EXPONENTIAL);
        format++;
        break;
#endif  /* RT_KLIBC_USING_VSNPRINTF_EXPONENTIAL_SPECIFIERS */
      case 'c' : {
        printf_size_t l = 1U;
        /* 非左对齐时，在字符前补空格。 */
        if (!(flags & FLAGS_LEFT)) {
          while (l++ < width) {
            putchar_via_gadget(output, ' ');
          }
        }
        /* char 经可变参数整数提升，按 int 取出再转换。 */
        putchar_via_gadget(output, (char) va_arg(args, int) );
        /* 左对齐时，在字符后补空格。 */
        if (flags & FLAGS_LEFT) {
          while (l++ < width) {
            putchar_via_gadget(output, ' ');
          }
        }
        format++;
        break;
      }

      case 's' : {
        const char* p = va_arg(args, char*);
        if (p == NULL) {
          out_rev_(output, ")llun(", 6, width, flags);
        }
        else {
          printf_size_t l = strnlen_s_(p, precision ? precision : PRINTF_MAX_POSSIBLE_BUFFER_SIZE);
          /* 精度限制字符串最大输出长度；右对齐先补空格。 */
          if (flags & FLAGS_PRECISION) {
            l = (l < precision ? l : precision);
          }
          if (!(flags & FLAGS_LEFT)) {
            while (l++ < width) {
              putchar_via_gadget(output, ' ');
            }
          }
          /* 输出至 '\0' 或精度耗尽，二者先到者为止。 */
          while ((*p != 0) && (!(flags & FLAGS_PRECISION) || precision)) {
            putchar_via_gadget(output, *(p++));
            --precision;
          }
          /* 左对齐在字符串之后补齐字段宽度。 */
          if (flags & FLAGS_LEFT) {
            while (l++ < width) {
              putchar_via_gadget(output, ' ');
            }
          }
        }
        format++;
        break;
      }

      case 'p' : {
        width = sizeof(void*) * 2U + 2; /* 每字节两位十六进制，再加 `0x` 前缀。 */
        flags |= FLAGS_ZEROPAD | FLAGS_POINTER;
        uintptr_t value = (uintptr_t)va_arg(args, void*);
        (value == (uintptr_t) NULL) ?
          out_rev_(output, ")lin(", 5, width, flags) :
          print_integer(output, (printf_unsigned_value_t) value, false, BASE_HEX, precision, width, flags);
        format++;
        break;
      }

      case '%' :
        putchar_via_gadget(output, '%');
        format++;
        break;

      /*
       * %n 会把当前逻辑输出长度写入参数指针。若格式串来自不可信输入，这相当于可控
       * 的内存写操作，存在安全风险，因此允许通过配置完全禁用。
       */
#ifdef RT_KLIBC_USING_VSNPRINTF_WRITEBACK_SPECIFIER
      case 'n' : {
        if       (flags & FLAGS_CHAR)      *(va_arg(args, char*))      = (char) output->pos;
        else if  (flags & FLAGS_SHORT)     *(va_arg(args, short*))     = (short) output->pos;
        else if  (flags & FLAGS_LONG)      *(va_arg(args, long*))      = (long) output->pos;
#ifdef RT_KLIBC_USING_VSNPRINTF_LONGLONG
        else if  (flags & FLAGS_LONG_LONG) *(va_arg(args, long long*)) = (long long int) output->pos;
#endif /* RT_KLIBC_USING_VSNPRINTF_LONGLONG */
        else                               *(va_arg(args, int*))       = (int) output->pos;
        format++;
        break;
      }
#endif /* RT_KLIBC_USING_VSNPRINTF_WRITEBACK_SPECIFIER */

      default :
        putchar_via_gadget(output, *format);
        format++;
        break;
    }
  }
}

/*
 * 公共格式化核心：运行格式串循环、为缓冲区模式追加终止符，并返回不含终止符的
 * 完整逻辑长度。内部通常以 pos=0 调用，也允许从非零 pos 继续补救式输出。
 */
static int vsnprintf_impl(output_gadget_t* output, const char* format, va_list args)
{
  /* 本文件入口以 pos=0 调用；设计上也支持已累计一部分位置的输出器。 */
  format_string_loop(output, format, args);

  /* 仅缓冲区输出器需要写字符串终止符；回调和计数模式跳过。 */
  append_termination_with_gadget(output);

  /* 返回值不计结尾 '\0'，且包含因容量不足而未真正写入的字符。 */
  return (int)output->pos;
}

///////////////////////////////////////////////////////////////////////////////

/**
 * @brief 按格式串和 va_list 生成有容量上限的字符串。
 *
 * @param buf 输出缓冲区；size 为 0 时可以为空，仅计算所需长度。
 *
 * @param size 缓冲区总容量，包含结尾 '\0' 的位置。
 *
 * @param fmt 以 '\0' 结尾的 printf 风格格式串。
 *
 * @param args 与格式说明符类型和顺序匹配的可变参数列表。
 *
 * @return 完整结果本应具有的字符数，不含结尾 '\0'；不等同于实际写入数。
 *         若返回值大于或等于 size，表示结果发生截断。
 *
 * @note size 大于 0 且 buf 非空时，函数在可写范围内保证 '\0' 结尾。实现不分配
 *       动态内存，但浮点和整数转换会使用固定大小栈数组。
 * @warning fmt 与 args 不匹配属于未定义用法；启用 `%n` 时，不可信格式串还能触发
 *          写内存。返回值内部按 int 范围设计，超大 size 会饱和到 INT_MAX。
 */
int rt_vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
  output_gadget_t gadget = buffer_gadget(buf, size);
  return vsnprintf_impl(&gadget, fmt, args);
}
