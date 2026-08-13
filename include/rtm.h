/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 */

/**
 * @file rtm.h
 * @brief Kernel symbol publication interface for RT-Thread modules.
 *
 * A dynamically loaded module cannot resolve a reference merely because the
 * kernel contains a C symbol with the same name.  The kernel must explicitly
 * publish that symbol in a linker-collected table.  `RTM_EXPORT(symbol)` emits
 * the metadata needed by the module loader, and `struct rt_module_symtab`
 * defines the common address/name record inspected during symbol lookup.
 *
 * The export macro is intentionally safe to leave next to a function or object
 * definition in every configuration.  When `RT_USING_MODULE` is off it expands
 * to nothing.  Toolchain behavior differs when modules are enabled: MinGW also
 * emits nothing, the embedded branch emits address/name records, and MSVC emits
 * a specially prefixed name string in a COFF subsection rather than the common
 * rt_module_symtab record.  Storage cost therefore depends on the branch.
 *
 * Exporting a symbol exposes it to separately built modules and therefore
 * creates an ABI commitment: its signature, object layout, calling convention,
 * and lifetime must remain compatible with those modules.  This facility
 * only publishes an address; it does not add argument checking, privilege
 * isolation, reference counting, or automatic version negotiation.
 */

#ifndef __RTM_H__
#define __RTM_H__

/* rtdef.h supplies compiler section macros; rtthread.h supplies kernel APIs. */
#include <rtdef.h>
#include <rtthread.h>

#ifdef RT_USING_MODULE

/**
 * @brief One kernel symbol visible to the dynamic module loader.
 *
 * Linker-generated start/end markers delimit an array of these records.  The
 * loader performs name lookup and returns `addr` to relocate a module's
 * unresolved reference.
 *
 * The record deliberately stores an untyped address because one table must
 * represent both functions and data objects.  Correctly casting and calling
 * that address is governed by the declaration compiled into the module.
 */
struct rt_module_symtab
{
    void       *addr;      /**< Link-time address of the exported function or object. */
    const char *name;      /**< NUL-terminated source-level symbol name used for lookup. */
};

#if defined(_MSC_VER)
/*
 * MSVC's hosted/simulator build uses a COFF subsection.  `$f` participates in
 * the linker's lexical subsection ordering, while `allocate` places the
 * generated name object there.  The `__vs_rtm_` prefix is the simulator-side
 * encoding recognized by its symbol handling.  `/merge` folds RTMSymTab into
 * `mytext` so the metadata is carried in the intended image segment.
 *
 * Token pasting (`##`) gives every expansion a unique C identifier derived
 * from the exported symbol; stringification (`#`) records its textual name.
 */
#pragma section("RTMSymTab$f",read)
#define RTM_EXPORT(symbol)                                            \
__declspec(allocate("RTMSymTab$f"))const char __rtmsym_##symbol##_name[] = "__vs_rtm_"#symbol;
#pragma comment(linker, "/merge:RTMSymTab=mytext")

#elif defined(__MINGW32__)
/* MinGW hosted builds do not emit an RT-Thread module symbol table here. */
#define RTM_EXPORT(symbol)

#else
/*
 * Embedded ELF/Arm/IAR-style builds emit two linker objects per export:
 *
 * - `__rtmsym_<symbol>_name` stores the exact source spelling in a read-only
 *   name section;
 * - `__rtmsym_<symbol>` stores the address/name pair in `RTMSymTab`.
 *
 * The toolchain's linker configuration must retain and collect `RTMSymTab`
 * despite section garbage collection.  Loader boundary discovery is likewise
 * toolchain-specific: GNU, Arm, and IAR builds use their respective linker or
 * section-boundary mechanisms.  Keeping the string in `.rodata.name` makes the
 * record writable-independent and avoids duplicating the spelling in code.
 * The semicolon is part of the macro so a normal `RTM_EXPORT(foo);` invocation
 * is syntactically harmless.
 */
#define RTM_EXPORT(symbol)                                            \
const char __rtmsym_##symbol##_name[] rt_section(".rodata.name") = #symbol;     \
const struct rt_module_symtab __rtmsym_##symbol rt_section("RTMSymTab")= \
{                                                                     \
    (void *)&symbol,                                                  \
    __rtmsym_##symbol##_name                                          \
};
#endif

#else
/*
 * Keep source sites configuration-independent when the module loader is not
 * built.  No symbol name, table entry, or linker-section dependency remains.
 */
#define RTM_EXPORT(symbol)
#endif

#endif
