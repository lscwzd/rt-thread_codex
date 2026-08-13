/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2024-03-10     Meco 人第一个版本
 */

/**
 * @file rtklibc.h
 * @brief RT-Thread 内核侧 libc 兼容 API 的总入口头文件。
 *
 * 内核 libc（klibc）为内核、组件、BSP 和模块代码提供稳定的 `rt_*` 命名空间，
 * 其中包含常用的内存、字符串、格式化输入输出、扫描和错误码服务。它可用于裸机式
 * 构建，但这并不等同于启用完整的应用程序 C 库。具体实现可以是 RT-Thread 的紧凑版
 * 或标准版例程，也可以是对所选 libc 的封装。经配置后，部分内存和字符串例程允许由
 * 用户替换；这并不表示每个格式化、扫描或 errno 符号都能被统一替换。选择结果由
 * rtconfig.h 中生成的 `RT_KLIBC_*` 选项决定。
 *
 * 包含此文件会引入以下三个专用接口：
 *
 * - klibc/kstring.h：字节内存和以 NUL 结尾的字符串操作；
 * - klibc/kstdio.h：基于缓冲区的 printf/scanf 系列；
 * - klibc/kerrno.h：RT-Thread 错误常量和与执行上下文相关的 errno 访问接口。
 *
 * 这些例程不会验证任意指针，也不会推断目标缓冲区容量。缓冲区大小、内存重叠、
 * 格式、分配、执行上下文和对象生命周期等约束，均由各自的声明详细说明。
 */

#ifndef __RT_KLIBC_H__
#define __RT_KLIBC_H__

/* 构建期后端选择，以及 RT-Thread 的核心类型和 API 定义。 */
#include <rtconfig.h>
#include <rtdef.h>

/* klibc 的公开子接口；应用程序也可以按需分别包含它们。 */
#include "klibc/kstring.h"
#include "klibc/kstdio.h"
#include "klibc/kerrno.h"

#endif /* __RT_KLIBC_H__ */
