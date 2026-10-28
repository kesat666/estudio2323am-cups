/*
 * cups_filter.c — CUPS filter 外壳：application/vnd.cups-raster → 私有打印流
 *
 * 这一层很薄，只做三件事：
 *   1. 从 CUPS 读 1bpp（或 8bpp 灰度）raster；
 *   2. 交给 tsb_enc 编码；
 *   3. 写到 stdout，由 CUPS 的 socket:// 或 usb:// backend 送到设备。
 *
 * 目标：CUPS 2.2 – 2.4。实测目标系统：
 *   银河麒麟 V10 SP1  → glibc 2.28, CUPS 2.2.10
 *   统信 UOS 20       → glibc 2.28, CUPS 2.2.x
 *   Debian 12         → glibc 2.36, CUPS 2.4
 * 因此二进制必须在 glibc 2.28 基线上构建（见 .github/workflows/build.yml），
 * 否则在麒麟/UOS 上会报 "version `GLIBC_2.34' not found"。
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
#include <strings.h>   /* strcasecmp：解析 TSBRotBack 用 */

#if defined(CUPS_VERSION_MAJOR) && CUPS_VERSION_MAJOR >= 3
#error "本项目面向 CUPS 2.x；CUPS 3 需改用 PAPPL，见 README 第六节"
#endif

#define BAND_LINES 128

/* ------------------------------------------------------------ 选项解析 */

/*
 * CUPS 会把 PPD 里选中的选项合并进 argv[5]，形如
 *   "media=iso_a4_210x297mm Resolution=600dpi Duplex=DuplexNoTumble job-uuid=..."
 * （PPD 未暴露 InputSlot/MediaType，那两项改由环境变量注入，见下方说明）
 *
 * 这里用"按关键字精确取值"而不是 strstr 子串匹配：
 * strstr("media=") 会误命中 "media-type="，导致取错字段。
 */
static int opt_get(const char *opts, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = opts;

    while ((p = strstr(p, key)) != NULL) {
        int at_start = (p == opts) || (p[-1] == ' ');
        if (at_start && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t n = strcspn(v, " \t\r\n");
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return 1;
        }
        p += klen;
    }
    out[0] = '\0';
    return 0;
}

static int opt_is(const char *opts, const char *key, const char *value)
{
    char buf[40];
    return opt_get(opts, key, buf, sizeof buf) && strcmp(buf, value) == 0;
}

/* 返回 0 单面 / 1 长边 / 2 短边；需要选配双面单元 MD-0107 */
static int parse_duplex(const char *opts)
{
    if (opt_is(opts, "Duplex", "DuplexNoTumble")) return 1;
    if (opt_is(opts, "Duplex", "DuplexTumble"))   return 2;
    return 0;
}

/*
 * 纸盒与介质类型不走 PPD（本机无第二纸盒，PPD 也不暴露这两个选项）。
 * 它们只作为逆向差分实验的强制开关存在：TSB_SLOT=tray1|bypass|auto，
 * TSB_MEDIATYPE=plain|thick|thin|recycled。
 * 这样抓包时可以在同一份文档上单独改一个变量，不必反复改代码重编。
 */
static int parse_input_slot(const char *opts)
{
    const char *e = getenv("TSB_SLOT");

    (void)opts;                 /* 保留签名，协议确认后可改回读 PPD 选项 */
    if (!e || !*e) return 0;
    if (strcmp(e, "tray1") == 0)  return 1;
    if (strcmp(e, "tray2") == 0)  return 2;
    if (strcmp(e, "bypass") == 0) return 3;
    return 0;
}

static int parse_media_type(const char *opts)
{
    const char *e = getenv("TSB_MEDIATYPE");

    (void)opts;
    if (!e || !*e) return 0;
    if (strcmp(e, "thick") == 0)    return 1;
    if (strcmp(e, "thin") == 0)     return 2;
    if (strcmp(e, "recycled") == 0) return 3;
    return 0;
}

