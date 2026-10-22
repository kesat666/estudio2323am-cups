/*
 * test_enc.c — 编码器单测 + 差分打印图案生成器（不依赖 CUPS）
 *
 * 两个用途：
 *   1) 自测：验证 tsb_packbits_encode 的实现是否正确（编解码往返）。
 *   2) 生成差分打印用的图案集（P4 PBM, 1bpp, 600dpi A4）。
 *      这是逆向流程的第一步：在 Windows 驱动里打印这些图案并抓包，
 *      再和这里生成的原始位图逐字节比对，就能定位"位图从哪个偏移开始、有没有压缩"。
 *
 * 编译：  cc -O2 -std=c99 -Wall -Wextra -o test_enc test_enc.c tsb_enc.c
 * 运行：  ./test_enc --selftest
 *         ./test_enc --patterns ./patterns
 */

#include "tsb_enc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A4_W 4960     /* 8.267in * 600 */
#define A4_H 7016     /* 11.693in * 600 */

/* ------------------------------------------------------------ PackBits 解码 */

static size_t packbits_decode(const unsigned char *in, size_t n,
                              unsigned char *out, size_t cap)
{
    size_t i = 0, o = 0;

    while (i < n) {
        signed char h = (signed char)in[i++];
        if (h >= 0) {
            size_t cnt = (size_t)h + 1;
            if (i + cnt > n || o + cnt > cap) return (size_t)-1;
            memcpy(out + o, in + i, cnt);
            i += cnt;
            o += cnt;
        } else if (h != -128) {
            size_t cnt = (size_t)(1 - h);
            if (i >= n || o + cnt > cap) return (size_t)-1;
            memset(out + o, in[i], cnt);
            i++;
            o += cnt;
        }
    }
    return o;
}

/* ---------------------------------------------------------------- 自测 */

static int fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

static int test_roundtrip(const unsigned char *src, size_t n, const char *label)
{
    unsigned char *enc, *dec;
    size_t enc_n, dec_n;
    size_t cap = n * 2 + 64;

    enc = (unsigned char *)malloc(cap);
    dec = (unsigned char *)malloc(n + 16);
    if (!enc || !dec) { free(enc); free(dec); return 1; }

    enc_n = tsb_packbits_encode(src, n, enc, cap);
    if (enc_n == 0) { free(enc); free(dec); return fail(label); }

    dec_n = packbits_decode(enc, enc_n, dec, n + 16);
    if (dec_n != n || memcmp(dec, src, n) != 0) {
        fprintf(stderr, "FAIL: %s (enc %lu -> dec %lu, want %lu)\n",
                label, (unsigned long)enc_n, (unsigned long)dec_n, (unsigned long)n);
        free(enc); free(dec);
        return 1;
    }
    printf("  ok  %-28s raw %6lu -> enc %6lu\n",
           label, (unsigned long)n, (unsigned long)enc_n);

    free(enc); free(dec);
    return 0;
}

static int selftest(void)
{
    int rc = 0;
    unsigned char buf[4096];
    size_t i;

    printf("PackBits roundtrip:\n");

    memset(buf, 0x00, sizeof buf);
    rc |= test_roundtrip(buf, sizeof buf, "all zero 4096");

    memset(buf, 0xff, sizeof buf);
    rc |= test_roundtrip(buf, sizeof buf, "all ones 4096");

    for (i = 0; i < sizeof buf; i++) buf[i] = (unsigned char)(i * 7);
    rc |= test_roundtrip(buf, sizeof buf, "ramp i*7");

    for (i = 0; i < sizeof buf; i++) buf[i] = (unsigned char)((i & 1) ? 0x00 : 0xff);
    rc |= test_roundtrip(buf, sizeof buf, "alternating");

    /* 边界：1 字节 / 128 字节重复 / 129 字节重复 */
    { unsigned char one = 0x5a; rc |= test_roundtrip(&one, 1, "single byte"); }
    { memset(buf, 0xaa, 128); rc |= test_roundtrip(buf, 128, "run 128"); }
    { memset(buf, 0xaa, 129); rc |= test_roundtrip(buf, 129, "run 129"); }

    /* 255 个交替字节（字面串上限 128 的跨界） */
    for (i = 0; i < 255; i++) buf[i] = (unsigned char)(i & 1 ? 0x11 : 0x22);
    rc |= test_roundtrip(buf, 255, "literal 255");

    /* 空输入 */
    if (tsb_packbits_encode(buf, 0, buf, sizeof buf) != 0)
        rc |= fail("empty input should produce 0 bytes");

    printf(rc ? "\nSELFTEST: FAILED\n" : "\nSELFTEST: PASSED\n");
    return rc;
}

