# e-STUDIO 2323AM：Linux（amd64 / arm64）CUPS 驱动

> **结论先行**
> 1. 2323AM/2823AM 是 **GDI（主机渲染）机型**，没有 PCL/PS，所以不存在"套个通用驱动"的可能——这条必须先接受。
> 2. 但 **Linux 侧没有架构门槛**。Windows 之所以难，是微软强制"打印驱动必须是原生 Arm64 二进制"，x64 模拟不覆盖打印驱动；Linux 没有这个约束，CUPS 是普通用户态进程，同一个 C 程序编两次就是 amd64 和 arm64 两个包。
> 3. 而且 **CUPS 已经替你做完光栅化**。你要写的不是"驱动"，是一个 **filter**：从 CUPS 拿到 1bpp / 600dpi 的页面位图，编码成机器认识的私有字节流，写到 `socket://ip:9100`。
> 4. 所以整个项目的唯一真难点是：**把那个私有编码格式逆出来**。工作量集中在逆向，不在跨架构。

---

## 一、事实核对

| 项目 | 结论 | 依据 |
|---|---|---|
| 页面描述语言 | GDI（主机渲染），无 PCL/PS | 东芝官方规格页 `Page Description Language: Graphic Device Interface`；同代 2829A 系列才有 PCL6/PS3/PCL5e |
| 官方 Linux 驱动 | 无 | 东芝下载站中 CUPS 驱动只覆盖 PCL/PS 机型（ColorMFP 系列），2323AM 的下载项只有 Windows |
| Windows 侧 | x64 官方已有，ARM64 无且不可能用 x64 | 兼容列表 32/64 bit；微软 Windows on Arm FAQ 明确要求打印驱动原生 Arm64、安装程序也须 Arm64 |
| 硬件 ID | `USBPRINT\TOSHIBA_e-STUDIO2323360E` | 驱动信息页；扫描部件 `VID_08a6 PID_8056` |
| 接口 | USB 2.0 / 10-100 以太网，RAW 9100 | 官方规格 |

---

## 二、为什么 Linux 路线比 Windows 路线轻得多

| 环节 | Windows | Linux |
|---|---|---|
| 架构约束 | 驱动必须原生 Arm64，另需 Arm64 安装器 | 无。普通 ELF 程序，`gcc` 两次即可 |
| 页面解释 | 自己实现（无 PDL，驱动要自带渲染） | **CUPS/Ghostscript 已把 PDF 光栅化成 1bpp 位图** |
| 驱动框架 | v4 驱动 + INF + GPD + filter pipeline | 一个可执行文件 + 一个 PPD + CUPS 配置 |
| 签名 | catalog 签名 / WHQL | 无签名要求 |
| 装机 | 驱动商店 / INF 安装 | `.deb` 拷贝到 `/usr/lib/cups/filter` |

一句话：**Linux 把"排版 + 光栅化 + 半色调"全部交给上游 CUPS 栈，你只负责"位图 → 压缩 → 私有报文"这一段。** 这一段是几百行 C，不是几万行。

---

## 三、数据通路

```
应用 (PDF/图片)
   │
   ▼  CUPS 调度
cups-filters: pdftoraster / gstoraster        ← 上游现成，不用写
   │  输出 application/vnd.cups-raster  (1bpp, 600dpi, 每行按字节对齐)
   ▼
rastertotoshiba                                ← ★ 你要写的唯一组件
   │  位图 → 分带 → 压缩 → 作业头/页头/页尾
   ▼
socket://<printer>:9100  (RAW)  或  usb:// (libusb)
   │
   ▼
e-STUDIO 2323AM 固件解析 → 成像
```

设计上把 **编码器核心**（`src/tsb_enc.c`）和 **CUPS 外壳**（`src/cups_filter.c`）彻底分开：
- 核心不依赖 cups 头文件，可在任何机器上编译、单测、用抓包样本回放验证；
- 协议逆出来之后，**只改核心**，外壳和打包完全不动；
- 两个架构共用同一份核心源码，交叉编译只换 `CC`。

---

## 四、逆向的可行路径（这是 90% 的工作量）

### 4.1 抓包
- **网络**：`sudo tcpdump -i any -s0 -w job.pcap host <printer> and port 9100`，Wireshark 里看 `TCP payload`。
- **USB**：`sudo modprobe usbmon`，Wireshark 抓 `usbmon`；或在 Windows 侧用 USBPcap 抓同一份文档，两边对照。
- 两边都要抓**反向通道**：GDI 机型的作业握手、状态回读常走同一条连接，只抓单向会误判"没有应答"。

### 4.2 差分打印（核心手法）
不要一上来分析真实文档。**打印设计好的图案集**，逐字节对比报文：

