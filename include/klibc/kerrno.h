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
 * @file kerrno.h
 * @brief RT-Thread error-code namespace and context-local errno interface.
 *
 * RT-Thread uses `RT_E*` names in kernel APIs while optionally interoperating
 * with POSIX/libc code that uses `E*` values and the `errno` lvalue.  In a
 * full non-Nano libc build, most RT-Thread constants are aliases of their
 * closest POSIX values.  Freestanding and Nano selections use a compact private
 * numbering scheme rather than aliases to POSIX constants.  This does not mean
 * every non-Nano build avoids `<sys/errno.h>`: rttypes.h includes that system
 * header for non-Nano profiles even when the compact constants are selected.
 *
 * Error sign is an API-level convention, not a property enforced here.  Many
 * kernel functions return `-RT_E*`, while POSIX interfaces conventionally
 * return -1 and store a positive `E*` value in errno.  `rt_set_errno()` stores
 * a supplied value without sign normalization, and `rt_get_errno()` returns the
 * selected slot's value; callers must follow the convention of the interface
 * they implement.  Values should remain representable by `int`: the interrupt/
 * early-boot slot and the `_rt_errno()` lvalue interface are int-width, which is
 * narrower than rt_err_t on configured 64-bit targets.
 *
 * Errno storage is context-sensitive.  A running thread uses its TCB `error`
 * field, giving normal thread-local behavior.  Interrupt context and early
 * boot/no-current-thread context share a fallback global slot.  Consequently
 * ISR errno is transient shared diagnostic state: a nested/later interrupt or
 * another CPU can replace it, so it must not be used for synchronization or
 * durable error reporting.
 */

#ifndef __RT_KERRNO_H__
#define __RT_KERRNO_H__

/* Feature selection controls POSIX mapping; rttypes.h supplies rt_err_t. */
#include <rtconfig.h>
#include <rttypes.h>

