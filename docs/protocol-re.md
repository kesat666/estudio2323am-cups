# e-STUDIO 2323AM 作业流协议（已解出）

**结论：不是私有协议。** 外壳是标准 PJL，压缩是标准 JBIG（ITU-T T.82），
没有任何自定义压缩算法。项目原计划里风险最高的那部分（"压缩算法要是私有的就做不出来"）
已经不存在了。

解出方式：在 Windows 上把官方 GDI 驱动的输出用 `Local Port` 重定向成文件，
拿到 39,700 字节的真实作业流，然后解析 + 反向组包比对 + 解码验证。
方法见 `docs/capture-guide.md`，工具见 `tools/pjl_probe.py`。

---

## 1. 完整报文结构

一页 A4 / 600dpi / 单面的实际字节（`C:\captures\white.prn`，39,700 字节）：

```
偏移 0x000  1b 25 2d 31 32 33 34 35 58     \x1b%-12345X   UEL，无换行紧跟 @PJL
偏移 0x009  "@PJL\r\n"
偏移 0x00F  "@PJL SET TIMESTAMP=2026/10/22 23:22:01\r\n"
            "@PJL SET COMPRESS=JBIG\r\n"                ← 压缩方式，标准 JBIG
            "@PJL SET USERNAME=Administrator\r\n"
            "@PJL SET COVER=OFF\r\n"
            "@PJL SET HOLD=OFF\r\n"
            "@PJL SET ROTATESORT=OFF\r\n"
            "@PJL SET SMOOTHING=OFF\r\n"
            "@PJL SET TONERSAVE=OFF\r\n"
            "@PJL SET DEPTCODENUM=\"\"\r\n"
            "@PJL SET MEDIASOURCE=AUTO\r\n"
            "@PJL SET MEDIATYPE=PAPER\r\n"
            "@PJL SET PAGESTATUS=START\r\n"
            "@PJL SET COPIES=1\r\n"
            "@PJL SET DUPLEX=OFF\r\n"
            "@PJL SET PAPER=A4\r\n"
            "@PJL SET PAPERWIDTH=4961\r\n"                ← 像素宽度
            "@PJL SET PAPERLENGTH=7016\r\n"               ← 像素高度
            "@PJL SET RESOLUTION=600\r\n"
            "@PJL SET ORIENTATION=PORTRAIT\r\n"
偏移 0x1F5  "@PJL SET IMAGELEN=39106\r\n"              ← 数据段长度
偏移 0x20A  <JBIG BIE，39106 字节>
偏移 0x0F0A "@PJL SET DOTCOUNT=855608\r\n"             ← 本页黑点数
            "@PJL SET PAGESTATUS=END\r\n"
            "@PJL EOJ\r\n"
            "\x1b%-12345X\r\n"                          ← 结尾 UEL + CRLF
```

要点：

- **UEL 与 `@PJL` 之间没有换行**，`@PJL` 后面直接 `\r\n`。
- 所有 SET 行以 `\r\n` 结尾，**顺序固定**，字段名全大写。
- **`IMAGELEN` 在数据之前**，所以编码器必须先编完整页、拿到长度，才能写表头。
  这是 filter 里整页缓冲（A4 4.35 MB）的唯一原因。
- 尾部 `DOTCOUNT` 是黑点总数，仅统计用；`PAGESTATUS=END` 与 `EOJ` 收尾。

**`tools/pjl_probe.py --selftest` 用解析结果重新组包，与原始文件逐字节一致**
（39,700 → 39,700 字节，0 字节差异）。这是本节可信的依据。

## 2. 字段取值表

