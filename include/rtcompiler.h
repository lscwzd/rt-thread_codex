/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-01-18     Shell        Separate the compiler porting from rtdef.h
 */

/**
 * @file rtcompiler.h
 * @brief Uniform compiler-attribute interface for all RT-Thread toolchains.
 *
 * Kernel and component code must express several properties that ISO C does
 * not describe uniformly: placing an object in a linker section, retaining
 * an apparently unreferenced registration record, forcing alignment or
 * packed layout, declaring a weak override point, obtaining an expression's
 * type, marking a non-returning routine, and controlling inlining.  This
 * header translates the RT-Thread spelling of each property to the selected
 * compiler's extension.
 *
 * These macros are part of the compiler-porting boundary.  Their expansion
 * may intentionally be empty on a toolchain that cannot express a property;
 * callers should use the portable macro rather than copying a compiler
 * attribute into common code.  Linker scripts must also preserve or collect
 * the section names supplied through `rt_section`, especially for automatic
 * initialization and module-symbol tables.
 */
#ifndef __RT_COMPILER_H__
#define __RT_COMPILER_H__

/* Generated build configuration selects the compiler and related ABI details. */
#include <rtconfig.h>

/**
 * @name Portable compiler-property macros
 *
 * Every supported branch below provides the following logical interface:
 *
 * - `rt_section(name)`: request placement in linker section `name`.
 * - `rt_used`: request retention when normal references are absent.
 * - `rt_align(n)`: request at least `n`-byte alignment.
 * - `rt_packed(declare)`: request the branch's closest packed declaration.
 * - `rt_weak`: request a definition overridable by a strong definition.
 * - `rt_typeof`: obtain the compiler type of an expression.
 * - `rt_noreturn`: state that control never returns to the caller when this
 *   compatibility branch supplies an annotation.
 * - `rt_inline`: internal-linkage inline helper.
 * - `rt_always_inline`: strongest available request to inline such a helper.
 *
 * These are portability requests, not identical guarantees on every branch:
 * several legacy/hosted branches intentionally map unsupported properties to
 * empty or pass-through expansions.  Packed and aligned objects can have
 * stricter hardware access constraints; an effective layout annotation does
 * not make an unaligned access safe.  Where weak binding is supported, it is
 * commonly used for a BSP/application override of a default body.
 * @{
 */

#if defined(__ARMCC_VERSION)        /* ARM Compiler */
/*
 * Arm Compiler 5 uses the legacy `__packed` keyword, whereas Arm Compiler 6
 * (armclang) accepts the Clang/GNU packed attribute.  The numeric threshold
 * keeps a single public macro valid for both generations.
 */
#define rt_section(x)               __attribute__((section(x)))
#define rt_used                     __attribute__((used))
#define rt_align(n)                 __attribute__((aligned(n)))
#if __ARMCC_VERSION >= 6010050
#define rt_packed(declare)          declare __attribute__((packed))
#else
#define rt_packed(declare)          __packed declare
#endif
#define rt_weak                     __attribute__((weak))
#define rt_typeof                   __typeof
#define rt_noreturn
#define rt_inline                   static __inline
#define rt_always_inline            rt_inline
#elif defined (__IAR_SYSTEMS_ICC__) /* for IAR Compiler */
/*
 * IAR attaches a section with `@`, retains roots with `__root`, and expresses
 * alignment through `_Pragma`.  `PRAGMA` stringizes its argument so callers
 * can pass a natural token sequence such as data_alignment=8.
 *
 * This generic IAR packed macro is intentionally a pass-through and therefore
 * does not itself remove padding; an IAR-specific declaration must express any
 * layout requirement that common code cannot leave at the default ABI.
 */
#define rt_section(x)               @ x
#define rt_used                     __root
#define PRAGMA(x)                   _Pragma(#x)
#define rt_align(n)                 PRAGMA(data_alignment=n)
#define rt_packed(declare)          declare
#define rt_weak                     __weak
#define rt_typeof                   __typeof
#define rt_noreturn
#define rt_inline                   static inline
#define rt_always_inline            rt_inline
#elif defined (__GNUC__)            /* GNU GCC Compiler */
/*
 * Two-stage stringification expands macro arguments before converting them
 * to a string.  It is used where a generated token/value must become linker
 * or assembler text rather than the literal macro name supplied by a caller.
 */