| 用例 | 目的 |
|---|---|
| 全白页 | 压缩下限。报文极短说明强压缩（RLE/JBIG）；接近 `宽×高/8` 说明基本没压 |
| 全黑页 | 压缩上限；与全白页一起可估出每行开销 |
| 1px 棋盘（隔点） | 最坏情况，能暴露"原始位图段"的真实体积 |
| 上半白下半黑 | 定位"分带（band）边界"在报文里的位置 |
| 仅左上角一个黑点 | 定位位图数据起始偏移 |
| A4 / A3 / Letter | 定位纸张字段 |
| 单面 / 双面 | 定位 duplex 控制位 |
| 同一页打两份 | 区分"作业头"与"页数据" |

然后 `tools/analyze_stream.py --diff a.bin b.bin` 看差异区间，`--stats` 看熵与重复字节分布。

### 4.3 判定压缩格式
- 全白页报文长度 ≈ 几十字节 → RLE / PackBits 类；
- 出现明显 `0x00 n` 成对模式 → 行程编码；
- 熵接近 1 bit/byte 且长度 ≈ 原始/8 → 无压缩直传；
- 有 `FF D8` / `FF xx` 结构 → JPEG 类（整页 JPEG 在 GDI 机型里很常见）；
- 固定头 + 后续渐进位平面 → MMR/G4 或 JBIG。

`tsb_enc.c` 里已放 PackBits 参考实现和原始位图导出模式（`TSB_DUMP_RAW=1`）：**先用它把同一页的原始 1bpp 位图 dump 出来，和抓包数据段逐字节比对**——能直接确认"到底压没压、从哪个字节开始是位图"。

### 4.4 回放验证（硬标准）
写一个 `--replay sample.bin` 模式原样重发，确认能出纸；再**用自己编码器生成同内容的报文**，与抓包逐字节相等。
**做到这一条，协议就算逆完了。** 做不到，说明还有没识别的字段。

### 4.5 REA 的位置
`github.com/morluto/rea`（MIT）是 MCP 服务，把 Ghidra/IDA/radare2 暴露给 agent。它能帮的：在厂商 Windows DLL 里找压缩函数、协议常量表、位图缓冲的调用链，直接回答"这个驱动到底用哪个库压的图"。它不能帮的：还原源码、替代抓包。**协议是运行期行为，静态分析只产生假设，必须用差分抓包验证。**
代价：要装 MCP + Ghidra（JDK）+ 重启会话，建议在 4.1/4.2 拿到样本之后再接。

---

## 五、工程结构

```
.
├── README.md                 本文
├── Makefile                  本地编译 + 交叉编译 + 打包
├── Dockerfile                buildx 多架构构建（amd64 + arm64）
├── src/
│   ├── tsb_enc.h             编码器接口（协议无关部分已定型）
│   ├── tsb_enc.c             编码器核心：缓冲、分带、PackBits、raw/capture dump
│   ├── cups_filter.c         CUPS filter 外壳：raster 读取 → 编码 → stdout
│   └── test_enc.c            核心单测 + 差分图案生成器，不依赖 CUPS
├── test/stub/cups/           无 CUPS 头文件时的语法校验存根（不参与正式构建）
├── ppd/
│   └── toshiba-estudio2323am.ppd.in   PPD 模板（协议未定时不可用，见文件头注释）
├── debian/                   .deb 打包（Architecture: amd64 arm64）
├── docs/
│   ├── protocol-re.md        逆向操作手册（用例矩阵、字段记录表、进度清单）
│   └── build-env.md          Linux 构建环境（WSL / Docker / CI 三条路）
├── .github/workflows/build.yml   CI：amd64 + arm64 自动构建与打包
└── tools/
    ├── probe_printer.py      Step 0：先确认是否真的只有私有协议
    └── analyze_stream.py     抓包分析：--stats / --entropy / --diff / --hexdump
```

### 当前验证状态（诚实标注）

| 部分 | 状态 |
|---|---|
| `tsb_enc.c` 编码器核心 | ✅ 已在本机用 gcc 15.2 编译通过，PackBits 编解码往返单测全过（含 128/129 边界、字面串跨 128 上限） |
| `test_enc --patterns` | ✅ 已跑通，能生成 7 张 A4 600dpi 1bpp 图案 |
| `analyze_stream.py` | ✅ 已用构造样本验证 stats / diff / 熵 / 标记识别 |
| `cups_filter.c` | ⚠️ 仅做了 `-fsyntax-only` 语法校验（本机无 CUPS 头文件），**未链接、未在真实 CUPS 上跑过** |
| PPD | ⚠️ 模板，未用 `ppd-c` 校验，装上也打不出纸 |
| **私有协议报文** | ❌ 未知。这是整个项目剩下的全部工作量 |


---

## 六、编译与打包

