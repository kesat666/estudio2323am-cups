# 多架构构建镜像
#
#   docker buildx build --platform linux/amd64,linux/arm64 \
#          --output type=local,dest=./dist .
#
# 说明：这里用「原生平台 + QEMU 模拟」而不是交叉工具链，目的是让
# `make test` 真的在目标架构上跑一遍。arm64 下会慢一些，但结果可信。

FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential pkg-config libcups2-dev file \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY Makefile ./
COPY src ./src
COPY test ./test
COPY ppd ./ppd

# 构建 + 单测 + 打印产物架构，任何一步失败都让镜像构建失败
RUN make clean \
 && make \
 && make test \
 && echo "--- artifact ---" \
 && file rastertotoshiba \
 && ldd rastertotoshiba

RUN mkdir -p /out && cp rastertotoshiba /out/