#define __RT_STRINGIFY(x...)        #x
#define RT_STRINGIFY(x...)          __RT_STRINGIFY(x)
/* GCC attributes provide the complete portable property set. */
#define rt_section(x)               __attribute__((section(x)))
#define rt_used                     __attribute__((used))
#define rt_align(n)                 __attribute__((aligned(n)))
#define rt_packed(declare)          declare __attribute__((packed))
#define rt_weak                     __attribute__((weak))
#define rt_typeof                   __typeof__
#define rt_noreturn                 __attribute__ ((noreturn))
#define rt_inline                   static __inline
/* Unlike plain rt_inline, this asks GCC to inline even without optimization heuristics. */
#define rt_always_inline            static inline __attribute__((always_inline))
#elif defined (__ADSPBLACKFIN__)    /* for VisualDSP++ Compiler */
/* VisualDSP++ supports GNU-like attributes but uses native `typeof`. */
#define rt_section(x)               __attribute__((section(x)))
#define rt_used                     __attribute__((used))
#define rt_align(n)                 __attribute__((aligned(n)))
#define rt_packed(declare)          declare
#define rt_weak                     __attribute__((weak))
#define rt_typeof                   typeof
#define rt_noreturn
#define rt_inline                   static inline
#define rt_always_inline            rt_inline
#elif defined (_MSC_VER)            /* for Visual Studio Compiler */
/*
 * MSVC has no direct equivalents for all GNU declaration attributes in this
 * compatibility layer.  Section placement, retention, weak binding, and
 * noreturn therefore expand to no annotation here.  Alignment uses
 * `__declspec`, and packing brackets exactly one declaration with push/pop
 * so the caller's surrounding packing state is restored.
 */
#define rt_section(x)
#define rt_used
#define rt_align(n)                 __declspec(align(n))
#define rt_packed(declare)          __pragma(pack(push, 1)) declare __pragma(pack(pop))
#define rt_weak
#define rt_typeof                   typeof
#define rt_noreturn
#define rt_inline                   static __inline
#define rt_always_inline            rt_inline
#elif defined (__TI_COMPILER_VERSION__) /* for TI CCS Compiler */
/**
 * The way that TI compiler set section is different from other(at least
 * GCC and MDK) compilers. See ARM Optimizing C/C++ Compiler 5.9.3 for more
 * details.
 */
#define rt_section(x)               __attribute__((section(x)))
#ifdef __TI_EABI__
/* EABI's retain attribute prevents linker garbage collection as well as compiler removal. */
#define rt_used                     __attribute__((retain)) __attribute__((used))
#else
#define rt_used                     __attribute__((used))
#endif
#define PRAGMA(x)                   _Pragma(#x)
#define rt_align(n)                 __attribute__((aligned(n)))
#define rt_packed(declare)          declare __attribute__((packed))
#ifdef __TI_EABI__
/* Weak binding is available in the EABI mode used by modern TI ports. */
#define rt_weak                     __attribute__((weak))
#else
/* Legacy TI ABI has no portable weak expansion in this interface. */
#define rt_weak
#endif
#define rt_typeof                   typeof
#define rt_noreturn
#define rt_inline                   static inline
#define rt_always_inline            rt_inline
#elif defined (__TASKING__)         /* for TASKING Compiler */
/* `protect` complements `used` so linker optimization retains registry objects. */
#define rt_section(x)               __attribute__((section(x)))
#define rt_used                     __attribute__((used, protect))
#define PRAGMA(x)                   _Pragma(#x)
#define rt_align(n)                 __attribute__((__align(n)))
#define rt_packed(declare)          declare __packed__
#define rt_weak                     __attribute__((weak))
#define rt_typeof                   typeof
#define rt_noreturn
#define rt_inline                   static inline
#define rt_always_inline            rt_inline
#else                              /* Unkown Compiler */
    /* Fail at preprocessing time: silently losing ABI/linker attributes is unsafe. */
    #error not supported tool chain
#endif /* __ARMCC_VERSION */

/** @} */

#endif /* __RT_COMPILER_H__ */
