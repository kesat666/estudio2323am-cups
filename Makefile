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

CFLAGS      ?= -O2
CFLAGS      += -std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
LDFLAGS     ?=

BIN         := rastertotoshiba
TESTBIN     := test_enc
SRC         := src/tsb_enc.c
FILTER_SRC  := src/cups_filter.c $(SRC)
TEST_SRC    := src/test_enc.c $(SRC)

.PHONY: all test syntax patterns install uninstall clean deb help

all: $(BIN)

$(BIN): $(FILTER_SRC) src/tsb_enc.h
	$(CC) $(CFLAGS) $(CUPS_CFLAGS) -o $@ $(FILTER_SRC) $(CUPS_LIBS) $(LDFLAGS)

$(TESTBIN): $(TEST_SRC) src/tsb_enc.h
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(LDFLAGS)

test: $(TESTBIN)
	./$(TESTBIN) --selftest

# 没装 libcups2-dev 时，用存根头文件只做语法检查（抓拼写/类型错误）
syntax:
	$(CC) -std=c99 -Wall -Wextra -fsyntax-only -I test/stub $(FILTER_SRC)
	$(CC) -std=c99 -Wall -Wextra -fsyntax-only $(TEST_SRC)

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
