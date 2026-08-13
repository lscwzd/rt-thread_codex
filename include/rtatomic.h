/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2023-03-14     WangShun     first version
 * 2023-05-20     Bernard      add stdc atomic detection.
 * 2026-03-09     wdfk-prog    add 8/16-bit atomic operations support
 */

/**
 * @file rtatomic.h
 * @brief Backend-independent atomic operations and atomic-list helpers.
 *
 * RT-Thread exposes one set of fetch-style atomic operations while allowing
 * a BSP/architecture to select one of three implementations:
 *
 * 1. `RT_USING_STDC_ATOMIC`: C11 `<stdatomic.h>` primitives.  The unsuffixed
 *    standard functions used here have sequentially consistent ordering.
 * 2. `RT_USING_HW_ATOMIC`: CPU-port primitives, normally implemented with
 *    exclusive-load/store, compare-and-swap, or equivalent instructions.  This
 *    interface does not impose a uniform cross-port memory order: barriers and
 *    acquire/release strength are defined by each architecture implementation.
 * 3. Software fallback: a short critical section entered through
 *    rt_hw_interrupt_disable().
 *
 * Fetch-add/subtract/bitwise operations and exchange return the value observed
 * *before* the modification.  Compare-and-exchange instead reports success as
 * a boolean: on failure it writes the actual value back through `expected`.
 * Flag test-and-set reports the prior clear/set state, but callers should not
 * assume that every backend normalizes its return to exactly zero or one.
 *
 * `volatile` in the API prevents inappropriate ordinary load/store elision;
 * it is not itself a synchronization mechanism.  Ordering for the hardware
 * backend is part of the CPU port's contract.  The software fallback uses the
 * generic RT-Thread exclusion primitive: it masks local interrupts on UP, while
 * an SMP build maps that primitive to rt_cpus_lock()/rt_cpus_unlock().  It is
 * therefore serialized but is not a lock-free implementation; CPU-port and
 * early-boot constraints of that generic lock still apply.
 *
 * This interface is currently emitted only for C translation units.  The
 * atomic storage aliases remain available to C++, but this header does not
 * attempt to map C11 `<stdatomic.h>` semantics onto differing C++ toolchain
 * implementations.
 */
#ifndef __RT_ATOMIC_H__
#define __RT_ATOMIC_H__

/* Supplies atomic storage types, inline attributes, and interrupt primitives. */
#include <rthw.h>

#if !defined(__cplusplus)

/**
 * @name Architecture atomic primitive contract
 *
 * A CPU port supplies these functions when `RT_USING_HW_ATOMIC` selects the
 * hardware backend.  Native-width operations use `rt_atomic_t`; optional
 * byte and halfword operations are selected separately by
 * `ARCH_USING_HW_ATOMIC_8` and `ARCH_USING_HW_ATOMIC_16`.
 * @{
 */

/** Atomically read a native-width object and return its current value. */
rt_atomic_t rt_hw_atomic_load(volatile rt_atomic_t *ptr);

