/*
 * tsb_jbig.h — 1bpp 位图 -> JBIG BIE（ITU-T T.82）
 *
 * 依据：驱动实测报文头里写着 `@PJL SET COMPRESS=JBIG`，数据段以标准 BIE
 * 头开始（DL/D/RP/L0/MX/MY，其中 D=4961 正好等于 PAPERWIDTH），
 * 用 jbigkit 的 jbgtopbm 解码后得到 4961x7016 的 PBM，重编码再解码图像完全一致。
 * 所以压缩是标准 JBIG，不是私有格式。
 *
 * 本文件把 libjbig 的用法收敛成一个函数，便于：
 *   - 没有 libjbig 的环境仍能编译（返回 -1，不产生半截数据）
 *   - 单测里直接对比编码/解码往返
 */

#ifndef TSB_JBIG_H
#define TSB_JBIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 编码一整页。bits 为连续 1bpp 位图，每行 bpl 字节、共 height 行，
 * 高位在前（PBM/CUPS 约定），1 = 黑。
 *
 * 成功返回 0，*out 指向 malloc 出来的 BIE（调用方 free），*out_len 为长度。
 * 失败返回 -1（*out 为 NULL）。
 */
int tsb_jbig_encode(const unsigned char *bits, int width, int height, int bpl,
                    unsigned char **out, size_t *out_len);

/* 1 表示本次编译链接了 libjbig；0 表示是空实现，无法出纸 */
int tsb_jbig_available(void);

#ifdef __cplusplus
}
#endif
#endif /* TSB_JBIG_H */
