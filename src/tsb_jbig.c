/*
 * tsb_jbig.c — libjbig 封装
 *
 * 编码参数取自驱动实测的 BIE 头：
 *   DL=256  D=width  RP=height  L0=128  MX=840  MY=255
 * 其中 L0/MX/MY 通过 jbg_enc_options 指定，尽量与厂商一致。
 *
 * 已知差异（真机验证时要盯住）：
 *   jbigkit 会把 order/options 编码进 DL 与 MX 的高位，写出来是
 *   DL=0x40100、MX=0x0800031c 这种形态；厂商写的是 DL=0x100、MX=0x348。
 *   两个方向里 jbigkit -> 厂商流已经验证通过（jbgtopbm 解出正确图像），
 *   反方向（Toshiba 解 jbigkit 的流）只能上机验证。
 *   若上机出花屏，把 order 换成 JBG_SEQUENTIAL 或关掉 TPDON 再试。
 */

#include "tsb_jbig.h"

#include <stdlib.h>
#include <string.h>

#ifdef TSB_HAVE_JBIG

#include <jbig.h>

typedef struct {
    unsigned char *p;
    size_t         len;
    size_t         cap;
    int            err;
} jbuf;

/* jbigkit 的 data_out 回调：分多次吐出，这里累积到可增长的缓冲 */
static void emit(unsigned char *start, size_t len, void *file)
{
    jbuf *b = (jbuf *)file;
    size_t need;

    if (b->err) return;
    need = b->len + len;
    if (need > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        unsigned char *np;

        while (nc < need) nc *= 2;
        np = (unsigned char *)realloc(b->p, nc);
        if (!np) { b->err = 1; return; }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, start, len);
    b->len += len;
}

int tsb_jbig_encode(const unsigned char *bits, int width, int height, int bpl,
                    unsigned char **out, size_t *out_len)
{
    struct jbg_enc_state s;
    unsigned char *planes[1];
    jbuf buf;

    if (!bits || !out || !out_len || width <= 0 || height <= 0) return -1;
    if (bpl * 8 < width) return -1;

    *out = NULL;
    *out_len = 0;
    memset(&buf, 0, sizeof buf);

    planes[0] = (unsigned char *)bits;   /* 连续整幅位图，不是行指针数组 */

    jbg_enc_init(&s, (unsigned long)width, (unsigned long)height, 1,
                 planes, emit, &buf);
    /*
     * order=0 是关键：jbigkit 默认是 JBG_ILEAVE|JBG_SMID(0x3)，写出来的 BIE 头
     * 是 DL=0x40100 / MX=0x0800031c；而厂商驱动用的是 order=0，头里是
     * DL=0x100 / MX=0x348。对齐到 order=0 才能让表头形态与驱动一致。
     * L0=128 / MX=840 / MY=255 三项取自实测头。
     */
    jbg_enc_options(&s, 0, 0, 128, 840, 255);
    jbg_enc_out(&s);
    jbg_enc_free(&s);

    if (buf.err || !buf.p || buf.len == 0) {
        free(buf.p);
        return -1;
    }
    *out = buf.p;
    *out_len = buf.len;
    return 0;
}

int tsb_jbig_available(void) { return 1; }

#else  /* !TSB_HAVE_JBIG */

int tsb_jbig_encode(const unsigned char *bits, int width, int height, int bpl,
                    unsigned char **out, size_t *out_len)
{
    (void)bits; (void)width; (void)height; (void)bpl;
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    return -1;
}

int tsb_jbig_available(void) { return 0; }

#endif /* TSB_HAVE_JBIG */