| 字段 | 观测值 | 说明 / 来源 |
|---|---|---|
| `COMPRESS` | `JBIG` | 固定。数据段即 T.82 BIE |
| `USERNAME` | `Administrator` | Windows 登录用户；我们写 CUPS 的 argv[2] |
| `COVER` / `HOLD` | `OFF` | 固定 |
| `ROTATESORT` | `OFF` / `ON` | 旋转分页 |
| `SMOOTHING` | `OFF` / `ON` | 平滑（2400×600 就靠它） |
| `TONERSAVE` | `OFF` / `ON` | 省墨，对应 PPD 的 TonerSave |
| `MEDIASOURCE` | `AUTO`、`MANUALFEED` | 手送是 `MANUALFEED`，不是常见的 `BYPASS` |
| `MEDIATYPE` | `PAPER` | 普通纸；厚纸等取值未采集 |
| `PAGESTATUS` | `START`（页头）/ `END`（页尾） | 同一字段两处出现，**靠位置区分** |
| `COPIES` | `1` | CUPS 默认只传 1，多份由 CUPS 复制 |
| `DUPLEX` | `OFF` / `ON` | **只有通断**，见 §3 |
| `BINDING` | `LONGEDGE` / `SHORTEDGE` | 双面时才有，见 §3 |
| `PAPER` | `A4`、`A3`、`CUSTOM` | 按像素尺寸反推 |
| `PAPERWIDTH` / `PAPERLENGTH` | 4961 / 7016 | 像素，600dpi 下的 A4 |
| `RESOLUTION` | `600` | 只观测到 600 |
| `ORIENTATION` | `PORTRAIT` | 横向由驱动先旋转光栅，仍报 PORTRAIT |
| `IMAGELEN` | 39106 | 数据段字节数 |
| `DOTCOUNT` | 855608 | 黑点数，统计用 |

字段名与取值还有第二个独立来源：驱动 DLL
`C:\Windows\System32\spool\drivers\x64\3\eSSh6OCM.dll` 里的 PJL 模板字符串，
与实测报文完全吻合，并额外暴露了本机没打印到的 `BINDING`、`MANUALFEED`。

## 3. 双面：是两个字段，不是一个

这是最容易搞错的地方，驱动 DLL 里能直接看到：

```
@PJL SET DUPLEX=OFF
@PJL SET DUPLEX=ON
@PJL SET BINDING=LONGEDGE
@PJL SET BINDING=SHORTEDGE
@PJL SET MANUALDUPLEX=FLIPSIDE      ← 手动双面（手送翻面），另一回事
```

所以：

| 模式 | 下发 |
|---|---|
| 单面 | `DUPLEX=OFF` |
| 双面长边装订 | `DUPLEX=ON` + `BINDING=LONGEDGE` |
| 双面短边装订 | `DUPLEX=ON` + `BINDING=SHORTEDGE` |

**不要写 `DUPLEX=FLIPSIDE`**——那是 `MANUALDUPLEX` 的取值，语义完全不同。

本机已确认加装 MD-0107 双面单元，双面是必支持功能。但 PPD 里仍默认单面：
协议未上机验证前，宁可打单面，也不要错送双面指令导致卡纸。

## 4. 数据段：标准 JBIG BIE

`COMPRESS=JBIG` 之后紧跟的是 ITU-T T.82 的 BIE（Bi-level Image Entity）：

```
00 00 01 00   DL   = 256      256 行一条带
00 00 13 61   D    = 4961     ← 等于 PAPERWIDTH
00 00 1b 68   RP   = 7016     ← 等于 PAPERLENGTH
00 00 00 80   L0   = 128      默认表大小
00 00 03 48   MX   = 840      ATMOVE 最大 x 偏移
ff            MY   = 255
02 ff 02 ff 02 ...            SC/SDNORM marker，之后是算术编码数据
```

编码参数对齐（`src/tsb_jbig.c`）：

- **`order=0`**：jbigkit 默认是 `JBG_ILEAVE|JBG_SMID`（0x3），写出来的头是
  `DL=0x40100`；厂商用 order=0，头是 `DL=0x100`。必须显式设 0。
- `L0=128`、`MY=255` 可对齐。`MX=840` 无法对齐：jbigkit 的 `MX_MAX=127`，
  它的 `jbg_enc_options` 在 `my>=128` 时还会跳过 mx 的设置。
  MX 只影响编码器的 ATMOVE 优化，不影响解码正确性，属于可接受差异。

### 已做的四项验证