> 本机没有 Linux 环境（WSL 未启用且被安全策略拦截）。三条路的详细步骤见 **`docs/build-env.md`**；CI 已配好，推到 GitHub 即自动产出 amd64 / arm64 两个 `.deb`。

### 已采用：GitHub Actions（无需本机 Linux）

仓库已初始化并完成提交，作者为 `keast666 <keast606@gmail.com>`（`debian/rules` 的可执行位已写入 git index，换行统一为 LF）。发布用 `tools/publish.sh`，它会先体检再推：

```bash
./tools/publish.sh --check     # 只体检：账号是否存在、仓库是否已建、SSH 是否就绪
./tools/publish.sh --ssh       # SSH 方式（先把 ~/.ssh/id_ed25519.pub 加到 GitHub）
GH_TOKEN=ghp_xxx ./tools/publish.sh --token   # HTTPS+PAT，自动建仓并推送
```

账号或仓库名不同时用环境变量覆盖：`GH_USER=xxx GH_REPO=yyy ./tools/publish.sh --ssh`。

然后在 Actions 页面看 `build (amd64)` 与 `build (arm64)` 两个 job，跑完在 job 页面底部下载 artifact：

- `toshiba-estudio2323am-amd64` → `toshiba-estudio2323am_0.0.1_amd64.deb`
- `toshiba-estudio2323am-arm64` → `toshiba-estudio2323am_0.0.1_arm64.deb`

CI 做的事（每一步失败都会红）：

1. `debian:bookworm` 容器内 `make` → 对**真实 CUPS 头文件**编译并链接；
2. `make test` → 编码器单测；
3. `chmod +x debian/rules` → 兜底 Windows 提交丢失可执行位的情况；
4. `dpkg-buildpackage -us -uc -b` → 出 `.deb`；
5. `dpkg-deb -f` 校验包架构与 matrix 一致。

私有仓库同样可用；arm64 走 QEMU，整体约 5–8 分钟。

> 注意：CI 只证明"能编译、能打包"，**不证明能打印**。协议未逆出来之前，产物安装后依然打不出纸。

### 本地（在 Linux 上）
```bash
sudo apt install build-essential libcups2-dev
make            # 产出 rastertotoshiba
make test       # 编码器单测
sudo make install
```

### amd64 + arm64 双架构
```bash
# 方式一：docker buildx（推荐，无需交叉工具链）
docker buildx build --platform linux/amd64,linux/arm64 -t print2323am .

# 方式二：Debian multiarch 交叉编译
sudo dpkg --add-architecture arm64
sudo apt install gcc-aarch64-linux-gnu libcups2-dev:arm64
make ARCH=arm64 CC=aarch64-linux-gnu-gcc

# 方式三：在目标机上直接编译（最省事，国产系统一般自带 gcc）
```

### 打 .deb
```bash
dpkg-buildpackage -us -uc -a amd64
dpkg-buildpackage -us -uc -a arm64
```

> 注意：**CUPS 3.x 移除了 PPD 与 filter 模型**，改为 IPP 化的 "Printer Application"（PAPPL）。本项目面向 CUPS 2.4（Debian 12 / Ubuntu 22.04–24.04 / 麒麟 V10 / 统信 UOS 均为 2.x）。若目标系统是 CUPS 3，filter 需改造成 PAPPL 应用，工作量另算。

---

## 七、失败风险（必须在投入前认知）

1. **压缩格式私有且带校验/加密**。若位图段既不是 RLE 也不是 JPEG，且带滚动校验，逆向周期会从"2 周"变成"不确定"。**建议先花 2 周做 4.1–4.3，确认能解出一页再建工程。**
2. **双向握手不可省略**。部分 GDI 机型要求主机先回应设备的状态查询，否则作业被丢弃。届时 filter 不能是单向写，需要实现一个小型回读循环（socket 双向），架构上要提前留位置。
3. **CUPS 版本分裂**。2.x/3.x 模型不同，见上。
4. **法律边界**。为自己拥有的设备做互操作实现通常属正当使用，但**不要分发厂商二进制、不要反编译后重新打包厂商代码**。自研编码器是干净实现（clean-room），这是唯一安全的做法。

---

## 八、下一步需要你定的

1. 打印机是否联网？给个 IP，先跑 `tools/probe_printer.py` 排掉"其实支持 IPP/PCL"的可能（5 分钟成本，收益可能是整个项目不用做）。
2. 能否提供**一份 Windows 驱动打同一页时的网络抓包**（Wireshark `.pcap`）？有它就能直接开始格式分析。
3. 目标系统：Ubuntu / Debian / 麒麟 / 统信？决定 PPD 路径与包格式。
4. 是否要复刻双面、纸盒选择、省墨？只做"能打单面 A4"的话，字段可以少逆一半。
