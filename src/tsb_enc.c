/*
 * tsb_enc.c — 见 tsb_enc.h 的说明
 *
 * 注意：本文件里所有标记 [TODO-PROTO] 的地方，都是等待抓包确认的私有报文。
 * 在这些 TODO 被替换之前，产出的字节流不能直接送给打印机。
 */

#include "tsb_enc.h"

#include <stdlib.h>
#include <string.h>

#define TSB_MAX_BAND_BYTES (1024u * 1024u * 8u)   /* 单个 page_begin 的上限保护 */

struct tsb_ctx {
    FILE       *out;
    tsb_jobopts opt;
    tsb_stats   st;

    int         page_open;
    int         cur_w;
    int         cur_h;
    int         rows_done;

    unsigned char *scratch;      /* PackBits 输出暂存 */
    size_t         scratch_cap;
};

/* ------------------------------------------------------------------ 基础写出 */

static int wr(tsb_ctx *c, const void *p, size_t n)
{
    if (n == 0) return TSB_OK;
    if (fwrite(p, 1, n, c->out) != n) return TSB_ERR;
    c->st.bytes_out += n;
    return TSB_OK;
}

static int wr_u8(tsb_ctx *c, unsigned v)
{
    unsigned char b = (unsigned char)(v & 0xffu);
    return wr(c, &b, 1);
}

static int wr_u16be(tsb_ctx *c, unsigned v)
{
    unsigned char b[2];
    b[0] = (unsigned char)((v >> 8) & 0xffu);
    b[1] = (unsigned char)(v & 0xffu);
    return wr(c, b, 2);
}

/* 注：协议确认后如果页/作业头需要 32 位字段或校验和，在这里补 wr_u32be / wr_crc。 */

/* --------------------------------------------------------------- PackBits */

/*
 * 标准 PackBits：
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
        /* 找重复串 */
        size_t run = 1;
        while (i + run < n && in[i + run] == in[i] && run < 128) run++;

        if (run >= 2) {
            if (o + 2 > cap) return 0;
            out[o++] = (unsigned char)(257 - run);
            out[o++] = in[i];
            i += run;
        } else {
            /* 字面串：直到遇到 2 个以上重复 */
            size_t start = i, lit = 0;
            while (i < n && lit < 128) {
                if (i + 1 < n && in[i] == in[i + 1]) break;
                i++;
                lit++;
            }
            if (lit == 0) {              /* 不可能，防御 */
                i = start + 1;
                lit = 1;
            }
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

    c->scratch_cap = 1u << 20;
    c->scratch = (unsigned char *)malloc(c->scratch_cap);
    if (!c->scratch) {
        free(c);
        return NULL;
    }
    return c;
}

void tsb_close(tsb_ctx *c)
{
    if (!c) return;
    free(c->scratch);
    free(c);
}

const tsb_stats *tsb_get_stats(const tsb_ctx *c)
{
    return c ? &c->st : NULL;
}

/* ---------------------------------------------------------------- 作业头 */

static const char *slot_name(int slot)
{
    switch (slot) {
    case 1:  return "TRAY1";
    case 2:  return "TRAY2";
    case 3:  return "BYPASS";
    default: return "AUTO";
    }
}

static const char *media_type_name(int t)
{
    switch (t) {
    case 1:  return "THICK";
    case 2:  return "THIN";
    case 3:  return "RECYCLED";
    default: return "PLAIN";
    }
}

static int wr_line(tsb_ctx *c, const char *fmt, const char *s)
{
    char buf[128];
    int n;

    n = snprintf(buf, sizeof buf, fmt, s);
    if (n <= 0 || (size_t)n >= sizeof buf) return TSB_ERR;
    return wr(c, buf, (size_t)n);
}

