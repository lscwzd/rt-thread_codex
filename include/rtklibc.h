/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-03-10     Meco Man     the first version
 */

/**
 * @file rtklibc.h
 * @brief Umbrella header for RT-Thread's kernel-side libc compatibility API.
 *
 * The kernel libc (klibc) gives kernel, component, BSP, and module code a
 * stable `rt_*` namespace for commonly needed memory, string, formatted I/O,
 * scanning, and error-number services.  It is available in freestanding
 * builds and is not the same thing as enabling a full application C library.
 * Individual implementations can be RT-Thread's compact/standard routines or
 * wrappers around the selected libc.  A configured subset of the memory/string
 * routines also supports user-supplied replacements; this is not a blanket
 * replacement mechanism for every formatting, scanning, or errno symbol.
 * Selection follows the `RT_KLIBC_*` options generated in rtconfig.h.
 *
 * Including this file imports three focused interfaces:
 *
 * - klibc/kstring.h: byte-memory and NUL-terminated string operations;
 * - klibc/kstdio.h: buffer-based printf/scanf families;
 * - klibc/kerrno.h: RT-Thread error constants and context-local errno access.
 *
 * These routines do not validate arbitrary pointers or infer destination
 * capacity.  Their buffer, overlap, format, allocation, execution-context,
 * and lifetime contracts are documented by the individual declarations.
 */

#ifndef __RT_KLIBC_H__
#define __RT_KLIBC_H__

/* Build-time backend selection and the core RT-Thread type/API definitions. */
#include <rtconfig.h>
#include <rtdef.h>

/* Public klibc subinterfaces; applications may also include them separately. */
#include "klibc/kstring.h"
#include "klibc/kstdio.h"
#include "klibc/kerrno.h"

#endif /* __RT_KLIBC_H__ */
