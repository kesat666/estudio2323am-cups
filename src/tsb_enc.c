/*
 * tsb_enc.c — e-STUDIO 2323AM 作业流编码器
 *
 * 报文结构已由 Windows 官方驱动的实际输出确认（tools/pjl_probe.py 可逐字节复现）：
 *
 *   \x1b%-12345X@PJL\r\n                     UEL + PJL 起始
 *   @PJL SET TIMESTAMP=YYYY/MM/DD HH:MM:SS\r\n
 *   @PJL SET COMPRESS=JBIG\r\n
 *   @PJL SET USERNAME=<用户>\r\n
 *   @PJL SET COVER=OFF\r\n
 *   @PJL SET HOLD=OFF\r\n
 *   @PJL SET ROTATESORT=OFF\r\n
 *   @PJL SET SMOOTHING=ON|OFF\r\n
 *   @PJL SET TONERSAVE=ON|OFF\r\n
 *   @PJL SET DEPTCODENUM=""\r\n
 *   @PJL SET MEDIASOURCE=TRAY1\r\n
 *   @PJL SET MEDIATYPE=PAPER\r\n
 *   @PJL SET PAGESTATUS=START\r\n
 *   @PJL SET COPIES=<n>\r\n
 *   @PJL SET DUPLEX=ON|OFF\r\n             （双面时另加 BINDING=LONGEDGE|SHORTEDGE）
 *   @PJL SET PAPER=A4\r\n
 *   @PJL SET PAPERWIDTH=<px>\r\n
 *   @PJL SET PAPERLENGTH=<px>\r\n
 *   @PJL SET RESOLUTION=600\r\n
 *   @PJL SET ORIENTATION=PORTRAIT\r\n
 *   @PJL SET IMAGELEN=<n>\r\n
 *   <JBIG BIE，n 字节>
 *   @PJL SET DOTCOUNT=<黑点数>\r\n
 *   @PJL SET PAGESTATUS=END\r\n
 *   @PJL EOJ\r\n
 *   \x1b%-12345X\r\n
 *
 * 因为 IMAGELEN 在数据前面，必须先编码完整页才能写出表头，
 * 所以整页缓存在内存（A4/600dpi 1bpp = 4.35 MB，可接受）。
 */

#include "tsb_enc.h"
#include "tsb_jbig.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

struct tsb_ctx {
    FILE       *out;
    tsb_jobopts opt;
    tsb_stats   st;

    int         page_open;
    int         cur_w;
    int         cur_h;
    int         rows_done;

    /* 页面尺寸（点）。0 表示上游没给，退回用像素尺寸反推。 */
    double      page_w_pt;
    double      page_h_pt;

    unsigned char *scratch;      /* 旧模式（RAW/CAPTURE）用 */
    size_t         scratch_cap;

    unsigned char *page;         /* 整页 1bpp：PJL_JBIG 模式用 */
    size_t         page_cap;
    int            page_bpl;
    int            copy_index;

    unsigned char *rot;          /* 背面 180° 旋转的临时缓冲 */
    size_t         rot_cap;
    int            page_seq;     /* 已产出页数：偶数=正面，奇数=背面 */
};

/* ------------------------------------------------------------------ 基础写出 */

static int wr(tsb_ctx *c, const void *p, size_t n)
{
    if (n == 0) return TSB_OK;
    if (fwrite(p, 1, n, c->out) != n) return TSB_ERR;
    c->st.bytes_out += n;
    return TSB_OK;
}

/* "@PJL SET KEY=VALUE\r\n" */
static int wr_set(tsb_ctx *c, const char *k, const char *v)
{
    char buf[192];
    int n = snprintf(buf, sizeof buf, "@PJL SET %s=%s\r\n", k, v);

    if (n <= 0 || (size_t)n >= sizeof buf) return TSB_ERR;
    return wr(c, buf, (size_t)n);
}

static int wr_set_long(tsb_ctx *c, const char *k, long v)
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%ld", v);

    if (n <= 0 || (size_t)n >= sizeof buf) return TSB_ERR;
    buf[n] = '\0';
    return wr_set(c, k, buf);
}

/* --------------------------------------------------------------- PackBits */

/*
 * 标准 PackBits（保留给单测；真实报文用 JBIG）：
 *   n >= 0  : 后面跟 n+1 个字面字节
 *   n <  0  : 下一个字节重复 (1-n) 次
 * 返回写入字节数；容量不足返回 0。
 */