/* 把 argv[5] 里某个选项的"原值"拷出来（用于直接透传 PJL token，不翻译） */
static void opt_copy(const char *opts, const char *key, char *out, size_t cap)
{
    if (opt_get(opts, key, out, cap)) return;
    out[0] = '\0';
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
    o.input_slot  = parse_input_slot(opts);
    o.media_type  = parse_media_type(opts);
    o.toner_save  = opt_is(opts, "TonerSave", "True") ? 1 : 0;
    o.band_height = BAND_LINES;

    /* 把可选覆盖字段从 CUPS 选项里原样取出（没有就留空，编码器走默认）。
     * 这三个字段的真实取值当初缺样张，做成可选项便于真机用 lp -o 试不同写法，
     * 试出正确 token 后再写死成默认值。 */
    opt_copy(opts, "TSBMediaSource", o.media_source,   sizeof o.media_source);
    opt_copy(opts, "TSBMT",          o.media_type_str, sizeof o.media_type_str);
    opt_copy(opts, "TSBBinding",     o.binding_override, sizeof o.binding_override);

    /* 背面旋转：PJL 的 BINDING 本机不认（长短边都出短边），长边要靠主机把
     * 背面图像转 180°。Auto=长边时旋转（默认），Off=不转，On=双面总转。 */
    {
        char v[24] = "";
        opt_copy(opts, "TSBRotBack", v, sizeof v);
        if (strcasecmp(v, "Off") == 0)       o.back_rotate = 1;
        else if (strcasecmp(v, "On") == 0)   o.back_rotate = 2;
        else                                 o.back_rotate = 0;
        if (getenv("TSB_ROTBACK")) {
            const char *e = getenv("TSB_ROTBACK");
            if (strcasecmp(e, "off") == 0)      o.back_rotate = 1;
            else if (strcasecmp(e, "on") == 0)  o.back_rotate = 2;
            else                                o.back_rotate = 0;
        }
    }
    /* 兼容旧的环境变量写法：设了就覆盖选项 */
    {
        const char *e;
        if ((e = getenv("TSB_SLOT"))    && *e) snprintf(o.media_source,    sizeof o.media_source,    "%s", e);
        if ((e = getenv("TSB_MEDIATYPE")) && *e) {
            /* 旧版映射厚纸等；新写法直接透传 token，这里仅当值是英文 token 时透传 */
            if (strcmp(e, "thick") == 0)    snprintf(o.media_type_str, sizeof o.media_type_str, "THICK");
            else if (strcmp(e, "thin") == 0) snprintf(o.media_type_str, sizeof o.media_type_str, "THIN");
            else if (strcmp(e, "recycled") == 0) snprintf(o.media_type_str, sizeof o.media_type_str, "RECYCLED");
            else snprintf(o.media_type_str, sizeof o.media_type_str, "%s", e);
        }
    }
    /* 报文结构已确认（PJL + JBIG），默认走真实输出。
     * 调试时用 TSB_MODE=raw|capture 切回比对模式。 */
    o.mode        = TSB_PAYLOAD_PJL_JBIG;
    if (user) {
        size_t ul = strlen(user);
        if (ul >= sizeof o.username) ul = sizeof o.username - 1;
        memcpy(o.username, user, ul);
        o.username[ul] = '\0';
    }

    (void)opt_get(opts, "media", o.media, sizeof o.media);

    /* 这几个值直接决定私有报文里的字段，抓包时必须能对上，所以打到 stderr */
    fprintf(stderr,
            "DEBUG: opts duplex=%d slot=%d media_type=%d toner_save=%d rotback=%d "
            "media=\"%s\" media_source=\"%s\" binding=\"%s\"\n",
            o.duplex, o.input_slot, o.media_type, o.toner_save, o.back_rotate,
            o.media, o.media_source, o.binding_override);

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

        /* 把 raster 头里的真实页面尺寸（点）交给编码器判定 @PJL SET PAPER。
         * 不能只靠像素：PPD 的 ImageableArea 一旦小于 PageSize，像素尺寸就
         * 对不上任何标准纸，PAPER 会退化成 CUSTOM，真机上表现为走旁路 + 双面失效。 */
        tsb_hint_page_size(enc, hdr.cupsPageSize[0], hdr.cupsPageSize[1]);
        fprintf(stderr, "DEBUG: page-size %.2fx%.2f pt\n",
                hdr.cupsPageSize[0], hdr.cupsPageSize[1]);

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
