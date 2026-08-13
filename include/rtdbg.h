/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2016-11-12     Bernard      The first version
 * 2018-05-25     armink       Add simple API, such as LOG_D, LOG_E
 */

/*
 * The macro definitions for debug
 *
 * These macros are defined in static. If you want to use debug macro, you can
 * use as following code:
 *
 * In your C/C++ file, enable/disable DBG_ENABLE macro, and then include this
 * header file.
 *
 * #define DBG_TAG           "MOD_TAG"
 * #define DBG_LVL           DBG_INFO
 * #include <rtdbg.h>          // must after of DBG_LVL, DBG_TAG or other options
 *
 * Then in your C/C++ file, you can use LOG_X macro to print out logs:
 * LOG_D("this is a debug log!");
 * LOG_E("this is a error log!");
 */

#ifndef RT_DBG_H__
#define RT_DBG_H__

/**
 * @file rtdbg.h
 * @brief Per-translation-unit logging facade with compile-time level filtering.
 *
 * Optional configuration macros such as DBG_TAG, DBG_LVL, DBG_ENABLE, and
 * DBG_COLOR must be defined before this header when the lightweight backend is
 * expected to consume them. The include guard makes that configuration local
 * to the first inclusion in each C/C++ source file. A common pattern is:
 *
 * @code
 * #define DBG_TAG "driver.uart"
 * #define DBG_LVL DBG_INFO
 * #include <rtdbg.h>
 * @endcode
 *
 * When RT_USING_ULOG is enabled this header delegates LOG_* to ulog, whose own
 * configuration determines enablement/filtering; the lightweight DBG_ENABLE
 * branch is bypassed. Otherwise this header supplies an rt_kprintf backend.
 * Disabled levels expand to empty variadic macros, so their arguments are not
 * evaluated and no format strings or side effects should be relied on.
 *
 * Logging may be invoked from many subsystems, but rt_kprintf/ulog context
 * safety depends on their configured backend. In particular, a hook or ISR
 * should not assume that a blocking console backend is interrupt-safe.
 */

#include <rtconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Global debug configuration overrides a source file that did not explicitly
 * enable its logger. An explicit DBG_ENABLE remains unchanged.
 */
#if defined(RT_USING_DEBUG) && !defined(DBG_ENABLE)
#define DBG_ENABLE
#endif

/* Globally request ANSI-colored output unless the source already requested it. */
#if defined(RT_DEBUGING_COLOR) && !defined(DBG_COLOR)
#define DBG_COLOR
#endif

/*
 * Optional dlog package compatibility. Without the package, DLOG and all of its
 * arguments disappear at preprocessing time.
 */
#ifdef PKG_USING_DLOG
#include <dlog.h>
#else
#define DLOG(...)
#endif

#if defined(RT_USING_ULOG)
/*
 * ulog provides compatible LOG_D/LOG_I/LOG_W/LOG_E/LOG_RAW/LOG_HEX macros and
 * owns formatting, filtering, asynchronous buffering, and backend dispatch.
 */
#include <ulog.h>
#else

/**
 * @name Log severity/verbosity values
 *
 * DBG_LEVEL is a maximum verbosity threshold. Error is always the least
 * verbose level (0), while DBG_LOG enables all levels through debug (3).
 * @{
 */
#define DBG_ERROR           0
#define DBG_WARNING         1
#define DBG_INFO            2
#define DBG_LOG             3
/** @} */

/*
 * Select the label printed in each prefixed log line. DBG_TAG is the preferred
 * source-level spelling; DBG_SECTION_NAME preserves the older interface and
 * can still be supplied directly. String literals are expected.
 */
#ifdef DBG_TAG
#ifndef DBG_SECTION_NAME
#define DBG_SECTION_NAME    DBG_TAG
#endif
#else
/* compatible with old version */
#ifndef DBG_SECTION_NAME
#define DBG_SECTION_NAME    "DBG"
#endif
#endif /* DBG_TAG */

#ifdef DBG_ENABLE

/*
 * Select the compile-time threshold. DBG_LVL is the current spelling;
 * DBG_LEVEL remains supported for older sources. The default prints warnings
 * and errors but omits informational and debug messages.
 */
#ifdef DBG_LVL
#ifndef DBG_LEVEL
#define DBG_LEVEL         DBG_LVL
#endif
#else
/* compatible with old version */
#ifndef DBG_LEVEL
#define DBG_LEVEL         DBG_WARNING
#endif
#endif /* DBG_LVL */

/*
 * ANSI Select Graphic Rendition foreground color values used below:
 * BLACK    30
 * RED      31
 * GREEN    32
 * YELLOW   33
 * BLUE     34
 * PURPLE   35
 * CYAN     36
 * WHITE    37
 *
 * Color output assumes an ANSI-compatible console. DBG_COLOR affects only the
 * lightweight rt_kprintf backend; ulog has its own color configuration.
 */
