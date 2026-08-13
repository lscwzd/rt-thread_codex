/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 */

/**
 * @file rtm.h
 * @brief RT-Thread 模块的内核符号发布接口。
 *
 * 动态加载的模块不会仅因内核中存在同名 C 符号就能解析引用。内核必须在由
 * 链接器收集的表中显式发布该符号。`RTM_EXPORT(symbol)` 会生成模块加载器所需
 * 的元数据，`struct rt_module_symtab` 则定义符号查找时检查的通用地址/名称记录。
 *
 * 此导出宏经过专门设计，在任何配置下都可以安全地放在函数或对象定义旁。当
 * `RT_USING_MODULE` 未启用时，它会展开为空。启用模块后，各工具链的行为不同：
 * MinGW 同样不生成任何内容；嵌入式分支会生成地址/名称记录；MSVC 则会在 COFF
 * 子节中生成带特殊前缀的名称字符串，而不是通用的 rt_module_symtab 记录。因此，
 * 存储开销取决于所使用的分支。
 *
 * 导出符号会使其对独立构建的模块可见，因此形成 ABI 兼容承诺：其函数签名、对象
 * 布局、调用约定和生命周期必须与这些模块保持兼容。该机制只发布一个地址；它不会
 * 增加参数检查、权限隔离、引用计数或自动版本协商。
 */

#ifndef __RTM_H__
#define __RTM_H__

/* rtdef.h 提供编译器段宏；rtthread.h 提供内核 API。 */
#include <rtdef.h>
#include <rtthread.h>

#ifdef RT_USING_MODULE

/**
 * @brief 动态模块加载器可见的一个内核符号。
 *
 * 链接器生成的起始/结束标记界定这些记录组成的数组。加载器按名称查找，并返回
 * `addr`，以重定位模块中尚未解析的引用。
 *
 * 此记录特意存储未指定类型的地址，因为同一张表必须同时表示函数和数据对象。如何
 * 正确转换该地址的类型并调用它，取决于编译进模块的声明。
 */
struct rt_module_symtab
{
    void       *addr;      /**< 已导出函数或对象在链接时的地址。 */
    const char *name;      /**< 用于查找、以 NUL 结尾的源代码级符号名称。 */
};

#if defined(_MSC_VER)
/*
 * MSVC 的宿主机/模拟器构建使用 COFF 子节。`$f` 参与链接器对子节进行的词典序
 * 排序，而 `allocate` 将生成的名称对象放入该子节。`__vs_rtm_` 前缀是模拟器一侧
 * 的符号处理可识别的编码。`/merge` 将 RTMSymTab 合并到 `mytext`，使元数据位于
 * 预期的映像段中。
 *
 * 记号拼接（`##`）会为每次展开生成一个由导出符号派生的唯一 C 标识符；字符串化
 * （`#`）会记录其文本名称。
 */
#pragma section("RTMSymTab$f",read)
#define RTM_EXPORT(symbol)                                            \
__declspec(allocate("RTMSymTab$f"))const char __rtmsym_##symbol##_name[] = "__vs_rtm_"#symbol;
#pragma comment(linker, "/merge:RTMSymTab=mytext")

#elif defined(__MINGW32__)
/* MinGW 宿主机构建不会在此生成 RT-Thread 模块符号表。 */
#define RTM_EXPORT(symbol)

#else
/*
 * 嵌入式 ELF/Arm/IAR 风格的构建会为每个导出生成两个链接器对象：
 *
 * - `__rtmsym_<symbol>_name` 会在只读名称段中保存源代码中完全一致的拼写；
 * - `__rtmsym_<symbol>` 会在 `RTMSymTab` 中保存地址/名称对。
 *
 * 即使启用了段垃圾回收，工具链的链接器配置也必须保留并收集 `RTMSymTab`。加载器
 * 边界的发现方式同样因工具链而异：GNU、Arm 和 IAR 构建分别使用各自的链接器或
 * 段边界机制。将字符串放在 `.rodata.name` 中可使记录不依赖可写存储，并避免在
 * 代码中重复保存该拼写。分号是宏的一部分，因此正常调用 `RTM_EXPORT(foo);` 在
 * 语法上不会产生问题。
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
 * 当未构建模块加载器时，保持各源代码位置与配置无关。此时不会保留符号名称、表项
 * 或对链接器段的依赖。
 */
#define RTM_EXPORT(symbol)
#endif

#endif
