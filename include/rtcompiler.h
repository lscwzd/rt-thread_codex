/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2024-01-18     Shell        将编译器适配相关内容从 rtdef.h 中拆分出来
 */

/**
 * @file rtcompiler.h
 * @brief 为所有 RT-Thread 工具链提供统一的编译器属性接口。
 *
 * 内核和组件代码需要表达若干 ISO C 无法统一描述的属性，例如：把对象放入链接器
 * 段、保留看似没有引用的注册记录、强制对齐或紧凑布局、声明可覆盖的弱符号、取得
 * 表达式的类型、标记不会返回的函数，以及控制内联。本头文件会把 RT-Thread
 * 对这些属性的统一写法转换为当前编译器支持的扩展语法。
 *
 * 这些宏位于编译器移植层的边界。在无法表达某项属性的工具链上，它们有意展开为
 * 空内容；调用方应使用这些可移植宏，而不要把某个编译器的属性直接复制到公共代码中。
 * 链接脚本还必须保留或收集通过 `rt_section` 指定的段名，自动初始化表和模块符号表
 * 尤其依赖这一点。
 */
#ifndef __RT_COMPILER_H__
#define __RT_COMPILER_H__

/* 自动生成的构建配置会选择编译器，并提供相关的 ABI 细节。 */
#include <rtconfig.h>

/**
 * @name 可移植的编译器属性宏
 *
 * 下方每个受支持的分支都提供以下逻辑接口：
 *
 * - `rt_section(name)`：请求把对象放入链接器段 `name`。
 * - `rt_used`：请求保留没有普通引用的对象。
 * - `rt_align(n)`：请求至少按 `n` 字节对齐。
 * - `rt_packed(declare)`：请求使用当前分支最接近的紧凑布局声明。
 * - `rt_weak`：请求声明一个可被强符号定义覆盖的弱符号。
 * - `rt_typeof`：取得表达式的编译器类型。
 * - `rt_noreturn`：若该兼容性分支提供此标记，则说明控制流不会返回调用方。
 * - `rt_inline`：定义具有内部链接属性的内联辅助函数。
 * - `rt_always_inline`：尽可能强烈地请求对此类辅助函数进行内联。
 *
 * 它们是可移植性请求，并非在每个分支上提供完全相同的保证：一些旧版或宿主机分支
 * 会有意把不支持的属性映射为空展开或直接透传。紧凑或指定对齐的对象可能受到更严格的
 * 硬件访问限制；即使布局标记生效，也不代表未对齐访问一定安全。支持弱绑定时，BSP 或
 * 应用程序通常利用它覆盖默认实现。
 * @{
 */

#if defined(__ARMCC_VERSION)        /* ARM 编译器 */
/*
 * Arm Compiler 5 使用旧式 `__packed` 关键字，而 Arm Compiler 6（armclang）
 * 接受 Clang/GNU 的紧凑属性。通过版本号阈值，可让同一个公共宏适用于这两代
 * 编译器。
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
#elif defined (__IAR_SYSTEMS_ICC__) /* IAR 编译器 */
/*
 * IAR 使用 `@` 指定段，使用 `__root` 保留根对象，并通过 `_Pragma` 表达对齐。
 * `PRAGMA` 会将其参数字符串化，因此调用方可以传入类似 data_alignment=8 的
 * 自然预处理记号序列。
 *
 * 此通用 IAR 紧凑布局宏有意直接透传，所以它本身不会消除填充字节；如果公共代码
 * 无法保留默认 ABI 布局，任何布局要求都必须由 IAR 专用声明来表达。
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
#elif defined (__GNUC__)            /* GNU GCC 编译器 */
/*
 * 两阶段字符串化会先展开宏参数，再将其转换为字符串。当生成的 token 或值需要成为
 * 链接器或汇编器文本，而非调用方提供的字面宏名时，就需要这样处理。
 */
#define __RT_STRINGIFY(x...)        #x
#define RT_STRINGIFY(x...)          __RT_STRINGIFY(x)
/* GCC 属性提供了完整的可移植属性集合。 */
#define rt_section(x)               __attribute__((section(x)))
#define rt_used                     __attribute__((used))
#define rt_align(n)                 __attribute__((aligned(n)))
#define rt_packed(declare)          declare __attribute__((packed))
#define rt_weak                     __attribute__((weak))
#define rt_typeof                   __typeof__
#define rt_noreturn                 __attribute__ ((noreturn))
#define rt_inline                   static __inline
/* 与普通 rt_inline 不同，此宏要求 GCC 即使不依赖优化策略也尝试内联。 */
#define rt_always_inline            static inline __attribute__((always_inline))
#elif defined (__ADSPBLACKFIN__)    /* VisualDSP++ 编译器 */
/* VisualDSP++ 支持类似 GNU 的属性，但使用原生 `typeof`。 */
#define rt_section(x)               __attribute__((section(x)))
#define rt_used                     __attribute__((used))
#define rt_align(n)                 __attribute__((aligned(n)))
#define rt_packed(declare)          declare
#define rt_weak                     __attribute__((weak))
#define rt_typeof                   typeof
#define rt_noreturn
#define rt_inline                   static inline
#define rt_always_inline            rt_inline
#elif defined (_MSC_VER)            /* Visual Studio 编译器 */
/*
 * 在此兼容层中，MSVC 没有全部 GNU 声明属性的直接对应项。因此，段放置、对象保留、
 * 弱绑定和 noreturn 在这里都展开为无标记。对齐使用 `__declspec`；紧凑布局会用
 * push/pop 只包围一条声明，从而恢复调用方原有的紧凑布局状态。
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
#elif defined (__TI_COMPILER_VERSION__) /* TI CCS 编译器 */
/**
 * TI 编译器设置段的方式与其他编译器（至少与 GCC 和 MDK）不同。更多详情请参阅
 * ARM Optimizing C/C++ Compiler 5.9.3。
 */
#define rt_section(x)               __attribute__((section(x)))
#ifdef __TI_EABI__
/* EABI 的 retain 属性既能防止编译器移除对象，也能避免链接器垃圾回收该对象。 */
#define rt_used                     __attribute__((retain)) __attribute__((used))
#else
#define rt_used                     __attribute__((used))
#endif
#define PRAGMA(x)                   _Pragma(#x)
#define rt_align(n)                 __attribute__((aligned(n)))
#define rt_packed(declare)          declare __attribute__((packed))
#ifdef __TI_EABI__
/* 现代 TI 移植所用的 EABI 模式支持弱绑定。 */
#define rt_weak                     __attribute__((weak))
#else
/* 旧版 TI ABI 在此接口中没有可移植的弱符号展开方式。 */
#define rt_weak
#endif
#define rt_typeof                   typeof
#define rt_noreturn
#define rt_inline                   static inline
#define rt_always_inline            rt_inline
#elif defined (__TASKING__)         /* TASKING 编译器 */
/* `protect` 与 `used` 配合使用，使链接器优化时仍保留注册表对象。 */
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
#else                              /* 未知编译器 */
    /* 在预处理阶段报错：静默丢失 ABI 或链接器属性并不安全。 */
    #error not supported tool chain
#endif /* __ARMCC_VERSION */

/** @} */

#endif /* __RT_COMPILER_H__ */
