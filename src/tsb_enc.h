/*
 * tsb_enc.h — e-STUDIO 2323AM 私有打印流编码器（核心，不依赖 CUPS）
 *
 * 设计原则：
 *   - 这里只做"位图 -> 字节流"，不做排版、不做光栅化（那些 CUPS 上游已经做完）。
 *   - 不包含任何 cups 头文件，可在 Windows/Linux/任意架构编译、单测、回放。
 *   - 协议未确认前，tsb_page_band() 的具体报文由 TSB_PAYLOAD_MODE 决定：
 *       RAW      : 直接输出 1bpp 原始位图（用于和抓包做逐字节比对，这是逆向前提）
 *       PACKBITS : PackBits 行程压缩（常见 GDI 机型用，作为第一假设）
 *       CAPTURE  : 输出带偏移标记的转储，方便人工比对
 *     协议确认后，把 tsb_page_band() 里的封装换成真实报文即可，接口不变。
 */

#ifndef TSB_ENC_H
#define TSB_ENC_H

#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TSB_OK   0
#define TSB_ERR (-1)

/* 编码模式：逆向前用 RAW/CAPTURE 做比对，逆向完成后切到真实编码 */
typedef enum {
    TSB_PAYLOAD_RAW      = 0,
    TSB_PAYLOAD_PACKBITS = 1,
    TSB_PAYLOAD_CAPTURE  = 2
} tsb_payload_mode;

typedef struct {
    int  duplex;          /* 0 单面, 1 双面长边, 2 双面短边 */
    int  toner_save;      /* 省墨 */
    int  media_width_px;  /* 纸张宽度（像素，按 x_dpi 计） */
    int  media_height_px;
    int  x_dpi;
    int  y_dpi;
    int  copies;          /* 1 表示由 CUPS 复制, >1 表示本 filter 自己循环 */
    int  band_height;     /* 分带高度（行），0 表示不分带 */
    tsb_payload_mode mode;
} tsb_jobopts;

typedef struct tsb_ctx tsb_ctx;

/* 生命周期 */
tsb_ctx *tsb_open(FILE *out, const tsb_jobopts *opts);
void     tsb_close(tsb_ctx *c);

/* 作业级 */
int tsb_job_begin(tsb_ctx *c);
int tsb_job_end(tsb_ctx *c);

/* 页级 */
int tsb_page_begin(tsb_ctx *c, int width_px, int height_px, int copy_index);
int tsb_page_band(tsb_ctx *c, const unsigned char *band, int y0, int nlines,
                  int bytes_per_line);
int tsb_page_end(tsb_ctx *c);

/* 协议无关的原语，可独立单测 */
size_t tsb_packbits_encode(const unsigned char *in, size_t n,
                           unsigned char *out, size_t cap);

/* 统计，便于回放比对 */
typedef struct {
    unsigned long long bytes_out;
    unsigned long long bands;
    unsigned long long pages;
} tsb_stats;

const tsb_stats *tsb_get_stats(const tsb_ctx *c);

#ifdef __cplusplus
}
#endif
#endif /* TSB_ENC_H */