| # | 验证 | 结果 |
|---|---|---|
| 1 | PJL 层反向组包 | 39,700 字节**逐字节一致** |
| 2 | 厂商数据段解码 | `jbgtopbm` 解出 `P4 4961 7016`，尺寸与字段吻合 |
| 3 | 解码结果重编码 | 39,106 → 37,055 字节，再解码与原图 `cmp` **完全一致** |
| 4 | 我们自己的编码器端到端 | 生成的 `.prn` 解码回 4961×7016，画的第 100–199 行黑带、7000–7015 行黑边**位置与黑点数完全正确** |

第 4 项是本项目的关键闭环：`src/tsb_enc.c` 输出的报文能被标准 JBIG 解码器还原成
预期图像，说明位图极性（1=黑）、行对齐、位序、页高都正确。

## 5. 还没验证的（上真机才能确认）

诚实标注，不要当成已知：

1. **打印机是否接受 jbigkit 产出的 BIE 头**。解码方向已通过，编码方向只能上机看。
   若出花屏或拒收，先把 `order` 改成 `JBG_SEQ`，或去掉 `my=255` 试单条带。
   → **2026-10-28 上机确认：通过。** 单面 A4 能正常出纸，JBIG BIE 头被接受。
2. **多页作业的 EOJ 位置**。当前实现是每页 `PAGESTATUS=END`、整个作业末尾一个
   `EOJ`。单页样本无法区分"每页 EOJ"与"作业末尾 EOJ"。
3. **`BINDING` 的写法**。
   → **2026-10-28 已从驱动 DLL 定案。** 从 `eSSh6OCM.dll` 提取到 Windows 驱动
     实际 PJL 模板：`@PJL SET DUPLEX=ON` + `@PJL SET BINDING=LONGEDGE|SHORTEDGE`，
     与本驱动发出的逐字相同，故 token 无误、无需再改。
   → 仍未解释：本机双面长/短边都打成短边。v0.1.1 与 v0.1.2 的 BINDING 逻辑逐字
     相同；设备若真忽略 BINDING，则 Windows 也应短边——因此更可能是"CUPS 选项
     未传到 filter"或"驱动对长边做了背面图像旋转"。需在真机用显式
     `-o Duplex=DuplexNoTumble` 打一页确认。
4. **`MEDIATYPE` 厚纸取值**、`PAPER` 的完整取值表。
5. **`DOTCOUNT` 是否被校验**。若打印机拿它做校验，算错会拒页。

## 10. 真正的根因：PAPER=CUSTOM（2026-10-28，v0.1.4）

v0.1.3 把默认托盘改成 `TRAY1` 后，真机仍然**不管选哪个纸盒都从旁路出纸，双面也不对**。
说明问题不在托盘 token，而在更上游：报文里的纸张尺寸是 `PAPER=CUSTOM`。

**证据链**

1. PPD 把可打印区写成 `ImageableArea A4: "18 18 577.28 823.89"`（比整页小 36pt）。
   CUPS 按可打印区光栅化，A4 送出的是 **4661×6716**，而 Windows 驱动送的是整页
   **4961×7016**。
2. 编码器只按像素查表反推 `PAPER`，4661×6716 对不上任何标准纸 → `PAPER=CUSTOM`。
3. 真机上"非标尺寸"意味着只能走手送（旁路），且该尺寸下双面被拒——两个异常同源。
4. 把 `ImageableArea` 改成整页（`0 0 595.28 841.89`）后，同一份 PDF 的捕获立刻变成
   `PAPER=A4` / `PAPERWIDTH=4961` / `PAPERLENGTH=7016`，与 Windows 样张逐字段一致。

**结论**：GDI 机型由主机渲染整页，页边留白由打印机处理；PPD 里声明页边距只会让
CUPS 缩小光栅，反而让打印机认不出纸型。`ImageableArea` 必须等于 `PageSize`。

同时把 `PAPER` 的判定改成三级：media 关键字 → raster 头 `cupsPageSize`（点）→ 像素兜底，
避免上游任何尺寸变化又把 token 打回 `CUSTOM`。

## 8. 上机结果（2026-10-28，v0.1.1）