int tsb_job_begin(tsb_ctx *c)
{
    if (!c) return TSB_ERR;

    switch (c->opt.mode) {
    case TSB_PAYLOAD_RAW:
    case TSB_PAYLOAD_CAPTURE:
        /* 比对模式下不写任何伪报文，保证输出可与抓包数据段直接对比 */
        return TSB_OK;

    case TSB_PAYLOAD_PACKBITS:
    default:
        /* [TODO-PROTO] 作业头：UEL / PJL / 私有魔数 / 用户名 / 作业名
         * 抓包确认后按实际字节序列填写。下面这段是占位，不是真实协议。 */
        if (wr(c, "\x1b%-12345X", 9) != TSB_OK) return TSB_ERR;
        if (wr(c, "@PJL JOB NAME=\"cups\"\r\n", 24) != TSB_OK) return TSB_ERR;
        /* [TODO-PROTO] 双面 / 纸盒 / 介质
         * 这三行是占位，真实关键字必须由 docs/protocol-re.md 的用例 8–14
         * 抓包确认。写在这里的作用只有一个：抓包时能看出"选了哪个选项，
         * 报文里多了哪几个字节"，从而反推真实字段位置。 */
        if (wr_line(c, "@PJL SET DUPLEX=%s\r\n",
                    c->opt.duplex == 0 ? "OFF" :
                    (c->opt.duplex == 2 ? "ON/TUMBLE" : "ON")) != TSB_OK)
            return TSB_ERR;
        if (wr_line(c, "@PJL SET MEDIASOURCE=%s\r\n",
                    slot_name(c->opt.input_slot)) != TSB_OK)
            return TSB_ERR;
        if (wr_line(c, "@PJL SET MEDIATYPE=%s\r\n",
                    media_type_name(c->opt.media_type)) != TSB_OK)
            return TSB_ERR;
        break;
    }
    return TSB_OK;
}

int tsb_job_end(tsb_ctx *c)
{
    if (!c) return TSB_ERR;

    switch (c->opt.mode) {
    case TSB_PAYLOAD_RAW:
    case TSB_PAYLOAD_CAPTURE:
        return TSB_OK;
    default:
        /* [TODO-PROTO] 作业尾：EOJ、计数、校验和 */
        if (wr(c, "@PJL EOJ\r\n", 10) != TSB_OK) return TSB_ERR;
        if (wr(c, "\x1b%-12345X", 9) != TSB_OK) return TSB_ERR;
        break;
    }
    return TSB_OK;
}

/* ------------------------------------------------------------------ 页 */

int tsb_page_begin(tsb_ctx *c, int width_px, int height_px, int copy_index)
{
    if (!c || width_px <= 0 || height_px <= 0) return TSB_ERR;

    c->cur_w     = width_px;
    c->cur_h     = height_px;
    c->rows_done = 0;
    c->page_open = 1;
    c->st.pages++;

    switch (c->opt.mode) {
    case TSB_PAYLOAD_RAW:
        /* P4 PBM：1bpp，MSB 在左，和打印机光栅位序一致。
         * 用途：dump 出原始位图，与抓包逐字节比对。 */
        if (fprintf(c->out, "P4\n%d %d\n", width_px, height_px) < 0) return TSB_ERR;
        break;

    case TSB_PAYLOAD_CAPTURE:
        if (fprintf(c->out, "# PAGE w=%d h=%d copy=%d\n",
                    width_px, height_px, copy_index) < 0) return TSB_ERR;
        break;

    default:
        /* [TODO-PROTO] 页头：纸张尺寸、分辨率、方向、双面、省墨、副本号
         * 差分打印矩阵（README 4.2）就是用来定位这几个字段的。 */
        if (wr_u8(c, 0x1b) != TSB_OK) return TSB_ERR;
        if (wr_u8(c, 0x01) != TSB_OK) return TSB_ERR;      /* 占位 */
        if (wr_u16be(c, (unsigned)width_px) != TSB_OK) return TSB_ERR;
        if (wr_u16be(c, (unsigned)height_px) != TSB_OK) return TSB_ERR;
        if (wr_u16be(c, (unsigned)c->opt.x_dpi) != TSB_OK) return TSB_ERR;
        if (wr_u16be(c, (unsigned)c->opt.y_dpi) != TSB_OK) return TSB_ERR;
        if (wr_u8(c, (unsigned)c->opt.duplex) != TSB_OK) return TSB_ERR;
        if (wr_u8(c, (unsigned)c->opt.toner_save) != TSB_OK) return TSB_ERR;
        break;
    }
    return TSB_OK;
}

