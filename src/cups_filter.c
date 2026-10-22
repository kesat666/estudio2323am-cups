/*
 * cups_filter.c — CUPS filter 外壳：application/vnd.cups-raster → 私有打印流
 *
 * 这一层很薄，只做三件事：
 *   1. 从 CUPS 读 1bpp（或 8bpp 灰度）raster；
 *   2. 交给 tsb_enc 编码；
 *   3. 写到 stdout，由 CUPS 的 socket:// 或 usb:// backend 送到设备。
 *
 * 目标：CUPS 2.4（Debian 12 / Ubuntu 22.04–24.04 / 麒麟 V10 / 统信 UOS）。
 * CUPS 3.x 已移除 PPD + filter 模型，需改造成 PAPPL 应用（见 README 第六节）。
 *
 * 编译：  cc -O2 -std=c99 -Wall -Wextra -o rastertotoshiba cups_filter.c tsb_enc.c \
 *            $(pkg-config --cflags --libs cups)
 * 安装：  sudo install -m 755 rastertotoshiba /usr/lib/cups/filter/
 *
 * 为什么先整页缓存：CUPS raster 是不可回退的流，而多份打印需要重复发同一页。
 * A4@600dpi 单页 1bpp ≈ 4.3MB，完全可接受；换来的是 filter 不依赖可 seek 的临时文件。
 */

#include "tsb_enc.h"

#include <cups/cups.h>
#include <cups/raster.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CUPS_VERSION_MAJOR) && CUPS_VERSION_MAJOR >= 3
#error "本项目面向 CUPS 2.x；CUPS 3 需改用 PAPPL，见 README 第六节"
#endif

#define BAND_LINES 128

/* ------------------------------------------------------------ 选项解析 */

/*
 * CUPS 会把 PPD 里选中的选项合并进 argv[5]，形如
 *   "media=A4 Resolution=600dpi Duplex=DuplexNoTumble TonerSave=True job-uuid=..."
 * 这里只做最小解析，够用即可；要更严格可换 cupsParseOptions()。
 */
static int opt_has(const char *opts, const char *key, const char *value)
{
    char needle[128];
    snprintf(needle, sizeof needle, "%s=%s", key, value);
    return strstr(opts, needle) != NULL;
}

static int parse_duplex(const char *opts)
{
    if (opt_has(opts, "Duplex", "DuplexNoTumble")) return 1;  /* 长边 */
    if (opt_has(opts, "Duplex", "DuplexTumble"))   return 2;  /* 短边 */
    return 0;
}

/* --------------------------------------------------- 8bpp 灰度 → 1bpp */

static void gray8_to_bw(const unsigned char *src, unsigned char *dst,
                        int width_px)
{
    int i;
    int out_bytes = (width_px + 7) / 8;

    memset(dst, 0, (size_t)out_bytes);
    for (i = 0; i < width_px; i++) {
        if (src[i] < 128)   /* 阈值；要更细腻可换成有序抖动/误差扩散 */
            dst[i >> 3] |= (unsigned char)(0x80u >> (i & 7));
    }
}

/* ------------------------------------------------------------------ main */