#ifdef __cplusplus
/* Preserve C linkage when the error interface is included by C++ code. */
extern "C" {
#endif

#if defined(RT_USING_LIBC) && !defined(RT_USING_NANO)
/**
 * @name Error values in POSIX-compatible libc builds
 *
 * Aliases retain the host/libc numeric values so errors can cross RT-Thread
 * and POSIX interfaces without a translation table.  RT_ERROR, RT_ETRAP, and
 * the scheduler diagnostics have reserved RT-Thread-only values.  RT_EFULL
 * and RT_ENOSPC intentionally map to the same POSIX condition, while
 * RT_EEMPTY uses ENODATA.
 * @{
 */
#define RT_EOK                          0               /**< Successful completion; never negate this value. */
#define RT_ERROR                        255             /**< RT-Thread generic/otherwise unclassified failure. */
#define RT_ETIMEOUT                     ETIMEDOUT       /**< Operation exceeded its allowed waiting time. */
#define RT_EFULL                        ENOSPC          /**< Bounded RT-Thread resource has no free capacity. */
#define RT_EEMPTY                       ENODATA         /**< Bounded RT-Thread resource currently contains no data. */
#define RT_ENOMEM                       ENOMEM          /**< Memory allocation or required memory is unavailable. */
#define RT_ENOSYS                       ENOSYS          /**< Requested operation is not implemented. */
#define RT_EBUSY                        EBUSY           /**< Resource cannot proceed because it is busy. */
#define RT_EIO                          EIO             /**< Device or other input/output operation failed. */
#define RT_EINTR                        EINTR           /**< Blocking operation was interrupted before completion. */
#define RT_EINVAL                       EINVAL          /**< Argument value or combination is invalid. */
#define RT_ENOENT                       ENOENT          /**< Requested named object or entry does not exist. */
#define RT_ENOSPC                       ENOSPC          /**< Storage/device/resource has no remaining space. */
#define RT_EPERM                        EPERM           /**< Caller is not permitted to perform the operation. */
#define RT_EFAULT                       EFAULT          /**< Supplied address cannot be accessed as required. */
#define RT_ENOBUFS                      ENOBUFS         /**< Required networking/system buffer is unavailable. */
#define RT_ESCHEDISR                    253             /**< Immediate switch deferred because caller is in an ISR. */
#define RT_ESCHEDLOCKED                 252             /**< Immediate switch deferred by nested scheduler critical state. */
#define RT_ETRAP                        254             /**< RT-Thread trap/exception event. */
/** @} */
#else
/**
 * @name Compact error values for freestanding or Nano builds
 *
 * These small stable values do not alias the platform's POSIX errno numbers.
 * They describe RT-Thread conditions only and must be mapped explicitly when
 * a boundary requires a platform-specific POSIX value.  A non-Nano build may
 * still include the system errno header indirectly through rttypes.h.
 * @{
 */
#define RT_EOK                          0               /**< Successful completion; never negate this value. */
#define RT_ERROR                        1               /**< Generic/otherwise unclassified failure. */
#define RT_ETIMEOUT                     2               /**< Operation exceeded its allowed waiting time. */
#define RT_EFULL                        3               /**< Bounded resource has no free capacity. */
#define RT_EEMPTY                       4               /**< Bounded resource currently contains no data. */
#define RT_ENOMEM                       5               /**< Required memory could not be obtained. */
#define RT_ENOSYS                       6               /**< Requested operation is not implemented. */
#define RT_EBUSY                        7               /**< Resource cannot proceed because it is busy. */
#define RT_EIO                          8               /**< Device or other input/output operation failed. */
#define RT_EINTR                        9               /**< Blocking operation was interrupted. */
#define RT_EINVAL                       10              /**< Argument value or combination is invalid. */
#define RT_ENOENT                       11              /**< Requested named object or entry does not exist. */
#define RT_ENOSPC                       12              /**< Storage/device/resource has no remaining space. */
#define RT_EPERM                        13              /**< Caller is not permitted to perform the operation. */
#define RT_ETRAP                        14              /**< RT-Thread trap/exception event. */
#define RT_EFAULT                       15              /**< Supplied address cannot be accessed as required. */
#define RT_ENOBUFS                      16              /**< Required networking/system buffer is unavailable. */
#define RT_ESCHEDISR                    17              /**< Immediate switch deferred because caller is in an ISR. */
#define RT_ESCHEDLOCKED                 18              /**< Immediate switch deferred by nested scheduler critical state. */
/** @} */
#endif /* defined(RT_USING_LIBC) && !defined(RT_USING_NANO) */

/**
 * @brief Return the errno value belonging to the current execution context.
 *
 * In thread context this reads the current thread's TCB error field.  During
 * an interrupt, or before a current thread exists, it reads the shared global
 * fallback slot.  The function does not clear or normalize the value.
 *
 * @return The selected slot converted to rt_err_t.  Thread storage is rt_err_t;
 *         the shared fallback slot is int-width.
 */
rt_err_t rt_get_errno(void);

/**
 * @brief Store an errno value for the current execution context.
 *
 * In normal thread context only the calling thread's error field is changed.
 * Interrupt and early-boot calls update the shared int-width fallback slot.
 * Both positive POSIX-style errno values and negative RT-Thread-style values
 * are accepted without sign conversion.  On a target where rt_err_t is wider
 * than int, a value outside the int range is narrowed in the fallback context.
 *
 * @param no Error value to store; no sign conversion or range validation is done.
 */
void rt_set_errno(rt_err_t no);

/**
 * @brief Return an lvalue-compatible address for the current context's errno.
 *
 * This is the accessor behind the compatibility `errno` macro.  The returned
 * address selects the current thread field or the interrupt/early-boot global
 * slot using the same rules as rt_get_errno().  The function's ABI is `int *`;
 * on a 64-bit build it casts the wider rt_err_t thread field to `int *`, so an
 * errno-lvalue write updates only an int-sized portion of that field.  This is
 * an implementation width caveat; prefer rt_set_errno()/rt_get_errno() for the
 * full rt_err_t API.  Do not hand the returned address to another context or
 * retain a thread-field address beyond that thread's lifetime.
 *
 * @return Pointer to the current errno storage.  The pointer is owned by the
 *         kernel and must not be freed.
 */
int *_rt_errno(void);

/**
 * @brief Convert a known RT-Thread error value to a static diagnostic string.
 *
 * Positive and negative forms are treated alike.  Only the compact table of
 * core RT-Thread errors implemented by klibc is recognized; other POSIX or
 * component-specific values produce the implementation's unknown-error
 * string.  This is a diagnostic label, not a localized `strerror()` message.
 *
 * @param error Positive or negative RT-Thread error code.
 * @return Pointer to immutable static storage.  The caller must not modify or
 *         free it, and no caller-provided buffer is required.
 */
const char *rt_strerror(rt_err_t error);

/*
 * Newlib and Windows builds suppress this compatibility macro.  RT-Thread's
 * Newlib glue can still implement Newlib's `__errno()` by forwarding to
 * _rt_errno(); suppression here merely avoids replacing Newlib's public macro.
 * For other configurations, define errno only when a previous header has not
 * already defined it.  The expansion is an lvalue:
 * both `errno = value` and reads dispatch through _rt_errno() each time, which
 * is what preserves per-thread selection across scheduling.
 */
#if !defined(RT_USING_NEWLIBC) && !defined(_WIN32)
#ifndef errno
#define errno    *_rt_errno()
#endif
#endif /* !defined(RT_USING_NEWLIBC) && !defined(_WIN32) */

#ifdef __cplusplus
}
#endif

#endif
