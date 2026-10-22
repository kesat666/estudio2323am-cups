/*
 * 编译期语法检查用的 CUPS 存根，不是真 CUPS。
 * 目的：在没有 libcups2-dev 的机器（如 Windows/MinGW）上仍能做 -fsyntax-only 校验。
 * 用法： gcc -fsyntax-only -I test/stub src/cups_filter.c
 * 不要在正式构建里包含本目录。
 */
#ifndef CUPS_STUB_CUPS_H
#define CUPS_STUB_CUPS_H

#define CUPS_VERSION_MAJOR 2
#define CUPS_VERSION_MINOR 4

typedef struct _cups_raster_s cups_raster_t;

#define CUPS_RASTER_READ 0

#endif /* CUPS_STUB_CUPS_H */