int main(int argc, char *argv[])
{
    const char *job_id = "-", *user = "-", *title = "-", *opts = "";
    const char *filename = NULL;
    int copies = 1;

    FILE *fp = NULL;
    cups_raster_t *ras = NULL;
    cups_page_header2_t hdr;
    tsb_jobopts o;
    tsb_ctx *enc = NULL;
    const tsb_stats *st;

    unsigned char *page_buf = NULL;      /* 整页 1bpp */
    size_t page_cap = 0;
    unsigned char *band_raw = NULL;      /* 读入缓冲（可能 8bpp） */
    size_t band_cap = 0;

    int rc = 1;
    int have_page;

    if (argc < 6) {
        fprintf(stderr,
                "ERROR: usage: %s job user title copies options [file]\n", argv[0]);
        return 1;
    }

    job_id = argv[1];
    user   = argv[2];
    title  = argv[3];
    copies = atoi(argv[4]);
    if (copies < 1) copies = 1;
    if (argc >= 6) opts = argv[5];
    if (argc >= 7) filename = argv[6];

    if (filename && strcmp(filename, "-") != 0) {
        fp = fopen(filename, "rb");
        if (!fp) {
            fprintf(stderr, "ERROR: cannot open %s: %s\n", filename, strerror(errno));
            return 1;
        }
    } else {
        fp = stdin;
    }

    ras = cupsRasterOpen(fileno(fp), CUPS_RASTER_READ);
    if (!ras) {
        fprintf(stderr, "ERROR: not a CUPS raster stream (job %s)\n", job_id);
        if (fp != stdin) fclose(fp);
        return 1;
    }

    have_page = cupsRasterReadHeader2(ras, &hdr) ? 1 : 0;
    if (!have_page) {
        fprintf(stderr, "ERROR: empty raster stream (job %s)\n", job_id);
        cupsRasterClose(ras);
        if (fp != stdin) fclose(fp);
        return 1;
    }

    fprintf(stderr,
            "DEBUG: job %s user=%s title=\"%s\" copies=%d\n",
            job_id, user, title, copies);

    /* 编码器只创建一次，作业头/尾包住全部页 */
    memset(&o, 0, sizeof o);
    o.copies      = copies;
    o.duplex      = parse_duplex(opts);
    o.toner_save  = opt_has(opts, "TonerSave", "True") ? 1 : 0;
    o.band_height = BAND_LINES;
    o.mode        = TSB_PAYLOAD_PACKBITS;   /* 逆向完成后改这里；调试可用 TSB_MODE */

    {
        const char *m = getenv("TSB_MODE");
        if (m) {
            if (strcmp(m, "raw") == 0)           o.mode = TSB_PAYLOAD_RAW;
            else if (strcmp(m, "capture") == 0)  o.mode = TSB_PAYLOAD_CAPTURE;
            else if (strcmp(m, "packbits") == 0) o.mode = TSB_PAYLOAD_PACKBITS;
        }
    }

    enc = tsb_open(stdout, &o);
    if (!enc) {
        fprintf(stderr, "ERROR: encoder init failed\n");
        goto done;
    }

    if (tsb_job_begin(enc) != TSB_OK) {
        fprintf(stderr, "ERROR: job header failed\n");
        goto done;
    }

    while (have_page) {
        int width   = (int)hdr.cupsWidth;
        int height  = (int)hdr.cupsHeight;
        int bpl_in  = (int)hdr.cupsBytesPerLine;
        int bpp     = (int)hdr.cupsBitsPerPixel;
        int bpl_out = (width + 7) / 8;
        size_t need;
        int y, copy;

        if (bpp != 1 && bpp != 8) {
            fprintf(stderr, "ERROR: unsupported bits-per-pixel %d (need 1 or 8)\n", bpp);
            goto done;
        }
        if (width <= 0 || height <= 0 || bpl_in <= 0) {
            fprintf(stderr, "ERROR: bad raster geometry %dx%d bpl=%d\n",
                    width, height, bpl_in);
            goto done;
        }

        fprintf(stderr, "DEBUG: page %dx%d %ddpi bpp=%d\n",
                width, height, (int)hdr.HWResolution[0], bpp);

        /* 整页缓冲：按最大需求一次分配，跨页复用 */
        need = (size_t)height * (size_t)bpl_out;
        if (need > page_cap) {
            unsigned char *p = (unsigned char *)realloc(page_buf, need);
            if (!p) {
                fprintf(stderr, "ERROR: out of memory (%lu bytes)\n",
                        (unsigned long)need);
                goto done;
            }
            page_buf = p;
            page_cap = need;
        }
        memset(page_buf, 0, need);

        need = (size_t)BAND_LINES * (size_t)bpl_in;
        if (need > band_cap) {
            unsigned char *p = (unsigned char *)realloc(band_raw, need);
            if (!p) {
                fprintf(stderr, "ERROR: out of memory (band)\n");
                goto done;
            }
            band_raw = p;
            band_cap = need;
        }

        /* 把整页读进来 */
        for (y = 0; y < height; ) {
            int lines = (height - y) < BAND_LINES ? (height - y) : BAND_LINES;
            unsigned nbytes = (unsigned)lines * (unsigned)bpl_in;

            if (cupsRasterReadPixels(ras, band_raw, nbytes) == 0) {
                fprintf(stderr, "ERROR: raster read failed at row %d\n", y);
                goto done;
            }

            if (bpp == 8) {
                int k;
                for (k = 0; k < lines; k++) {
                    gray8_to_bw(band_raw + (size_t)k * (size_t)bpl_in,
                                page_buf  + (size_t)(y + k) * (size_t)bpl_out,
                                width);
                }
            } else {
                memcpy(page_buf + (size_t)y * (size_t)bpl_out,
                       band_raw, (size_t)lines * (size_t)bpl_out);
            }
            y += lines;
        }

        /* 发 copies 份 */
        for (copy = 0; copy < copies; copy++) {
            if (tsb_page_begin(enc, width, height, copy) != TSB_OK) {
                fprintf(stderr, "ERROR: page begin failed\n");
                goto done;
            }
            for (y = 0; y < height; y += BAND_LINES) {
                int lines = (height - y) < BAND_LINES ? (height - y) : BAND_LINES;
                if (tsb_page_band(enc, page_buf + (size_t)y * (size_t)bpl_out,
                                  y, lines, bpl_out) != TSB_OK) {
                    fprintf(stderr, "ERROR: encode band failed at row %d\n", y);
                    goto done;
                }
            }
            if (tsb_page_end(enc) != TSB_OK) {
                fprintf(stderr, "ERROR: page end failed\n");
                goto done;
            }
        }

        have_page = cupsRasterReadHeader2(ras, &hdr) ? 1 : 0;
    }

    if (tsb_job_end(enc) != TSB_OK) {
        fprintf(stderr, "ERROR: job trailer failed\n");
        goto done;
    }
    if (fflush(stdout) != 0) {
        fprintf(stderr, "ERROR: flush failed: %s\n", strerror(errno));
        goto done;
    }

    st = tsb_get_stats(enc);
    fprintf(stderr, "DEBUG: done pages=%llu bands=%llu bytes=%llu\n",
            st->pages, st->bands, st->bytes_out);
    rc = 0;

done:
    free(page_buf);
    free(band_raw);
    tsb_close(enc);
    cupsRasterClose(ras);
    if (fp && fp != stdin) fclose(fp);
    return rc;
}
