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
 * @file kstring.h
 * @brief Kernel byte-memory and NUL-terminated string operations.
 *
 * These routines provide libc-like behavior under stable RT-Thread names so
 * common kernel code does not depend on a particular application C library.
 * Kconfig selects among user-provided, libc, tiny, and RT-Thread generic
 * implementations where each particular operation offers those choices; the
 * available alternatives are not identical for every function.  For example,
 * only a subset has a tiny implementation, and rt_strdup() additionally
 * depends on heap support.  User replacements must preserve the contracts in
 * this header because callers cannot distinguish the selected backend.
 *
 * Except for rt_strdup(), these functions neither allocate memory nor acquire
 * kernel objects.  They perform no pointer, object-size, or address-space
 * validation.  The caller owns every input/output region and must keep it
 * accessible for the entire operation.  Byte counts and string capacities
 * are measured in bytes, not characters in a multibyte encoding.
 */

#ifndef __RT_KSTRING_H__
#define __RT_KSTRING_H__

/* Supplies fixed/native-width types, size_t, RT_NULL, and C/C++ portability. */
#include <rttypes.h>

#ifdef __cplusplus
/* Expose the same unmangled C symbols to C++ components. */
extern "C" {
#endif

/**
 * @brief Fill a memory region with repeated copies of one byte.
 *
 * @p c is converted to `unsigned char` and that value is written to each of
 * the first @p n C bytes starting at @p s.  This wording remains valid on a
 * target whose `unsigned char` is wider than eight bits.  Alignment is not
 * required by the API; a selected optimized backend handles it internally.
 *
 * @param s Start of a writable region of at least @p n bytes when n is nonzero.
 * @param c Integer converted to `unsigned char` before replication.
 * @param n Number of bytes to write; zero performs no byte writes.
 * @return The original @p s pointer, allowing call chaining.
 */
void *rt_memset(void *s, int c, size_t n);

/**
 * @brief Copy a non-overlapping byte range.
 *
 * Exactly @p n bytes are copied from @p src to @p dest.  Source and destination
 * must not overlap according to the memcpy contract, even if a particular
 * tiny/user backend happens to tolerate overlap.  Use rt_memmove() whenever
 * the regions can intersect.
 *
 * @param dest Start of a writable destination region of at least @p n bytes.
 * @param src Start of a readable source region of at least @p n bytes.
 * @param n Number of bytes to copy; no NUL/string semantics are implied.
 * @return The original @p dest pointer.
 */
void *rt_memcpy(void *dest, const void *src, size_t n);

/**
 * @brief Copy a byte range correctly even when source and destination overlap.
 *
 * The result is as if all @p n source bytes were first copied to temporary
 * storage and then to @p dest; the implementation normally obtains that
 * effect by choosing forward or backward traversal without allocation.
 *
 * @param dest Start of a writable destination region of at least @p n bytes.
 * @param src Start of a readable source region of at least @p n bytes.
 * @param n Number of bytes to move.
 * @return The original @p dest pointer.
 */
void *rt_memmove(void *dest, const void *src, size_t n);

/**
 * @brief Lexicographically compare two byte regions.
 *
 * Bytes are interpreted as unsigned values.  Comparison stops at the first
 * differing byte or after @p count bytes; embedded zeros have no special
 * meaning.
 *
 * @param cs First readable region of at least @p count bytes.
 * @param ct Second readable region of at least @p count bytes.
 * @param count Maximum/exact number of bytes considered.
 * @return A value less than, equal to, or greater than zero according as the
 *         first differing byte in @p cs is less than, equal to, or greater
 *         than the corresponding byte in @p ct.  Do not depend on magnitude.
 */
int rt_memcmp(const void *cs, const void *ct, size_t count);

/**
 * @brief Allocate and return an independent copy of a string.
 *
 * The built-in definition is linked only when `RT_USING_HEAP` is enabled.  It
 * allocates `rt_strlen(s) + 1` bytes with rt_malloc(), copies the terminating
 * NUL, and transfers ownership of the new allocation to the caller.  The
 * caller must eventually release a non-NULL result with rt_free() and must
 * obey the configured heap's thread/interrupt-context restrictions.
 *
 * @param s Readable NUL-terminated source string; it must remain valid until
 *        the copy completes.
 * @return Newly allocated writable duplicate, or RT_NULL if allocation fails.
 */
char *rt_strdup(const char *s);

/**
 * @brief Determine a string length subject to an upper bound.
 *
 * The returned count excludes the terminating NUL and never exceeds
 * @p maxlen.  A return equal to @p maxlen does not prove that storage is
 * terminated.  Note a current built-in-backend quirk: its loop tests `*s`
 * before testing the bound, so it can read the byte at `s[maxlen]`, and even a
 * zero bound still evaluates `*s`.  Until that implementation is corrected,
 * provide a readable byte through that position; a user-supplied backend can
 * have different evaluation details.
 *
 * @param s String/buffer whose prefix is inspected.
 * @param maxlen Maximum length reported.
 * @return Number of non-NUL bytes before the first NUL or @p maxlen.
 */
size_t rt_strnlen(const char *s, size_t maxlen);

/**
 * @brief Find the first occurrence of one string within another.
 *
 * Both inputs must be NUL-terminated.  An empty @p str2 matches at the start
 * of @p str1.  Although the input is const-qualified, the historical API
 * returns a mutable pointer into @p str1; modifying through it is valid only
 * if the original storage was actually writable.
 *
 * @param str1 NUL-terminated haystack string.
 * @param str2 NUL-terminated needle string.
 * @return Pointer to the first matching byte in @p str1, or RT_NULL if absent.
 */
char *rt_strstr(const char *str1, const char *str2);

/**
 * @brief Compare two NUL-terminated strings without ASCII letter case.
 *
 * The built-in implementation folds only bytes `A` through `Z` to `a`
 * through `z`; it is locale-independent and does not perform Unicode or
 * multibyte case folding.  A user replacement should document any broader
 * locale behavior it introduces.
 *
 * @param a First readable NUL-terminated string.
 * @param b Second readable NUL-terminated string.
 * @return Negative, zero, or positive according as folded @p a sorts before,
 *         equals, or sorts after folded @p b.  Do not depend on magnitude.
 */
int rt_strcasecmp(const char *a, const char *b);

/**
 * @brief Copy a complete NUL-terminated string to caller-owned storage.
 *
 * The destination must have at least `rt_strlen(src) + 1` writable bytes.
 * There is no capacity argument or truncation; insufficient space overflows
 * the destination.  Source and destination must not overlap.
 *
 * @param dst Writable destination large enough for source bytes and final NUL.
 * @param src Readable NUL-terminated source string.
 * @return The original @p dst pointer.
 */
char *rt_strcpy(char *dst, const char *src);

/**
 * @brief Copy at most @p n bytes with strncpy-compatible padding semantics.
 *
 * If @p src ends before @p n bytes, the remainder of @p dest is filled with
 * NUL bytes.  If the source length is at least @p n, exactly @p n non-NUL
 * bytes may be copied and the destination is *not* NUL-terminated.  This is
 * fixed-field copying rather than a guaranteed-termination helper.  Regions
 * must not overlap.
 *
 * @param dest Writable destination region of at least @p n bytes.
 * @param src Readable NUL-terminated source, or readable for at least @p n
 *        bytes when no earlier NUL is present.
 * @param n Destination field size and maximum bytes copied.
 * @return The original @p dest pointer.
 */
char *rt_strncpy(char *dest, const char *src, size_t n);

/**
 * @brief Compare at most @p count bytes of two NUL-terminated strings.
 *
 * Comparison stops at the first differing byte, a NUL, or the count limit.
 * A zero count compares no characters and returns equality.  For ordinary
 * ASCII text, the sign of a nonzero result gives lexical order.  The built-in
 * backend subtracts plain `char` values, so ordering of bytes at or above 0x80
 * can differ with char signedness or from a libc/user backend; equality (zero)
 * remains the portable test for arbitrary high-bit data.
 *
 * @param cs First string, readable through the compared prefix.
 * @param ct Second string, readable through the compared prefix.
 * @param count Maximum number of bytes considered.
 * @return Negative, zero, or positive for the selected backend's ordering.
 */
int rt_strncmp(const char *cs, const char *ct, size_t count);

/**
 * @brief Lexicographically compare two complete NUL-terminated strings.
 *
 * @param cs First readable NUL-terminated string.
 * @param ct Second readable NUL-terminated string.
 * For ordinary ASCII text, the sign gives lexical order.  The built-in backend
 * subtracts plain `char` values, so high-bit-byte ordering can vary with target
 * char signedness and backend; use a zero result for portable equality.
 *
 * @return Negative, zero, or positive for the selected backend's ordering.
 */
int rt_strcmp(const char *cs, const char *ct);

/**
 * @brief Return the number of bytes before a string's terminating NUL.
 *
 * The terminator is not included.  There is no scan bound: if @p src is not
 * NUL-terminated within readable memory, the function reads beyond the object
 * and behavior is undefined.  The result counts bytes, not encoded characters.
 *
 * @param src Readable NUL-terminated string.
 * @return Number of non-NUL bytes preceding the terminator.
 */
size_t rt_strlen(const char *src);

#ifdef __cplusplus
}
#endif

#endif