size_t tsb_packbits_encode(const unsigned char *in, size_t n,
                           unsigned char *out, size_t cap)
{
    size_t i = 0, o = 0;

    if (n == 0) return 0;

    while (i < n) {
        size_t run = 1;
        while (i + run < n && in[i + run] == in[i] && run < 128) run++;

        if (run >= 2) {
            if (o + 2 > cap) return 0;
            out[o++] = (unsigned char)(257 - run);
            out[o++] = in[i];
            i += run;
        } else {
            size_t start = i, lit = 0;
            while (i < n && lit < 128) {
                if (i + 1 < n && in[i] == in[i + 1]) break;
                i++;
                lit++;
            }
            if (lit == 0) { i = start + 1; lit = 1; }
            if (o + 1 + lit > cap) return 0;
            out[o++] = (unsigned char)(lit - 1);
            memcpy(out + o, in + start, lit);
            o += lit;
        }
    }
    return o;
}

/* ------------------------------------------------------------- 生命周期 */

tsb_ctx *tsb_open(FILE *out, const tsb_jobopts *opts)
{
    tsb_ctx *c;

    if (!out || !opts) return NULL;

    c = (tsb_ctx *)calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->out = out;
    c->opt = *opts;
    if (c->opt.band_height <= 0) c->opt.band_height = 128;

    if (c->opt.mode == TSB_PAYLOAD_RAW || c->opt.mode == TSB_PAYLOAD_CAPTURE) {
        c->scratch_cap = 1u << 20;
        c->scratch = (unsigned char *)malloc(c->scratch_cap);
        if (!c->scratch) { free(c); return NULL; }
    }
    return c;
}

void tsb_close(tsb_ctx *c)
{
    if (!c) return;
    free(c->scratch);
    free(c->page);
    free(c->rot);
    free(c);
}

const tsb_stats *tsb_get_stats(const tsb_ctx *c)
{
    return c ? &c->st : NULL;
}

/* ------------------------------------------------------------ 纸张与统计 */

/* 由 CUPS media 关键字反推 PAPER 取值。关键字本身就编码了尺寸，比量像素可靠。 */
static const char *paper_from_media(const char *media)
{
    static const struct { const char *pfx; const char *tok; } tab[] = {
        { "iso_a4",       "A4"        },
        { "iso_a3",       "A3"        },
        { "iso_a5",       "A5"        },
        { "iso_a6",       "A6"        },
        { "jis_b5",       "B5"        },
        { "iso_b5",       "B5"        },
        { "na_letter",    "LETTER"    },
        { "na_legal",     "LEGAL"     },
        { "na_executive", "EXECUTIVE" },
        { "na_statement", "STATEMENT" }
    };
    size_t i, n;

    if (!media || !*media) return NULL;
    for (i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        n = strlen(tab[i].pfx);
        if (strncmp(media, tab[i].pfx, n) == 0) return tab[i].tok;
    }
    return NULL;
}

/* 按页面尺寸（点）匹配；容差 3pt，够吸收 595.28 与 595 这种四舍五入。 */
static const char *paper_from_pt(double w, double h)
{
    static const struct { double w, h; const char *n; } tab[] = {
        { 595.28,  841.89, "A4"        },
        { 841.89, 1190.55, "A3"        },
        { 419.53,  595.28, "A5"        },
        { 297.64,  419.53, "A6"        },
        { 612.0,   792.0,  "LETTER"    },
        { 612.0,  1008.0,  "LEGAL"     },
        { 521.86,  756.0,  "EXECUTIVE" },
        { 515.91,  728.5,  "B5"        },   /* JIS B5 182x257mm */
        { 498.90,  708.66, "B5"        },   /* ISO B5 176x250mm */
        { 396.0,   612.0,  "STATEMENT" }
    };
    size_t i;

    for (i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        double dw = w - tab[i].w, dh = h - tab[i].h;
        if (dw < 0) dw = -dw;
        if (dh < 0) dh = -dh;
        if (dw <= 3.0 && dh <= 3.0) return tab[i].n;
    }
    return NULL;
}

/* 兜底：按像素尺寸反推。仅在既没有 media 关键字、也没有页面尺寸提示时使用。 */
static const char *paper_from_px(int w, int h)
{
    static const struct { int w, h; const char *n; } tab[] = {
        { 4961,  7016, "A4"        },
        { 7016,  9921, "A3"        },
        { 3508,  4961, "A5"        },
        { 5100,  6600, "LETTER"    },
        { 5100,  8400, "LEGAL"     },
        { 3300,  5100, "STATEMENT" }
    };
    size_t i;

    for (i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        int dw = w - tab[i].w, dh = h - tab[i].h;
        if (dw < 0) dw = -dw;
        if (dh < 0) dh = -dh;
        if (dw <= 12 && dh <= 12) return tab[i].n;
    }
    return "CUSTOM";
}