#ifdef DBG_COLOR
/* Emit an arbitrary ANSI SGR sequence; #n stringizes the numeric argument. */
#define _DBG_COLOR(n)        rt_kprintf("\033["#n"m")
/* Prefix one record with color plus `[severity/section] `. */
#define _DBG_LOG_HDR(lvl_name, color_n)                    \
    rt_kprintf("\033["#color_n"m[" lvl_name "/" DBG_SECTION_NAME "] ")
/* Reset terminal attributes and terminate the record with a newline. */
#define _DBG_LOG_X_END                                     \
    rt_kprintf("\033[0m\n")
#else
/* Preserve call sites while compiling color selection to no code. */
#define _DBG_COLOR(n)
/* Non-color prefix retains the same severity/section text. */
#define _DBG_LOG_HDR(lvl_name, color_n)                    \
    rt_kprintf("[" lvl_name "/" DBG_SECTION_NAME "] ")
/* Complete a non-color record with a newline. */
#define _DBG_LOG_X_END                                     \
    rt_kprintf("\n")
#endif /* DBG_COLOR */

/**
 * Emit one complete formatted record through three rt_kprintf calls.
 *
 * The do/while wrapper makes the multi-statement macro behave like one C
 * statement in conditional code. `##__VA_ARGS__` permits a format string with
 * no additional arguments on compilers supporting the common comma-swallowing
 * extension. Output from concurrent callers can interleave between the prefix,
 * payload, and line ending unless the console backend serializes it.
 */
#define dbg_log_line(lvl, color_n, fmt, ...)                \
    do                                                      \
    {                                                       \
        _DBG_LOG_HDR(lvl, color_n);                         \
        rt_kprintf(fmt, ##__VA_ARGS__);                     \
        _DBG_LOG_X_END;                                     \
    }                                                       \
    while (0)

/*
 * Raw output has no severity prefix, color handling, or automatic newline.
 * The historical expansion contains its own semicolon; use LOG_RAW/dbg_raw as
 * a standalone statement or brace an if/else body to avoid dangling syntax.
 */
#define dbg_raw(...)         rt_kprintf(__VA_ARGS__);

#else
/*
 * Source-level logging is disabled. Empty variadic macros intentionally avoid
 * evaluating every argument, including function calls embedded in a log line.
 */
#define dbg_log_line(lvl, color_n, fmt, ...)
#define dbg_raw(...)
#endif /* DBG_ENABLE */

/*
 * Compile-time severity gates. Because levels increase with verbosity, a
 * threshold includes every numerically lower (more severe) record. Debug uses
 * the terminal default color, info green, warning yellow, and error red.
 */
/** Detailed diagnostic record; compiled only at the most verbose threshold. */
#if (DBG_LEVEL >= DBG_LOG)
#define LOG_D(fmt, ...)      dbg_log_line("D", 0, fmt, ##__VA_ARGS__)
#else
#define LOG_D(...)
#endif

/** Informational record describing normal state changes or progress. */
#if (DBG_LEVEL >= DBG_INFO)
#define LOG_I(fmt, ...)      dbg_log_line("I", 32, fmt, ##__VA_ARGS__)
#else
#define LOG_I(...)
#endif

/** Warning record for recoverable, suspicious, or degraded conditions. */
#if (DBG_LEVEL >= DBG_WARNING)
#define LOG_W(fmt, ...)      dbg_log_line("W", 33, fmt, ##__VA_ARGS__)
#else
#define LOG_W(...)
#endif

/** Error record for failed operations or invalid states. */
#if (DBG_LEVEL >= DBG_ERROR)
#define LOG_E(fmt, ...)      dbg_log_line("E", 31, fmt, ##__VA_ARGS__)
#else
#define LOG_E(...)
#endif

/**
 * Emit unframed bytes/text when the lightweight backend is enabled.  If
 * DBG_ENABLE is absent, dbg_raw() is empty and LOG_RAW arguments are not
 * evaluated even though the LOG_RAW macro itself remains defined.
 */
#define LOG_RAW(...)         dbg_raw(__VA_ARGS__)

/*
 * Hex dumping is supplied by ulog only; lightweight rtdbg keeps it a no-op.
 * In a ulog build, @p name labels the object, @p width selects bytes per output
 * line, @p buf addresses the byte buffer, and @p size gives its total length.
 * In this fallback form none of those arguments is evaluated.
 */
#define LOG_HEX(name, width, buf, size)

#endif /* RT_USING_ULOG */

#ifdef __cplusplus
}
#endif

#endif /* RT_DBG_H__ */
