/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-09-22     Meco Man     the first version
 */

/**
 * @file kstdio.h
 * @brief Buffer-oriented formatted output and input for kernel code.
 *
 * This interface mirrors the familiar sprintf/snprintf/sscanf families under
 * RT-Thread's `rt_` namespace.  It performs no stream or console I/O: output
 * is written to caller-owned memory and input is parsed from a caller-supplied
 * NUL-terminated string.
 *
 * The selected implementation is controlled by klibc configuration.  Output
 * may use the compact RT-Thread formatter, the larger standard formatter, or
 * libc `vsnprintf`.  The option sets are not independent: long-long support is
 * a non-libc option and is selected by the standard formatter; floating-point,
 * `%n`, and MSVC-style controls belong to the standard formatter's submenu;
 * the tiny formatter has its own fixed extensions such as `%b`; and a libc
 * backend follows that library.  Scanning may use RT-Thread's parser or libc
 * `vsscanf`.  Portable code must use only the conversion subset available in
 * every target configuration it supports.
 *
 * Format strings are trusted type contracts.  Each conversion must match the
 * promoted variadic argument type, and each scanning destination must have
 * the required pointer type and capacity.  A mismatch is undefined behavior
 * and cannot be detected reliably by these routines.
 */

#ifndef __RT_KSTDIO_H__
#define __RT_KSTDIO_H__

/* RT-Thread scalar types include size_t; stdarg.h defines va_list handling. */
#include <rttypes.h>
#include <stdarg.h>

#ifdef __cplusplus
/* Keep the C ABI for calls emitted by C++ kernel/component code. */
extern "C" {
#endif

/**
 * @brief Format a variadic argument list into an unbounded destination buffer.
 *
 * This is the `va_list` form of rt_sprintf().  It behaves as though the output
 * capacity were effectively unlimited, so the function cannot prevent a
 * destination overflow.  The caller must pre-compute or otherwise guarantee
 * space for every formatted character plus the terminating NUL byte.
 *
 * The function consumes values from @p arg_ptr.  A caller that needs to reuse
 * the list must pass a `va_copy` and later `va_end` that copy as required by
 * the platform ABI.
 *
 * @param dest Writable destination buffer with sufficient capacity.  It must
 *        not overlap storage read through @p format or formatted arguments.
 * @param format NUL-terminated format string supported by the selected backend.
 * @param arg_ptr Argument list whose promoted types must match @p format.
 * @return Number of formatted characters excluding the terminating NUL, or a
 *         negative value if the selected backend reports an encoding/format
 *         failure.
 */
int rt_vsprintf(char *dest, const char *format, va_list arg_ptr);

/**
 * @brief Format a variadic argument list into a size-bounded buffer.
 *
 * At most `size - 1` formatted characters are stored when @p size is nonzero,
 * followed by a NUL terminator.  A zero size stores nothing and provides no
 * terminator.  Backend portability is best when @p buf is a valid pointer even
 * for a zero-size sizing call; a nonzero size always requires writable storage
 * for at least @p size bytes.
 *
 * A nonnegative return is the full number of characters that would have been
 * produced without truncation, excluding the NUL.  Therefore truncation is
 * detected with `(size_t)return_value >= size` after first checking that the
 * return value is nonnegative.  The function never allocates the output buffer.
 *
 * @param buf Destination buffer; writable for @p size bytes when size is nonzero.
 * @param size Total destination capacity including space for the final NUL.
 * @param fmt NUL-terminated format string supported by the selected backend.
 * @param args Argument list matching @p fmt; it is consumed by this call.
 * @return Required formatted length excluding NUL, or a negative backend error.
 */
int rt_vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

/**
 * @brief Format variadic arguments into an unbounded destination buffer.
 *
 * This convenience wrapper creates a `va_list` and delegates to
 * rt_vsprintf().  It has the same overflow risk as sprintf: @p buf must hold
 * the complete result and terminating NUL.  Prefer rt_snprintf() whenever the
 * destination capacity is known.
 *
 * @param buf Writable destination with enough capacity for the complete result.
 * @param format NUL-terminated format string.
 * @param ... Values whose promoted types exactly match the conversions.
 * @return Number of characters written excluding NUL, or a negative backend error.
 */
int rt_sprintf(char *buf, const char *format, ...);

/**
 * @brief Format variadic arguments into a size-bounded destination buffer.
 *
 * This convenience wrapper delegates to rt_vsnprintf().  When @p size is
 * nonzero the result is always NUL-terminated by the selected conforming
 * backend, including when truncation occurs.  The return value describes the
 * untruncated result, not merely the bytes stored.
 *
 * @param buf Destination buffer; writable for @p size bytes when size is nonzero.
 * @param size Total capacity including the terminating NUL.
 * @param format NUL-terminated format string.
 * @param ... Values whose promoted types exactly match the conversions.
 * @return Required formatted length excluding NUL, or a negative backend error.
 */
int rt_snprintf(char *buf, size_t size, const char *format, ...);

/**
 * @brief Parse formatted fields from a string using an existing argument list.
 *
 * Input is consumed only from @p buffer; no device or console read occurs.
 * Whitespace and conversion behavior follow the selected RT-Thread/libc
 * scanner.  Destination pointers in @p ap must match the conversion types.
 * For `%s`, `%c`, and `%[` conversions, use a field width derived from the
 * destination capacity; an omitted/excessive width can overrun the buffer.
 * `%n`, when supported, writes a count and does not itself increase the
 * assignment result.
 *
 * The RT-Thread scanner backend obtains a temporary 256-byte character-class
 * table from the system heap and releases it before returning; failure to
 * allocate that internal table is reported as -1.  Consequently this backend
 * requires RT_USING_HEAP, and its usable execution contexts are limited by the
 * selected heap's locking and RT_USING_HEAP_ISR policy.  A no-heap build must
 * select a libc scanner or provide another compatible implementation.  The
 * libc backend follows its own allocation and locale rules.  No returned input
 * substring is owned by this function.
 *
 * @param buffer NUL-terminated input string that remains readable for the call.
 * @param format NUL-terminated scan format.
 * @param ap Destination-pointer argument list matching @p format; consumed by
 *        this call, so copy it first if it must be reused.
 * @return Backend-reported assignment count or failure.  The built-in scanner
 *         returns zero for some literal matching failures, but returns -1 for
 *         allocation failure, input exhaustion before the first conversion,
 *         and several first-conversion parse failures.  A libc backend follows
 *         that libc's vsscanf contract.
 */
int rt_vsscanf(const char *buffer, const char *format, va_list ap);

/**
 * @brief Parse formatted fields from a string into variadic destinations.
 *
 * This wrapper constructs a `va_list` and delegates to rt_vsscanf().  It has
 * the same destination type, field-width, heap-backend, and return contracts.
 * The input string is not modified.
 *
 * @param str NUL-terminated input string.
 * @param format NUL-terminated scan format.
 * @param ... Writable destination pointers matching each unsuppressed conversion.
 * @return Assignment count or backend-specific failure value; see
 *         rt_vsscanf() for the built-in scanner's zero/-1 distinction.
 */
int rt_sscanf(const char *str, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif
