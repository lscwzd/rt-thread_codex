/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2016-11-12     Bernard      首个版本
 * 2018-05-25     armink       新增简易 API，例如 LOG_D、LOG_E
 */

/*
 * 调试日志宏定义
 *
 * 这些宏以静态方式定义。若要使用调试宏，请按下面的方式配置：
 *
 * 在 C/C++ 源文件中启用或停用 DBG_ENABLE 宏，再包含本头文件。
 *
 * #define DBG_TAG           "MOD_TAG"
 * #define DBG_LVL           DBG_INFO
 * #include <rtdbg.h>          // must after of DBG_LVL, DBG_TAG or other options
 *
 * 随后即可在 C/C++ 源文件中使用 LOG_X 宏输出日志：
 * LOG_D("this is a debug log!");
 * LOG_E("this is a error log!");
 */

#ifndef RT_DBG_H__
#define RT_DBG_H__

/**
 * @file rtdbg.h
 * @brief 为每个编译单元提供带编译期级别过滤的日志接口。
 *
 * 使用轻量级后端时，必须在包含本头文件之前定义可选配置宏，例如
 * DBG_TAG、DBG_LVL、DBG_ENABLE 和 DBG_COLOR。包含保护会使这些配置只对每个
 * C/C++ 源文件中的首次包含生效。常见写法如下：
 *
 * @code
 * #define DBG_TAG "driver.uart"
 * #define DBG_LVL DBG_INFO
 * #include <rtdbg.h>
 * @endcode
 *
 * 启用 RT_USING_ULOG 后，本头文件会将 LOG_* 交给 ulog；是否启用及如何过滤由
 * ulog 自身配置决定，轻量级的 DBG_ENABLE 分支不会参与。否则，本头文件提供基于
 * rt_kprintf 的后端。被停用的级别会展开为空的可变参数宏，因此其参数不会被求值，
 * 不应依赖其中的格式字符串或副作用。
 *
 * 多个子系统都可能调用日志接口，但 rt_kprintf/ulog 在不同上下文中的安全性取决于
 * 所配置的后端。尤其在钩子或中断服务例程（ISR）中，不能假定会阻塞的控制台后端可
 * 以安全地在中断上下文运行。
 */

#include <rtconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 对未显式启用本地日志的源文件，使用全局调试配置予以启用；已经显式定义的
 * DBG_ENABLE 保持不变。
 */
#if defined(RT_USING_DEBUG) && !defined(DBG_ENABLE)
#define DBG_ENABLE
#endif

/* 若源文件尚未请求颜色输出，则根据全局配置请求 ANSI 彩色输出。 */
#if defined(RT_DEBUGING_COLOR) && !defined(DBG_COLOR)
#define DBG_COLOR
#endif

/*
 * 为可选的 dlog 软件包提供兼容性。未使用该软件包时，DLOG 及其全部参数会在
 * 预处理阶段消失。
 */
#ifdef PKG_USING_DLOG
#include <dlog.h>
#else
#define DLOG(...)
#endif

#if defined(RT_USING_ULOG)
/*
 * ulog 提供兼容的 LOG_D/LOG_I/LOG_W/LOG_E/LOG_RAW/LOG_HEX 宏，并负责格式化、
 * 过滤、异步缓冲及后端分发。
 */
#include <ulog.h>
#else

/**
 * @name 日志严重程度与详细程度取值
 *
 * DBG_LEVEL 是最高详细程度阈值。错误级别始终是详细程度最低的级别（0），而
 * DBG_LOG 会启用包括调试级别在内的全部级别（3）。
 * @{
 */
#define DBG_ERROR           0
#define DBG_WARNING         1
#define DBG_INFO            2
#define DBG_LOG             3
/** @} */

/*
 * 选择每条带前缀日志中输出的标签。DBG_TAG 是推荐在源文件中使用的名称；
 * DBG_SECTION_NAME 保留旧接口，仍可直接定义。这里应使用字符串字面量。
 */
#ifdef DBG_TAG
#ifndef DBG_SECTION_NAME
#define DBG_SECTION_NAME    DBG_TAG
#endif
#else
/* 兼容旧版本。 */
#ifndef DBG_SECTION_NAME
#define DBG_SECTION_NAME    "DBG"
#endif
#endif /* DBG_TAG */

#ifdef DBG_ENABLE

/*
 * 选择编译期阈值。DBG_LVL 是当前名称；为兼容旧源文件，仍支持 DBG_LEVEL。默认会
 * 输出警告和错误，但不输出信息及调试消息。
 */
#ifdef DBG_LVL
#ifndef DBG_LEVEL
#define DBG_LEVEL         DBG_LVL
#endif
#else
/* 兼容旧版本。 */
#ifndef DBG_LEVEL
#define DBG_LEVEL         DBG_WARNING
#endif
#endif /* DBG_LVL */

