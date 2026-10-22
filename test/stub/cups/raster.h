/*
 * 编译期语法检查用的 CUPS raster 存根，字段与 CUPS 2.4 的
 * cups_page_header2_t 同名，但只保留本项目用到的部分。
 */
#ifndef CUPS_STUB_RASTER_H
#define CUPS_STUB_RASTER_H

#include <cups/cups.h>

typedef struct {
    unsigned cupsWidth;
    unsigned cupsHeight;
    unsigned cupsBitsPerPixel;
    unsigned cupsBytesPerLine;
    unsigned HWResolution[2];
    unsigned NumCopies;
} cups_page_header2_t;

cups_raster_t *cupsRasterOpen(int fd, int mode);
void           cupsRasterClose(cups_raster_t *r);
unsigned       cupsRasterReadHeader2(cups_raster_t *r, cups_page_header2_t *h);
unsigned       cupsRasterReadPixels(cups_raster_t *r, unsigned char *p, unsigned len);

#endif /* CUPS_STUB_RASTER_H */