int tsb_page_band(tsb_ctx *c, const unsigned char *band, int y0, int nlines,
                  int bytes_per_line)
{
    size_t raw;

    (void)y0;
    if (!c || !c->page_open || !band) return TSB_ERR;
    if (nlines <= 0 || bytes_per_line <= 0) return TSB_OK;

    raw = (size_t)nlines * (size_t)bytes_per_line;
    if (raw > TSB_MAX_BAND_BYTES) return TSB_ERR;

    c->st.bands++;

    switch (c->opt.mode) {
    case TSB_PAYLOAD_RAW:
        /* 直接吐原始位图，不加任何封装 */
        return wr(c, band, raw);

    case TSB_PAYLOAD_CAPTURE: {
        /* 十六进制转储，带绝对偏移，便于和抓包对齐 */
        int y;
        for (y = 0; y < nlines; y++) {
            int x;
            const unsigned char *row = band + (size_t)y * (size_t)bytes_per_line;
            if (fprintf(c->out, "%06ld ",
                        (long)((c->rows_done + y) * bytes_per_line)) < 0)
                return TSB_ERR;
            for (x = 0; x < bytes_per_line; x++)
                fprintf(c->out, "%02x", row[x]);
            fputc('\n', c->out);
        }
        c->rows_done += nlines;
        return TSB_OK;
    }

    default: {
        size_t out_n;
        int y;

        /* PackBits 按行压，行间不跨行（多数 GDI 机型是行独立压缩） */
        if ((size_t)bytes_per_line * 2u + 16u > c->scratch_cap) {
            size_t need = (size_t)bytes_per_line * 2u + 16u;
            unsigned char *p = (unsigned char *)realloc(c->scratch, need);
            if (!p) return TSB_ERR;
            c->scratch = p;
            c->scratch_cap = need;
        }

        for (y = 0; y < nlines; y++) {
            const unsigned char *row = band + (size_t)y * (size_t)bytes_per_line;
            out_n = tsb_packbits_encode(row, (size_t)bytes_per_line,
                                        c->scratch, c->scratch_cap);
            if (out_n == 0) return TSB_ERR;
            /* [TODO-PROTO] 行头：行号 / 压缩后长度 / 行标志。若机器按行接受，
             * 这里必须写；若按整带接受，则应改成整带压缩 + 带长度前置。 */
            if (wr_u16be(c, (unsigned)out_n) != TSB_OK) return TSB_ERR;
            if (wr(c, c->scratch, out_n) != TSB_OK) return TSB_ERR;
        }
        break;
    }
    }

    c->rows_done += nlines;
    return TSB_OK;
}

int tsb_page_end(tsb_ctx *c)
{
    if (!c || !c->page_open) return TSB_ERR;

    if (c->rows_done != c->cur_h) {
        /* 行数不足：CUPS 侧异常，但不阻断出纸，只提示 */
        fprintf(stderr,
                "DEBUG: page incomplete: got %d rows, expected %d\n",
                c->rows_done, c->cur_h);
    }

    switch (c->opt.mode) {
    case TSB_PAYLOAD_RAW:
    case TSB_PAYLOAD_CAPTURE:
        break;
    default:
        /* [TODO-PROTO] 页尾：走纸 / 换页 / 页计数 */
        if (wr_u8(c, 0x0c) != TSB_OK) return TSB_ERR;
        break;
    }

    c->page_open = 0;
    return TSB_OK;
}