/*
 * 以下为后续使用的 ANSI Select Graphic Rendition 前景色取值：
 * BLACK    30
 * RED      31
 * GREEN    32
 * YELLOW   33
 * BLUE     34
 * PURPLE   35
 * CYAN     36
 * WHITE    37
 *
 * 彩色输出要求控制台兼容 ANSI。DBG_COLOR 只影响轻量级 rt_kprintf 后端；ulog 有
 * 自己的颜色配置。
 */
#ifdef DBG_COLOR
/* 输出任意 ANSI SGR 序列；#n 会将数值参数转换为字符串。 */
#define _DBG_COLOR(n)        rt_kprintf("\033["#n"m")
/* 为一条记录输出颜色和 `[严重程度/分区] ` 前缀。 */
#define _DBG_LOG_HDR(lvl_name, color_n)                    \
    rt_kprintf("\033["#color_n"m[" lvl_name "/" DBG_SECTION_NAME "] ")
/* 重置终端属性，并以换行结束该条记录。 */
#define _DBG_LOG_X_END                                     \
    rt_kprintf("\033[0m\n")
#else
/* 保留调用位置，同时将颜色选择编译为空代码。 */
#define _DBG_COLOR(n)
/* 无颜色前缀仍保留相同的严重程度和分区文本。 */
#define _DBG_LOG_HDR(lvl_name, color_n)                    \
    rt_kprintf("[" lvl_name "/" DBG_SECTION_NAME "] ")
/* 以换行完成无颜色记录。 */
#define _DBG_LOG_X_END                                     \
    rt_kprintf("\n")
#endif /* DBG_COLOR */

/**
 * 通过三次 rt_kprintf 调用输出一条完整的格式化记录。
 *
 * do/while 包装使这类多语句宏在条件语句中表现得像一条 C 语句。对于支持常见
 * 逗号吞并扩展的编译器，`##__VA_ARGS__` 允许格式字符串不带额外参数。若控制台后端
 * 未进行串行化，多个调用者并发输出时，前缀、正文和行尾可能相互穿插。
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
 * 原始输出不带严重程度前缀、颜色处理或自动换行。历史展开式自身含有分号；请将
 * LOG_RAW/dbg_raw 作为独立语句使用，或为 if/else 主体加花括号，以避免悬空语法。
 */
#define dbg_raw(...)         rt_kprintf(__VA_ARGS__);

#else
/*
 * 源文件级日志已停用。空的可变参数宏会有意避免求值所有参数，包括嵌在日志语句中
 * 的函数调用。
 */
#define dbg_log_line(lvl, color_n, fmt, ...)
#define dbg_raw(...)
#endif /* DBG_ENABLE */

/*
 * 编译期严重程度开关。级别数值随详细程度升高，因此某个阈值会包含所有数值更小、
 * 严重程度更高的记录。调试使用终端默认颜色，信息为绿色，警告为黄色，错误为红色。
 */
/** 详细诊断记录；仅在最高详细程度阈值下编译。 */
#if (DBG_LEVEL >= DBG_LOG)
#define LOG_D(fmt, ...)      dbg_log_line("D", 0, fmt, ##__VA_ARGS__)
#else
#define LOG_D(...)
#endif

/** 描述正常状态变化或执行进度的信息记录。 */
#if (DBG_LEVEL >= DBG_INFO)
#define LOG_I(fmt, ...)      dbg_log_line("I", 32, fmt, ##__VA_ARGS__)
#else
#define LOG_I(...)
#endif

/** 用于可恢复、可疑或性能降级情况的警告记录。 */
#if (DBG_LEVEL >= DBG_WARNING)
#define LOG_W(fmt, ...)      dbg_log_line("W", 33, fmt, ##__VA_ARGS__)
#else
#define LOG_W(...)
#endif

/** 用于操作失败或状态无效情况的错误记录。 */
#if (DBG_LEVEL >= DBG_ERROR)
#define LOG_E(fmt, ...)      dbg_log_line("E", 31, fmt, ##__VA_ARGS__)
#else
#define LOG_E(...)
#endif

/**
 * 轻量级后端启用时，输出不带框架的字节或文本。若未定义 DBG_ENABLE，dbg_raw() 会
 * 为空；尽管 LOG_RAW 宏仍然定义，其参数也不会被求值。
 */
#define LOG_RAW(...)         dbg_raw(__VA_ARGS__)

/*
 * 十六进制转储仅由 ulog 提供；轻量级 rtdbg 将其保持为空操作。在使用 ulog 的构建中，
 * @p name 用于标记对象，@p width 指定每输出行的字节数，@p buf 指向字节缓冲区，
 * @p size 给出其总长度。此回退形式不会求值这些参数。
 */
#define LOG_HEX(name, width, buf, size)

#endif /* RT_USING_ULOG */

#ifdef __cplusplus
}
#endif

#endif /* RT_DBG_H__ */