void tsb_hint_page_size(tsb_ctx *c, double width_pt, double height_pt)
{
    if (!c) return;
    if (width_pt > 1.0 && height_pt > 1.0) {
        c->page_w_pt = width_pt;
        c->page_h_pt = height_pt;
    }
}

/* PAPER 取值：media 关键字 > 页面尺寸（点）> 像素尺寸 > CUSTOM */
static const char *paper_token(const tsb_ctx *c)
{
    const char *t = paper_from_media(c->opt.media);

    if (t) return t;
    if (c->page_w_pt > 1.0 && c->page_h_pt > 1.0) {
        t = paper_from_pt(c->page_w_pt, c->page_h_pt);
        if (t) return t;
    }
    return paper_from_px(c->cur_w, c->cur_h);
}

static unsigned long count_dots(const unsigned char *p, size_t n)
{
    static const unsigned char pc[16] = {
        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4
    };
    unsigned long bits = 0;
    size_t i;

    for (i = 0; i < n; i++)
        bits += (unsigned long)(pc[p[i] & 0x0f] + pc[(p[i] >> 4) & 0x0f]);
    return bits;
}

/* ------------------------------------------------------- 背面 180° 旋转 */

/* 1bpp 位图原地转 180°（行序倒置 + 每行位序倒置），宽度不必是 8 的倍数。 */
static void rot180_1bpp(const unsigned char *src, unsigned char *dst,
                        int w, int h, int bpl)
{
    int x, y;

    memset(dst, 0, (size_t)bpl * (size_t)h);
    for (y = 0; y < h; y++) {
        const unsigned char *srow = src + (size_t)(h - 1 - y) * (size_t)bpl;
        unsigned char       *drow = dst + (size_t)y * (size_t)bpl;

        for (x = 0; x < w; x++) {
            int sx = w - 1 - x;
            if ((srow[sx >> 3] >> (7 - (sx & 7))) & 1)
                drow[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
        }
    }
}

/* 当前页是否需要旋转背面 */
static int need_back_rotate(const tsb_ctx *c)
{
    if (!c->opt.duplex) return 0;
    if (c->opt.back_rotate == 1) return 0;                      /* Off  */
    if (c->opt.back_rotate == 2) return (c->page_seq & 1);      /* On   */
    return (c->opt.duplex == 1) && (c->page_seq & 1);           /* Auto */
}

/* ---------------------------------------------------------------- 作业头 */

int tsb_job_begin(tsb_ctx *c)
{
    if (!c) return TSB_ERR;

    switch (c->opt.mode) {
    case TSB_PAYLOAD_RAW:
    case TSB_PAYLOAD_CAPTURE:
        return TSB_OK;          /* 比对模式不加任何封装 */

    case TSB_PAYLOAD_PJL_JBIG:
    default:
        /* UEL 紧跟 "@PJL" + CRLF，与驱动一致（UEL 与 @PJL 之间没有换行） */
        if (wr(c, "\x1b%-12345X", 9) != TSB_OK) return TSB_ERR;
        if (wr(c, "@PJL\r\n", 6) != TSB_OK) return TSB_ERR;
        c->st.pages = 0;
        return TSB_OK;
    }
}

int tsb_job_end(tsb_ctx *c)
{
    if (!c) return TSB_ERR;

    switch (c->opt.mode) {
    case TSB_PAYLOAD_RAW:
    case TSB_PAYLOAD_CAPTURE:
        return TSB_OK;
    default:
        if (wr(c, "@PJL EOJ\r\n", 10) != TSB_OK) return TSB_ERR;
        if (wr(c, "\x1b%-12345X", 9) != TSB_OK) return TSB_ERR;
        if (wr(c, "\r\n", 2) != TSB_OK) return TSB_ERR;
        return TSB_OK;
    }
}

/* ------------------------------------------------------------------ 页 */

int tsb_page_begin(tsb_ctx *c, int width_px, int height_px, int copy_index)
{
    int bpl;

    if (!c || width_px <= 0 || height_px <= 0) return TSB_ERR;

    /* 换了一份（copy）就把页序从头数：背面旋转靠奇偶判断，
     * 页数为奇数时若不重置，第二份的正面会被当成背面转掉。 */
    if (c->page_seq > 0 && copy_index != c->copy_index) c->page_seq = 0;

    c->cur_w      = width_px;
    c->cur_h      = height_px;
    c->rows_done  = 0;
    c->copy_index = copy_index;
    c->page_open  = 1;

    bpl = (width_px + 7) / 8;
    c->page_bpl = bpl;

    if (c->opt.mode == TSB_PAYLOAD_PJL_JBIG) {
        size_t need = (size_t)bpl * (size_t)height_px;

        if (need > c->page_cap) {
            unsigned char *np = (unsigned char *)realloc(c->page, need);
            if (!np) { c->page_open = 0; return TSB_ERR; }
            c->page = np;
            c->page_cap = need;
        }
        /* 未写到的行保持白色（0 = 白，与 PBM/驱动一致） */
        memset(c->page, 0, need);
    }
    return TSB_OK;
}

int tsb_page_band(tsb_ctx *c, const unsigned char *band, int y0, int nlines,
                  int bytes_per_line)
{
    if (!c || !band || !c->page_open || nlines <= 0) return TSB_ERR;
    if (y0 < 0 || y0 + nlines > c->cur_h) return TSB_ERR;

    if (c->opt.mode == TSB_PAYLOAD_PJL_JBIG) {
        int copy = bytes_per_line;

        if (copy > c->page_bpl) copy = c->page_bpl;
        memcpy(c->page + (size_t)y0 * c->page_bpl, band,
               (size_t)copy * (size_t)nlines);
        if (y0 + nlines > c->rows_done) c->rows_done = y0 + nlines;
        c->st.bands++;
        return TSB_OK;
    }

    /* 旧模式：逐带直接吐原始位图 / 转储 */
    {
        size_t need = (size_t)bytes_per_line * (size_t)nlines;

        if (need > c->scratch_cap) {
            unsigned char *np = (unsigned char *)realloc(c->scratch, need);
            if (!np) return TSB_ERR;
            c->scratch = np;
            c->scratch_cap = need;
        }
        if (c->opt.mode == TSB_PAYLOAD_CAPTURE) {
            char hdr[64];
            int n = snprintf(hdr, sizeof hdr, "@@ y=%d lines=%d bpl=%d\n",
                             y0, nlines, bytes_per_line);
            if (n > 0 && wr(c, hdr, (size_t)n) != TSB_OK) return TSB_ERR;
        }
        if (wr(c, band, need) != TSB_OK) return TSB_ERR;
        c->rows_done += nlines;
        c->st.bands++;
        return TSB_OK;
    }
}

int tsb_page_end(tsb_ctx *c)
{
    if (!c || !c->page_open) return TSB_ERR;

    c->page_open = 0;

    if (c->opt.mode != TSB_PAYLOAD_PJL_JBIG) {
        c->st.pages++;
        return TSB_OK;
    }

    {
        unsigned char *bie = NULL;
        size_t bie_len = 0;
        char ts[32];
        const char *paper;
        time_t now;
        struct tm tmv;
        unsigned char *img = c->page;   /* 实际送进 JBIG 的位图（可能是旋转后的） */

        /* 背面旋转：本机机械翻转轴固定为短边，长边装订必须由主机把背面转 180°。
         * 必须在编码与 DOTCOUNT 之前完成，两者都要用旋转后的像素。 */
        if (need_back_rotate(c)) {
            size_t need = (size_t)c->page_bpl * (size_t)c->cur_h;

            if (need > c->rot_cap) {
                unsigned char *np = (unsigned char *)realloc(c->rot, need);
                if (!np) {
                    fprintf(stderr, "ERROR: out of memory (rot %lu)\n",
                            (unsigned long)need);
                    return TSB_ERR;
                }
                c->rot = np;
                c->rot_cap = need;
            }
            rot180_1bpp(c->page, c->rot, c->cur_w, c->cur_h, c->page_bpl);
            img = c->rot;
            fprintf(stderr, "DEBUG: back side rotated 180 (page_seq=%d)\n",
                    c->page_seq);
        }
        c->page_seq++;

        if (tsb_jbig_encode(img, c->cur_w, c->cur_h, c->page_bpl,
                            &bie, &bie_len) != 0) {
            fprintf(stderr,
                    "ERROR: JBIG encode failed (w=%d h=%d) — "
                    "此构建没有链接 libjbig，无法生成可打印报文\n",
                    c->cur_w, c->cur_h);
            return TSB_ERR;
        }

        /* ---- 页头：字段顺序必须与驱动一致，工具 pjl_probe 可直接比对 ---- */
        now = time(NULL);
#if defined(_WIN32)
        {
            struct tm *lt = localtime(&now);
            if (lt) tmv = *lt; else memset(&tmv, 0, sizeof tmv);
        }
#else
        if (!localtime_r(&now, &tmv)) memset(&tmv, 0, sizeof tmv);
#endif
        strftime(ts, sizeof ts, "%Y/%m/%d %H:%M:%S", &tmv);

        paper = paper_token(c);

        if (wr_set(c, "TIMESTAMP",     ts)                                  != TSB_OK) goto fail;
        if (wr_set(c, "COMPRESS",      "JBIG")                              != TSB_OK) goto fail;
        if (wr_set(c, "USERNAME",      c->opt.username[0] ? c->opt.username : "cups") != TSB_OK) goto fail;
        if (wr_set(c, "COVER",         "OFF")                               != TSB_OK) goto fail;
        if (wr_set(c, "HOLD",          "OFF")                               != TSB_OK) goto fail;
        if (wr_set(c, "ROTATESORT",    "OFF")                               != TSB_OK) goto fail;
        if (wr_set(c, "SMOOTHING",     "OFF")                               != TSB_OK) goto fail;
        if (wr_set(c, "TONERSAVE",     c->opt.toner_save ? "ON" : "OFF")    != TSB_OK) goto fail;
        if (wr_set(c, "DEPTCODENUM",   "\"\"")                              != TSB_OK) goto fail;
        {
            const char *ms = c->opt.media_source[0] ? c->opt.media_source : "TRAY1";
            const char *mt = c->opt.media_type_str[0] ? c->opt.media_type_str : "PAPER";
            if (wr_set(c, "MEDIASOURCE", ms) != TSB_OK) goto fail;
            if (wr_set(c, "MEDIATYPE",   mt) != TSB_OK) goto fail;
        }
        if (wr_set(c, "PAGESTATUS",    "START")                             != TSB_OK) goto fail;
        if (wr_set_long(c, "COPIES",   (long)(c->opt.copies > 0 ? c->opt.copies : 1)) != TSB_OK) goto fail;

        /* 双面：DUPLEX 只有 ON/OFF，装订方向是独立字段 BINDING。
         * BINDING 的取值（长短边 token）当初只能从驱动 DLL 字符串猜，未抓过真实
         * 双面样张，故做成可被 TSBBinding 选项覆盖，便于真机试不同写法。 */
        if (wr_set(c, "DUPLEX", c->opt.duplex ? "ON" : "OFF") != TSB_OK) goto fail;
        if (c->opt.duplex) {
            const char *bv;
            if (c->opt.binding_override[0] == '\0') {
                bv = (c->opt.duplex == 2) ? "SHORTEDGE" : "LONGEDGE";
            } else if (strcasecmp(c->opt.binding_override, "NONE") == 0) {
                bv = NULL;          /* 明确不发射 BINDING 字段 */
            } else {
                bv = c->opt.binding_override;
            }
            if (bv && wr_set(c, "BINDING", bv) != TSB_OK) goto fail;
        }

        if (wr_set(c, "PAPER",         paper)                               != TSB_OK) goto fail;
        if (wr_set_long(c, "PAPERWIDTH",  (long)c->cur_w)                   != TSB_OK) goto fail;
        if (wr_set_long(c, "PAPERLENGTH", (long)c->cur_h)                   != TSB_OK) goto fail;
        if (wr_set_long(c, "RESOLUTION",  (long)(c->opt.x_dpi > 0 ? c->opt.x_dpi : 600)) != TSB_OK) goto fail;
        if (wr_set(c, "ORIENTATION",   "PORTRAIT")                          != TSB_OK) goto fail;
        if (wr_set_long(c, "IMAGELEN", (long)bie_len)                       != TSB_OK) goto fail;

        /* ---- 数据段 ---- */
        if (wr(c, bie, bie_len) != TSB_OK) goto fail;

        /* ---- 页尾 ---- */
        if (wr_set_long(c, "DOTCOUNT",
                        (long)count_dots(img,
                                         (size_t)c->page_bpl * (size_t)c->cur_h)) != TSB_OK)
            goto fail;
        if (wr_set(c, "PAGESTATUS", "END") != TSB_OK) goto fail;

        free(bie);
        c->st.pages++;
        return TSB_OK;

fail:
        free(bie);
        return TSB_ERR;
    }
}