/* ------------------------------------------------------- 图案生成 */

static unsigned char *alloc_page(void)
{
    size_t n = (size_t)A4_H * ((A4_W + 7) / 8);
    unsigned char *p = (unsigned char *)calloc(1, n);
    return p;   /* 全 0 = 全白（PBM 里 1 是黑，0 是白） */
}

static void set_px(unsigned char *page, int x, int y)
{
    int bpl = (A4_W + 7) / 8;
    page[(size_t)y * bpl + (x >> 3)] |= (unsigned char)(0x80u >> (x & 7));
}

static int write_pbm(const char *dir, const char *name, const unsigned char *page)
{
    char path[512];
    int bpl = (A4_W + 7) / 8;
    FILE *f;

    snprintf(path, sizeof path, "%s/%s.pbm", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fprintf(f, "P4\n%d %d\n", A4_W, A4_H);
    if (fwrite(page, 1, (size_t)A4_H * bpl, f) != (size_t)A4_H * bpl) {
        fclose(f);
        return 1;
    }
    fclose(f);
    printf("  %s\n", path);
    return 0;
}

static int gen_patterns(const char *dir)
{
    unsigned char *p;
    int x, y, bpl = (A4_W + 7) / 8;

    printf("生成差分打印图案集 -> %s\n", dir);

    /* 1. 全白：压缩下限 */
    p = alloc_page();
    if (!p) return 1;
    if (write_pbm(dir, "01-white", p)) return 1;
    free(p);

    /* 2. 全黑：压缩上限 */
    p = alloc_page();
    if (!p) return 1;
    memset(p, 0xff, (size_t)A4_H * bpl);
    if (write_pbm(dir, "02-black", p)) return 1;
    free(p);

    /* 3. 1px 棋盘：最坏情况，暴露原始位图体积 */
    p = alloc_page();
    if (!p) return 1;
    for (y = 0; y < A4_H; y++)
        for (x = (y & 1); x < A4_W; x += 2)
            set_px(p, x, y);
    if (write_pbm(dir, "03-checker1", p)) return 1;
    free(p);

    /* 4. 上半白下半黑：定位分带边界 */
    p = alloc_page();
    if (!p) return 1;
    memset(p + (size_t)(A4_H / 2) * bpl, 0xff, (size_t)(A4_H - A4_H / 2) * bpl);
    if (write_pbm(dir, "04-half", p)) return 1;
    free(p);

    /* 5. 只有一个点在左上角：定位位图起始偏移 */
    p = alloc_page();
    if (!p) return 1;
    set_px(p, 0, 0);
    set_px(p, 1, 0);
    set_px(p, 0, 1);
    set_px(p, 1, 1);
    if (write_pbm(dir, "05-dot", p)) return 1;
    free(p);

    /* 6. 竖向 2px / 2px 条纹：区分行独立压缩 vs 整带压缩 */
    p = alloc_page();
    if (!p) return 1;
    for (y = 0; y < A4_H; y++)
        if ((y / 2) & 1)
            memset(p + (size_t)y * bpl, 0xff, (size_t)bpl);
    if (write_pbm(dir, "06-stripes", p)) return 1;
    free(p);

    /* 7. 50% 网点（8x8 有序抖动）：检验半色调在前端还是驱动内完成 */
    p = alloc_page();
    if (!p) return 1;
    {
        static const int m[8][8] = {
            { 0,32, 8,40, 2,34,10,42}, {48,16,56,24,50,18,58,26},
            {12,44, 4,36,14,46, 6,38}, {60,28,52,20,62,30,54,22},
            { 3,35,11,43, 1,33, 9,41}, {51,19,59,27,49,17,57,25},
            {15,47, 7,39,13,45, 5,37}, {63,31,55,23,61,29,53,21}
        };
        for (y = 0; y < A4_H; y++)
            for (x = 0; x < A4_W; x++)
                if (m[y & 7][x & 7] < 32)
                    set_px(p, x, y);
    }
    if (write_pbm(dir, "07-halftone50", p)) return 1;
    free(p);

    printf("完成。下一步：把这 7 张转成 PDF，用 Windows 驱动逐张打印并抓包。\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
        return selftest();
    if (argc >= 3 && strcmp(argv[1], "--patterns") == 0)
        return gen_patterns(argv[2]);

    fprintf(stderr,
            "用法:\n"
            "  %s --selftest\n"
            "  %s --patterns <目录>\n", argv[0], argv[0]);
    return 2;
}
