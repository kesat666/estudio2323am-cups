# e-STUDIO 2323AM CUPS 驱动 — 构建
#
# 常用：
#   make                      编译 filter（需要 libcups2-dev）
#   make test                 编码器单测
#   make syntax               没有 CUPS 头文件时做语法校验
#   make patterns             生成差分打印图案集
#   sudo make install         装到 /usr/lib/cups/filter
#
# 交叉编译 arm64：
#   make ARCH=arm64 CC=aarch64-linux-gnu-gcc
#
# 注意：CC ?= cc 只在外部未指定时生效，交叉编译请显式传 CC。

PREFIX      ?= /usr
FILTERDIR   ?= $(PREFIX)/lib/cups/filter
PPDDIR      ?= $(PREFIX)/share/cups/model
DESTDIR     ?=

CC          ?= cc
PKG_CONFIG  ?= pkg-config

CUPS_CFLAGS := $(shell $(PKG_CONFIG) --cflags cups 2>/dev/null)
CUPS_LIBS   := $(shell $(PKG_CONFIG) --libs cups 2>/dev/null)
ifeq ($(strip $(CUPS_LIBS)),)
CUPS_LIBS   := -lcups
endif

# raster API（cupsRasterOpen/ReadHeader/ReadPixels）的归属随 CUPS 版本变化：
#   CUPS 2.2 / 2.3  → 在 libcupsimage 里（麒麟 V10 2.2.10、统信 UOS 2.2.x 属于这类）
#   CUPS 2.4 及以后 → 已合并进 libcups，libcupsimage 被删除
# 所以这里做动态探测：先问 pkg-config，再直接问编译器库搜索路径，
# 找不到就不加 -lcupsimage（否则在 CUPS 2.4 系统上会因找不到库而链接失败）。
CUPS_IMAGE_LIBS := $(shell $(PKG_CONFIG) --libs cupsimage 2>/dev/null)
ifeq ($(strip $(CUPS_IMAGE_LIBS)),)
# 注意：这里的命令不能出现 ')'，否则会提前闭合 $(shell ...)
CUPS_IMAGE_LIBS := $(shell lib=$$($(CC) -print-file-name=libcupsimage.so); test -f "$$lib" && echo -lcupsimage)
endif

# 链接顺序要求：cupsimage 依赖 cups 的符号，必须排在前面
CUPS_LINK_LIBS := $(strip $(CUPS_IMAGE_LIBS) $(CUPS_LIBS))

# libjbig（ITU-T T.82 JBIG）：驱动实测头里是 @PJL SET COMPRESS=JBIG，
# 数据段就是标准 BIE，所以压缩必须用 libjbig，不能用自研算法。
# 头文件存在即启用，编译期加 -DTSB_HAVE_JBIG；不存在时 tsb_jbig.c 退化为
# 空实现（能编译、能单测，但出不了可打印报文，运行时会明确报错）。
JBIG_HDR    := $(wildcard /usr/include/jbig.h) $(wildcard /usr/local/include/jbig.h)
ifneq ($(strip $(JBIG_HDR)),)
JBIG_CFLAGS := -DTSB_HAVE_JBIG
JBIG_LIBS   := -ljbig
endif
# 允许外部覆盖（交叉编译 sysroot 里的 jbig.h 不在默认路径时）
JBIG_CFLAGS ?=
JBIG_LIBS   ?=

CFLAGS      ?= -O2
CFLAGS      += -std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
LDFLAGS     ?=

BIN         := rastertotoshiba
TESTBIN     := test_enc
SRC         := src/tsb_enc.c src/tsb_jbig.c
FILTER_SRC  := src/cups_filter.c $(SRC)
TEST_SRC    := src/test_enc.c $(SRC)
HDRS        := src/tsb_enc.h src/tsb_jbig.h

.PHONY: all test syntax patterns install uninstall clean deb help

all: $(BIN)

$(BIN): $(FILTER_SRC) $(HDRS)
	$(CC) $(CFLAGS) $(JBIG_CFLAGS) $(CUPS_CFLAGS) -o $@ $(FILTER_SRC) $(CUPS_LINK_LIBS) $(JBIG_LIBS) $(LDFLAGS)

$(TESTBIN): $(TEST_SRC) $(HDRS)
	$(CC) $(CFLAGS) $(JBIG_CFLAGS) -o $@ $(TEST_SRC) $(JBIG_LIBS) $(LDFLAGS)

test: $(TESTBIN)
	./$(TESTBIN) --selftest

# 没装 libcups2-dev 时，用存根头文件只做语法检查（抓拼写/类型错误）
syntax:
	$(CC) -std=c99 -Wall -Wextra $(JBIG_CFLAGS) -fsyntax-only -I test/stub $(FILTER_SRC)
	$(CC) -std=c99 -Wall -Wextra $(JBIG_CFLAGS) -fsyntax-only $(SRC)

patterns: $(TESTBIN)
	mkdir -p patterns
	./$(TESTBIN) --patterns patterns

install: $(BIN)
	install -d $(DESTDIR)$(FILTERDIR)
	install -m 755 $(BIN) $(DESTDIR)$(FILTERDIR)/$(BIN)
	install -d $(DESTDIR)$(PPDDIR)
	install -m 644 ppd/toshiba-estudio2323am.ppd.in \
		$(DESTDIR)$(PPDDIR)/toshiba-estudio2323am.ppd

uninstall:
	rm -f $(DESTDIR)$(FILTERDIR)/$(BIN)
	rm -f $(DESTDIR)$(PPDDIR)/toshiba-estudio2323am.ppd

deb:
	dpkg-buildpackage -us -uc -b

clean:
	rm -f $(BIN) $(TESTBIN) *.exe *.o
	rm -rf patterns

help:
	@echo "make | test | syntax | patterns | install | deb | clean"