/** Atomically replace a native-width object with @p val. */
void rt_hw_atomic_store(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** Atomically read an 8-bit object and return its current value. */
rt_atomic8_t rt_hw_atomic_load8(volatile rt_atomic8_t *ptr);

/** Atomically replace an 8-bit object with @p val. */
void rt_hw_atomic_store8(volatile rt_atomic8_t *ptr, rt_atomic8_t val);

/** Atomically read a 16-bit object and return its current value. */
rt_atomic16_t rt_hw_atomic_load16(volatile rt_atomic16_t *ptr);

/** Atomically replace a 16-bit object with @p val. */
void rt_hw_atomic_store16(volatile rt_atomic16_t *ptr, rt_atomic16_t val);

/** Add @p val atomically and return the value that preceded the addition. */
rt_atomic_t rt_hw_atomic_add(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** Subtract @p val atomically and return the value that preceded the subtraction. */
rt_atomic_t rt_hw_atomic_sub(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** Apply 8-bit bitwise AND atomically and return the previous value. */
rt_atomic8_t rt_hw_atomic_and8(volatile rt_atomic8_t *ptr, rt_atomic8_t val);

/** Apply 8-bit bitwise OR atomically and return the previous value. */
rt_atomic8_t rt_hw_atomic_or8(volatile rt_atomic8_t *ptr, rt_atomic8_t val);

/** Apply 16-bit bitwise AND atomically and return the previous value. */
rt_atomic16_t rt_hw_atomic_and16(volatile rt_atomic16_t *ptr, rt_atomic16_t val);

/** Apply 16-bit bitwise OR atomically and return the previous value. */
rt_atomic16_t rt_hw_atomic_or16(volatile rt_atomic16_t *ptr, rt_atomic16_t val);

/** Apply native-width bitwise AND atomically and return the previous value. */
rt_atomic_t rt_hw_atomic_and(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** Apply native-width bitwise OR atomically and return the previous value. */
rt_atomic_t rt_hw_atomic_or(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** Apply native-width bitwise XOR atomically and return the previous value. */
rt_atomic_t rt_hw_atomic_xor(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** Replace a native-width object atomically and return its previous value. */
rt_atomic_t rt_hw_atomic_exchange(volatile rt_atomic_t *ptr, rt_atomic_t val);

/** Clear a flag object to zero atomically. */
void rt_hw_atomic_flag_clear(volatile rt_atomic_t *ptr);

/** Set a flag object to one atomically and return its previous zero/nonzero state. */
rt_atomic_t rt_hw_atomic_flag_test_and_set(volatile rt_atomic_t *ptr);

/**
 * Compare @p ptr with `*expected` and conditionally store @p desired.
 *
 * @param ptr Object to test and possibly update.
 * @param expected Expected input value; overwritten with the observed value
 *        if the comparison fails.
 * @param desired Value stored when the comparison succeeds.
 * @return Nonzero on successful replacement, zero on mismatch.
 */
rt_atomic_t rt_hw_atomic_compare_exchange_strong(volatile rt_atomic_t *ptr, rt_atomic_t *expected, rt_atomic_t desired);

/** @} */

#if defined(RT_USING_STDC_ATOMIC)

/*
 * The C standard permits an implementation to define __STDC_NO_ATOMICS__
 * even in C11 mode.  Reject that combination rather than silently degrading
 * code explicitly configured to use the standard atomic backend.
 *
 * Fetch arithmetic/bitwise operations and exchange below return the
 * pre-operation value.  The generic C11 macros infer 8-, 16-, or native-width
 * operation from the pointed-to
 * `_Atomic` type.  No explicit memory order is supplied, so the C11 default
 * is memory_order_seq_cst.
 *
 * There is a compatibility caveat in the unchanged flag macros below:
 * rt_atomic_t is an atomic integer, not C11 `atomic_flag`, yet it is passed to
 * atomic_flag_clear/test_and_set.  That type mismatch is outside the C11 API
 * contract and some implementations operate on only flag-sized storage.  Do
 * not infer native-word flag semantics from this branch; configurations using
 * these macros must validate the toolchain behavior or prefer a corrected
 * port/backend implementation.
 */
#ifndef __STDC_NO_ATOMICS__
#define rt_atomic_load(ptr) atomic_load(ptr)
#define rt_atomic_store(ptr, v) atomic_store(ptr, v)
#define rt_atomic_load8(ptr) atomic_load(ptr)
#define rt_atomic_store8(ptr, v) atomic_store(ptr, v)
#define rt_atomic_load16(ptr) atomic_load(ptr)
#define rt_atomic_store16(ptr, v) atomic_store(ptr, v)
#define rt_atomic_add(ptr, v) atomic_fetch_add(ptr, v)
#define rt_atomic_sub(ptr, v) atomic_fetch_sub(ptr, v)
#define rt_atomic_and8(ptr, v) atomic_fetch_and(ptr, v)
#define rt_atomic_or8(ptr, v) atomic_fetch_or(ptr, v)
#define rt_atomic_and16(ptr, v) atomic_fetch_and(ptr, v)
#define rt_atomic_or16(ptr, v) atomic_fetch_or(ptr, v)
#define rt_atomic_and(ptr, v) atomic_fetch_and(ptr, v)
#define rt_atomic_or(ptr, v)  atomic_fetch_or(ptr, v)
#define rt_atomic_xor(ptr, v) atomic_fetch_xor(ptr, v)
#define rt_atomic_exchange(ptr, v) atomic_exchange(ptr, v)
#define rt_atomic_flag_clear(ptr) atomic_flag_clear(ptr)
#define rt_atomic_flag_test_and_set(ptr) atomic_flag_test_and_set(ptr)
#define rt_atomic_compare_exchange_strong(ptr, v,des) atomic_compare_exchange_strong(ptr, v ,des)
#else
#error "The standard library C doesn't support the atomic operation"
#endif /* __STDC_NO_ATOMICS__ */

#elif defined(RT_USING_HW_ATOMIC)
/*
 * Native-width operations are always routed to the CPU port.  Byte and
 * halfword support is advertised independently: a CPU may have an atomic
 * word instruction but lack atomic sub-word read/modify/write instructions.
 */
#define rt_atomic_load(ptr) rt_hw_atomic_load(ptr)
#define rt_atomic_store(ptr, v) rt_hw_atomic_store(ptr, v)
#if defined(ARCH_USING_HW_ATOMIC_8)
/* The architecture guarantees hardware-safe 8-bit atomic primitives. */
#define rt_atomic_load8(ptr) rt_hw_atomic_load8(ptr)
#define rt_atomic_store8(ptr, v) rt_hw_atomic_store8(ptr, v)
#define rt_atomic_and8(ptr, v) rt_hw_atomic_and8(ptr, v)
#define rt_atomic_or8(ptr, v)  rt_hw_atomic_or8(ptr, v)
#else
/*
 * Route to the software-helper names when 8-bit hardware support is absent.
 * In this header's present conditional layout those inline helpers are emitted
 * only by the full software-backend branch below, so a HW-atomic configuration
 * lacking ARCH_USING_HW_ATOMIC_8 has an unresolved interface unless its port
 * supplies compatible helpers.  This is a configuration gap, not a guaranteed
 * generic fallback.
 */
#define rt_atomic_load8(ptr) rt_soft_atomic_load8(ptr)
#define rt_atomic_store8(ptr, v) rt_soft_atomic_store8(ptr, v)
#define rt_atomic_and8(ptr, v) rt_soft_atomic_and8(ptr, v)
#define rt_atomic_or8(ptr, v)  rt_soft_atomic_or8(ptr, v)
#endif
#if defined(ARCH_USING_HW_ATOMIC_16)
/* The architecture guarantees hardware-safe 16-bit atomic primitives. */
#define rt_atomic_load16(ptr) rt_hw_atomic_load16(ptr)
#define rt_atomic_store16(ptr, v) rt_hw_atomic_store16(ptr, v)
#define rt_atomic_and16(ptr, v) rt_hw_atomic_and16(ptr, v)
#define rt_atomic_or16(ptr, v)  rt_hw_atomic_or16(ptr, v)
#else
/*
 * Route to the software-helper names when 16-bit hardware support is absent.
 * As with the 8-bit case, the inline definitions below are excluded in this
 * HW branch; a port/configuration must supply them or enable 16-bit hardware
 * support.  The macro mapping alone does not make the API linkable.
 */
#define rt_atomic_load16(ptr) rt_soft_atomic_load16(ptr)
#define rt_atomic_store16(ptr, v) rt_soft_atomic_store16(ptr, v)
#define rt_atomic_and16(ptr, v) rt_soft_atomic_and16(ptr, v)
#define rt_atomic_or16(ptr, v)  rt_soft_atomic_or16(ptr, v)
#endif
#define rt_atomic_add(ptr, v) rt_hw_atomic_add(ptr, v)
#define rt_atomic_sub(ptr, v) rt_hw_atomic_sub(ptr, v)
#define rt_atomic_and(ptr, v) rt_hw_atomic_and(ptr, v)
#define rt_atomic_or(ptr, v)  rt_hw_atomic_or(ptr, v)
#define rt_atomic_xor(ptr, v) rt_hw_atomic_xor(ptr, v)
#define rt_atomic_exchange(ptr, v) rt_hw_atomic_exchange(ptr, v)
#define rt_atomic_flag_clear(ptr) rt_hw_atomic_flag_clear(ptr)
#define rt_atomic_flag_test_and_set(ptr) rt_hw_atomic_flag_test_and_set(ptr)
#define rt_atomic_compare_exchange_strong(ptr, v,des) rt_hw_atomic_compare_exchange_strong(ptr, v ,des)

#else
/*
 * No standard or CPU atomic backend was selected.  Map every public operation
 * to an inline critical section below.  rt_hw_interrupt_disable() returns the
 * prior interrupt state, which is restored verbatim by
 * rt_hw_interrupt_enable(level); this makes the helpers safe to call from an
 * already-excluded/nested context according to the UP or SMP CPU-port contract.
 */
#include <rthw.h>
#define rt_atomic_load(ptr) rt_soft_atomic_load(ptr)
#define rt_atomic_store(ptr, v) rt_soft_atomic_store(ptr, v)
#define rt_atomic_load8(ptr) rt_soft_atomic_load8(ptr)
#define rt_atomic_store8(ptr, v) rt_soft_atomic_store8(ptr, v)
#define rt_atomic_load16(ptr) rt_soft_atomic_load16(ptr)
#define rt_atomic_store16(ptr, v) rt_soft_atomic_store16(ptr, v)
#define rt_atomic_add(ptr, v) rt_soft_atomic_add(ptr, v)
#define rt_atomic_sub(ptr, v) rt_soft_atomic_sub(ptr, v)
#define rt_atomic_and8(ptr, v) rt_soft_atomic_and8(ptr, v)
#define rt_atomic_or8(ptr, v)  rt_soft_atomic_or8(ptr, v)
#define rt_atomic_and16(ptr, v) rt_soft_atomic_and16(ptr, v)
#define rt_atomic_or16(ptr, v)  rt_soft_atomic_or16(ptr, v)
#define rt_atomic_and(ptr, v) rt_soft_atomic_and(ptr, v)
#define rt_atomic_or(ptr, v)  rt_soft_atomic_or(ptr, v)
#define rt_atomic_xor(ptr, v) rt_soft_atomic_xor(ptr, v)
#define rt_atomic_exchange(ptr, v) rt_soft_atomic_exchange(ptr, v)
#define rt_atomic_flag_clear(ptr) rt_soft_atomic_flag_clear(ptr)
#define rt_atomic_flag_test_and_set(ptr) rt_soft_atomic_flag_test_and_set(ptr)
#define rt_atomic_compare_exchange_strong(ptr, v,des) rt_soft_atomic_compare_exchange_strong(ptr, v ,des)

/**
 * @brief Atomically load an 8-bit value using generic kernel exclusion.
 *
 * @param ptr Address of the value to read.  It must remain valid throughout
 *        the operation.
 * @return The value observed while the generic exclusion token was held.
 */
rt_inline rt_atomic8_t rt_soft_atomic_load8(volatile rt_atomic8_t *ptr)
{
    rt_base_t level;
    rt_atomic8_t temp;

    level = rt_hw_interrupt_disable();
    temp = *ptr;
    rt_hw_interrupt_enable(level);

    return temp;
}

/**
 * @brief Atomically store an 8-bit value using generic kernel exclusion.
 *
 * @param ptr Address of the destination object.
 * @param val New value to publish before the previous interrupt state is restored.
 */
rt_inline void rt_soft_atomic_store8(volatile rt_atomic8_t *ptr, rt_atomic8_t val)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    *ptr = val;
    rt_hw_interrupt_enable(level);
}

/**
 * @brief Atomically load a 16-bit value using generic kernel exclusion.
 *
 * This protects targets on which an aligned halfword access might otherwise
 * be interrupted by code that modifies the same logical object.
 *
 * @param ptr Address of the value to read.
 * @return The protected value observed by this operation.
 */
rt_inline rt_atomic16_t rt_soft_atomic_load16(volatile rt_atomic16_t *ptr)
{
    rt_base_t level;
    rt_atomic16_t temp;

    level = rt_hw_interrupt_disable();
    temp = *ptr;
    rt_hw_interrupt_enable(level);

    return temp;
}

/**
 * @brief Atomically store a 16-bit value using generic kernel exclusion.
 *
 * @param ptr Address of the destination object.
 * @param val New value to store.
 */
rt_inline void rt_soft_atomic_store16(volatile rt_atomic16_t *ptr, rt_atomic16_t val)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    *ptr = val;
    rt_hw_interrupt_enable(level);
}

/**
 * @brief Atomically apply an 8-bit AND and return the previous value.
 *
 * The new value is `old & val`.  Returning `old` gives fetch-and semantics,
 * which lets callers determine exactly which bits they cleared.
 *
 * @param ptr Object to update.
 * @param val Bit mask ANDed with the object.
 * @return Value of the object before the AND.
 */
rt_inline rt_atomic8_t rt_soft_atomic_and8(volatile rt_atomic8_t *ptr, rt_atomic8_t val)
{
    rt_base_t level;
    rt_atomic8_t temp;

    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = temp & val;
    rt_hw_interrupt_enable(level);

    return temp;
}

/**
 * @brief Atomically apply an 8-bit OR and return the previous value.
 *
 * @param ptr Object to update.
 * @param val Bit mask to set in the object.
 * @return Value of the object before the OR.
 */
rt_inline rt_atomic8_t rt_soft_atomic_or8(volatile rt_atomic8_t *ptr, rt_atomic8_t val)
{
    rt_base_t level;
    rt_atomic8_t temp;

    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = temp | val;
    rt_hw_interrupt_enable(level);

    return temp;
}

/**
 * @brief Atomically apply a 16-bit AND and return the previous value.
 *
 * @param ptr Object to update.
 * @param val Bit mask ANDed with the object.
 * @return Value of the object before the AND.
 */
rt_inline rt_atomic16_t rt_soft_atomic_and16(volatile rt_atomic16_t *ptr, rt_atomic16_t val)
{
    rt_base_t level;
    rt_atomic16_t temp;

    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = temp & val;
    rt_hw_interrupt_enable(level);

    return temp;
}

/**
 * @brief Atomically apply a 16-bit OR and return the previous value.
 *
 * @param ptr Object to update.
 * @param val Bit mask to set in the object.
 * @return Value of the object before the OR.
 */
rt_inline rt_atomic16_t rt_soft_atomic_or16(volatile rt_atomic16_t *ptr, rt_atomic16_t val)
{
    rt_base_t level;
    rt_atomic16_t temp;

    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = temp | val;
    rt_hw_interrupt_enable(level);

    return temp;
}

/**
 * @brief Atomically replace a native-width object and return its old value.
 *
 * @param ptr Object to replace.
 * @param val New value.
 * @return Value held by @p ptr before the replacement.
 */
rt_inline rt_atomic_t rt_soft_atomic_exchange(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = val;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief Atomically add to a native-width object and return its old value.
 *
 * The routine performs the selected backend's ordinary atomic addition; it
 * does not saturate or check overflow.  Because rt_atomic_t is signed, portable
 * callers must avoid values whose mathematical sum is outside its range; do
 * not rely on wraparound being identical across the C11, hardware, and
 * software backends.
 *
 * @param ptr Counter to update.
 * @param val Increment, which may be negative for the signed base type.
 * @return Counter value before the addition.
 */
rt_inline rt_atomic_t rt_soft_atomic_add(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr += val;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief Atomically subtract from a native-width object and return its old value.
 *
 * As with rt_soft_atomic_add(), callers must keep the mathematical result in
 * the signed rt_atomic_t range rather than relying on overflow behavior.
 *
 * @param ptr Counter to update.
 * @param val Amount to subtract.
 * @return Counter value before the subtraction.
 */
rt_inline rt_atomic_t rt_soft_atomic_sub(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr -= val;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief Atomically apply native-width XOR and return the previous value.
 *
 * @param ptr Bit field to update.
 * @param val Mask of bits to toggle.
 * @return Bit field value before the XOR.
 */
rt_inline rt_atomic_t rt_soft_atomic_xor(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = (*ptr) ^ val;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief Atomically apply native-width AND and return the previous value.
 *
 * @param ptr Bit field to update.
 * @param val Mask retained by the AND operation.
 * @return Bit field value before the AND.
 */
rt_inline rt_atomic_t rt_soft_atomic_and(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = (*ptr) & val;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief Atomically apply native-width OR and return the previous value.
 *
 * @param ptr Bit field to update.
 * @param val Mask of bits to set.
 * @return Bit field value before the OR.
 */
rt_inline rt_atomic_t rt_soft_atomic_or(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    *ptr = (*ptr) | val;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief Atomically load a native-width value under generic kernel exclusion.
 *
 * @param ptr Address of the object to read.
 * @return Value observed inside the critical section.
 */
rt_inline rt_atomic_t rt_soft_atomic_load(volatile rt_atomic_t *ptr)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    temp = *ptr;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief Atomically store a native-width value under generic kernel exclusion.
 *
 * @param ptr Address of the destination object.
 * @param val New value.
 */
rt_inline void rt_soft_atomic_store(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    rt_base_t level;
    level = rt_hw_interrupt_disable();
    *ptr = val;
    rt_hw_interrupt_enable(level);
}

/**
 * @brief Set an atomic flag and report whether it was already set.
 *
 * The object is changed to one.  A zero return means this call acquired a
 * previously clear flag; a one return means the observed value was nonzero.
 * The return is normalized to zero/one rather than returning an arbitrary
 * previous nonzero value.
 *
 * @param ptr Native-width flag object, conventionally initialized to zero.
 * @return Zero if clear before the call, otherwise one.
 */
rt_inline rt_atomic_t rt_soft_atomic_flag_test_and_set(volatile rt_atomic_t *ptr)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    if (*ptr == 0)
    {
        temp = 0;
        *ptr = 1;
    }
    else
        temp = 1;
    rt_hw_interrupt_enable(level);
    return temp;
}

/**
 * @brief Clear an atomic flag.
 *
 * @param ptr Native-width flag object to reset to zero.
 */
rt_inline void rt_soft_atomic_flag_clear(volatile rt_atomic_t *ptr)
{
    rt_base_t level;
    level = rt_hw_interrupt_disable();
    *ptr = 0;
    rt_hw_interrupt_enable(level);
}

/**
 * @brief Strong compare-and-exchange implemented under generic kernel exclusion.
 *
 * There is no spurious-failure path: equality always replaces `*ptr1` with
 * @p desired.  On mismatch, the observed value is copied to `*ptr2`; this
 * allows a caller's retry loop to compare against the newest observation.
 * `ptr1` and `ptr2` are expected to identify valid objects for the full
 * critical section and normally should not alias one another.
 *
 * @param ptr1 Atomic object to test and conditionally modify.
 * @param ptr2 In/out expected value.
 * @param desired Replacement used when `*ptr1 == *ptr2`.
 * @return One on replacement, zero on mismatch.
 */
rt_inline rt_atomic_t rt_soft_atomic_compare_exchange_strong(volatile rt_atomic_t *ptr1, rt_atomic_t *ptr2,
        rt_atomic_t desired)
{
    rt_base_t level;
    rt_atomic_t temp;
    level = rt_hw_interrupt_disable();
    if ((*ptr1) != (*ptr2))
    {
        *ptr2 = *ptr1;
        temp = 0;
    }
    else
    {
        *ptr1 = desired;
        temp = 1;
    }
    rt_hw_interrupt_enable(level);
    return temp;
}
#endif /* RT_USING_STDC_ATOMIC */

/**
 * @brief Decrement an atomic counter and test whether it reached zero.
 *
 * Because `rt_atomic_sub()` returns the value before subtraction, an old
 * value of one is exactly the transition to zero.
 *
 * @param ptr Counter to decrement.
 * @return RT_TRUE only for the one-to-zero transition; otherwise RT_FALSE.
 */
rt_inline rt_bool_t rt_atomic_dec_and_test(volatile rt_atomic_t *ptr)
{
    return rt_atomic_sub(ptr, 1) == 1;
}

/**
 * @brief Add @p a unless the current value equals @p u.
 *
 * The loop begins with a snapshot.  A failed compare-and-exchange refreshes
 * `c` with the value that caused the failure, so the next iteration either
 * retries using the new value or stops if that value is @p u.  The strong
 * primitive makes every loop failure correspond to real interference rather
 * than a permitted spurious failure.
 *
 * The expression `c + a` must remain representable by rt_atomic_t whenever it
 * is evaluated; signed overflow is not a portable atomic wraparound contract.
 *
 * @param ptr Counter to update.
 * @param a Amount to add.
 * @param u Sentinel value at which no addition is allowed.
 * @return Value observed immediately before a successful addition, or @p u
 *         when the addition was suppressed.
 */
rt_inline rt_atomic_t rt_atomic_fetch_add_unless(volatile rt_atomic_t *ptr, rt_atomic_t a, rt_atomic_t u)
{
    rt_atomic_t c = rt_atomic_load(ptr);

    do {
        if (c == u)
        {
            break;
        }
    } while (!rt_atomic_compare_exchange_strong(ptr, &c, c + a));

    return c;
}

/**
 * @brief Add @p a unless the counter equals @p u and return success status.
 *
 * @return RT_TRUE when the value was changed, RT_FALSE when it equaled @p u.
 */
rt_inline rt_bool_t rt_atomic_add_unless(volatile rt_atomic_t *ptr, rt_atomic_t a, rt_atomic_t u)
{
    return rt_atomic_fetch_add_unless(ptr, a, u) != u;
}

/**
 * @brief Increment a reference-style counter only when it is nonzero.
 *
 * This is useful when zero means that an object is already unavailable for
 * acquiring new references.  It does not by itself manage the object's
 * lifetime; reclamation must still be coordinated by the owner.
 *
 * @return RT_TRUE if incremented, RT_FALSE if zero was observed.
 */
rt_inline rt_bool_t rt_atomic_inc_not_zero(volatile rt_atomic_t *ptr)
{
    return rt_atomic_add_unless(ptr, 1, 0);
}

/**
 * @brief Initialize an empty atomic singly linked stack/list head.
 *
 * This direct store is intended for initialization before the head becomes
 * concurrently visible.  Once published, update it through enqueue/dequeue.
 *
 * @param l Head to initialize; its atomic next field is set to zero.
 */
rt_inline void rt_ll_slist_init(rt_ll_slist_t *l)
{
    l->next = 0;
}

/**
 * @brief Push a node onto an atomic singly linked list head.
 *
 * The function repeatedly links @p n to the observed head and replaces the
 * head with @p n using compare-and-exchange.  If another producer wins first,
 * compare-and-exchange refreshes `exp`, `n->next` is rebuilt, and the push is
 * retried.  The operation therefore has LIFO behavior despite the historical
 * `enqueue` name.
 *
 * @param l Shared list head.
 * @param n Detached node to push.  It must not already be reachable from a
 *        list and must remain alive while concurrently observable.
 */
rt_inline void rt_ll_slist_enqueue(rt_ll_slist_t *l, rt_ll_slist_t *n)
{
    rt_base_t exp;
    exp = rt_atomic_load(&l->next);
    do
    {
        n->next = exp;
    } while (!rt_atomic_compare_exchange_strong(&l->next, &exp, (rt_base_t)n));
}

/**
 * @brief Pop and return the current atomic-list head.
 *
 * A nonempty iteration reads the candidate head's successor, then attempts
 * to publish that successor as the new list head.  Interference refreshes
 * `exp` and repeats.  RT_NULL is returned for an empty list.
 *
 * This pointer-only algorithm does not tag the head with a generation count,
 * so it does not independently prevent the ABA problem.  A node must not be
 * freed and reused while another context may still hold it as an observed
 * candidate; users need an appropriate reclamation/lifetime discipline.
 *
 * @param l Shared list head.
 * @return Removed node, or RT_NULL when empty.
 */
rt_inline rt_ll_slist_t *rt_ll_slist_dequeue(rt_ll_slist_t *l)
{
    rt_base_t exp;
    rt_ll_slist_t *head;

    exp = rt_atomic_load(&l->next);
    do
    {
        head = (rt_ll_slist_t *)exp;
    } while (head && !rt_atomic_compare_exchange_strong(&l->next, &exp, rt_atomic_load(&head->next)));
    return head;
}

#endif /* __cplusplus */

#endif /* __RT_ATOMIC_H__ */