- 单面 A4 打印成功 → 报文被接受，JBIG 编码方向 OK（§5.1 关闭）。
- **异常 1：出纸走旁路（手送）而非主纸盒。** 报文写死 `MEDIASOURCE=AUTO`，
  在真机上 AUTO 让打印机自选、且当前默认到旁路。主纸盒的真实 token 单面样张没抓到
  （参考 `white.prn` 也是 AUTO），需抓"驱动明确选主纸盒"的样张定 token。
- **异常 2：双面长边/短边都打成短边。** `BINDING=LONGEDGE|SHORTEDGE` 是看驱动 DLL
  字符串猜的映射，未抓过真实双面样张；设备表现像忽略该字段。需抓长边、短边两份
  双面样张，逐字段对比确认 `DUPLEX`/`BINDING` 的真实写法。
- 应对：v0.1.2 起 `MEDIASOURCE`、`MEDIATYPE`、`BINDING` 改为 PPD 可选项
  （`TSBMediaSource` / `TSBMT` / `TSBBinding`），默认值仍 AUTO / PAPER / 由 Duplex 推导，
  可在真机用 `lp -o TSBMediaSource=CASSETTE -o TSBBinding=LONGEDGE` 直接试不同 token，
  不必每次重编。正确 token 确认后写死成默认值。

## 9. 托盘 token 定案（2026-10-28 远程联调）

Windows 驱动配置文件 `eSSh6CFG.xml`（`C:\Windows\System32\spool\DRIVERS\x64\3`）
里的 `MediaInputTrayCheck` 表：

| 记录 | PJL token | 说明 |
|---|---|---|
| AutoSelectInputTray | `AUTO` | 打印机自选 |
| ManualFeed | `MANUALFEED` | 旁路托盘/手送 |
| Tray1 | `TRAY1` | 纸盒 1（主纸盒） |
| Tray2 | `TRAY2` | 选配纸盒 2（本机未装） |

用 `file://` 真实 CUPS 管线（临时 `FileDevice Yes` + 重启 cups）捕获 Windows 驱动
对"纸盒 1"的输出，实测 `@PJL SET MEDIASOURCE=TRAY1`，与上表一致。

**结论**：本机默认应发 `TRAY1`；`AUTO` 在本机会被解析成旁路托盘（即"出纸走旁路"
现象）。v0.1.3 起默认 `TRAY1`。

> 注：该 GDI 驱动的双面参数锁在 4892 字节的私有 DEVMODE 区，`Set-PrintConfiguration`
> / WMI / 标准 `dmDuplex` 均改不到，无法用编程方式生成 Windows 双面样张；
> 双面 token 只能靠上述 DLL 模板定案。

## 6. 工具

```bash
# 解析任意作业流，打印字段表 + BIE 头 + 压缩比
python tools/pjl_probe.py --parse C:\captures\white.prn

# 用解析结果重新组包，逐字节比对（验证理解是否正确）
python tools/pjl_probe.py --selftest C:\captures\white.prn

# 抓真实作业流（驱动端口指向代理）
python tools/port9100_proxy.py --target 192.168.1.50 --label white

# 通用统计（熵 / 压缩率 / 差分）
python tools/analyze_stream.py --stats captures/white.bin --page a4-600

# 直接向打印机发 PJL 查询（libusb，ctypes，无需 pyusb）
#   注意：2323AM 实测对 @PJL INFO/DINQUIRE 完全不应答（四个 IN 端点均超时），
#   保留此工具是为了以后接其他 PJL 机型时能立刻取证。
sudo python3 tools/pjlq.py "@PJL INFO ID" "@PJL INFO VARIABLES"
```

## 7. 上机验证清单

装好 `.deb` 后按顺序打，每步只看一个变量：

1. 一页空白 A4 → 出白纸、不卡纸 = 报文被接受
2. 一页实心黑块 → 位置和大小对 = 位图极性、行对齐正确
3. 一页正文 → 字迹不糊、不偏移 = 分辨率与 PAPERWIDTH 正确
4. 双面长边 → 背面朝向正确 = DUPLEX + BINDING 正确
5. 连续两页 → 页序正确、不叠印 = EOJ 位置正确

任何一步不对，把现象告诉我，对照 §5 的怀疑点逐个排。
