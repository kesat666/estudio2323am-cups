# Linux 构建环境

## 先回答：现在需要吗？

**严格说不需要，而且现在也不该装。**

理由：当前卡点是**协议逆向**，不是编译器。`tsb_enc` 核心已经在本机 gcc 15.2 下编译通过、单测全过，那部分代码是纯 C99、与平台无关。真正必须用 Linux 的时刻是：

1. 要对 `cups_filter.c` 做真链接（需要 `libcups2-dev`）；
2. 要跑 `dpkg-buildpackage` 出 `.deb`；
3. 要在 arm64 上验证产物。

这三件事都发生在**协议确认之后**。在那之前，装 WSL 只是给系统加一个重启风险。

但既然迟早要做，下面是三条路，按推荐顺序。

---

## 路线 A：GitHub Actions（已采用）

`.github/workflows/build.yml` 已就绪，仓库也已推送完成：`kesat666/estudio2323am-cups`，分支 `main`。后续只需：

```bash
git push          # origin 已配置好，SSH 认证已通过
```

即可在 Actions 页面看到 `build (amd64)` 与 `build (arm64)` 两个 job 并行，产物是 `.deb` 形式的 artifact。

注意：仓库是**私有**的，Actions 页面和 artifact 都需要登录 GitHub 才能访问，命令行匿名查询会得到 404（不是故障）。

优点：不动本机、两个架构都真跑一遍（不是交叉编译的假设）、版本可追溯。
缺点：需要把代码推到 GitHub（私有仓库同样可用，免费额度够）。

下面两条路线保留作备选。

本地等价的一条命令（有 Docker 即可，不需要 WSL）：

```bash
docker buildx build --platform linux/amd64,linux/arm64 -t print2323am .
```

---

## 路线 B：WSL2 + Ubuntu 22.04

### 这台机器的现状（已实测）

| 项目 | 值 |
|---|---|
| 系统 | Windows 10 专业版 19045 |
| CPU | i7-1165G7，VT-x 可用，SLAT 可用 |
| 当前权限 | 管理员 |
| `Microsoft-Windows-Subsystem-Linux` | **Disabled** |
| `VirtualMachinePlatform` | **Disabled** |
| `HypervisorPresent` | False |

结论：**硬件完全支持 WSL2，只是功能没开**。开完需要重启一次。

### 操作步骤（管理员 PowerShell）

```powershell
# 1. 启用两个必需功能（不自动重启）
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart

# 2. 重启（必须，否则下一步会报 0x80370102）
Restart-Computer
```

重启后：

```powershell
# 3. Windows 10 需要单独装 WSL2 内核更新包
#    下载：https://wslstorestorage.blob.core.windows.net/wslblob/wsl_update_x64.msi
#    双击安装，或：
#    msiexec /i wsl_update_x64.msi /qn

# 4. 设为默认 WSL2
wsl --set-default-version 2

# 5. 装发行版
wsl --install -d Ubuntu-22.04

# 若上一条报「找不到发行版」，说明本机 wsl 是收件箱旧版，先升级：
#   wsl --update
# 仍不行则离线装：
#   下载 https://aka.ms/wslubuntu2204 得到 .appx
#   Add-AppxPackage .\Ubuntu_2204.appx
```

验证：

```powershell
wsl -l -v          # 应显示 VERSION 2
wsl -- uname -m    # 应显示 x86_64
```

### 进入 WSL 后装依赖并构建

```bash
sudo apt update
sudo apt install -y build-essential pkg-config libcups2-dev debhelper dpkg-dev

cd /mnt/d/e-STUDIO_2323AM
make            # 出 rastertotoshiba
make test       # 编码器单测
make syntax
sudo make install

# 出 deb
dpkg-buildpackage -us -uc -b
```

arm64 产物（在 x86 WSL 里交叉编译）：

```bash
sudo dpkg --add-architecture arm64
sudo apt update
sudo apt install -y gcc-aarch64-linux-gnu libcups2-dev:arm64
make clean && make CC=aarch64-linux-gnu-gcc
file rastertotoshiba        # 应显示 ARM aarch64
```

> 注意：`dpkg-buildpackage -a arm64` 需要完整的 arm64 依赖树，比交叉编译麻烦。要出 arm64 的 deb，用路线 A 的 QEMU 或直接在 arm64 机器上打，最省事。

---

## 路线 C：GitHub 之外的远程 Linux

任何一台能 SSH 的 Linux 机器（含国产系统、树莓派、云主机）：

```bash
scp -r /path/to/e-STUDIO_2323AM user@host:/tmp/
ssh user@host 'cd /tmp/e-STUDIO_2323AM && sudo apt install -y build-essential libcups2-dev && make && make test'
```

arm64 机器上直接 `make` 即得 arm64 产物，无需交叉工具链。

---

## 一个限制要说清楚

当前会话里 **`wsl.exe` 被本机安全策略列入程序黑名单**，我无法调用它（尝试会被沙箱拦截，且明确不可绕过）。所以即使功能开好、系统重启完，最后执行 `wsl --install` 和进入 WSL 跑构建的那几步**仍需你手动执行**，或者由你把 `wsl.exe` 从黑名单移除：`安全中心 → Command Security → Program Blacklist`。

除了 WSL 本身，其余我都能代劳：写命令、改代码、审查产物、分析结果。
