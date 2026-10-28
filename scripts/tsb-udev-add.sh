#!/bin/sh
# tsb-udev-add.sh — USB 插入后纠正被自动配错的 PPD
#
# 谁调用它：/lib/udev/rules.d/71-toshiba-estudio2323am.rules
# udev 的 RUN 不能长时间运行，所以规则里套了一层
# `systemd-run --no-block --collect`，这个脚本在独立的 transient unit 里跑。
#
# 为什么需要它：
#   Debian/Ubuntu 系（麒麟、UOS 都属于）在打印机热插拔时由
#   udev-configure-printer 自动建队列，它按 IEEE-1284 device-id 在自己能看到的
#   PPD 里挑一个。装了 openprinting 包的机器上有
#   "TOSHIBA e-ST280/282 Series PS"，在 e-STUDIO 系列上经常被挑中；可 2323AM /
#   2823AM 是 GDI 机型，没有 PostScript 解释器，用那个驱动只会打出乱码。
#
# 做法：等系统把队列建出来（最多 20 秒），只要队列的 device-uri 指向本机型、
# 而 PPD 不是本驱动，就用 lpadmin -m 换掉。已经是本驱动的队列一律不动，
# 所以不会反复重置用户设置。任何一步失败都静默退出——插拔打印机不该弹错误。

MODEL_PPD=toshiba-estudio2323am.ppd
MODEL_FILE=/usr/share/cups/model/toshiba-estudio2323am.ppd
MARK=TSB2323A

i=0
while [ "$i" -lt 20 ]; do
    i=$((i + 1))
    sleep 1

    found=0
    for f in /etc/cups/ppd/*.ppd; do
        [ -f "$f" ] || continue
        p=${f##*/}
        p=${p%.ppd}

        uri=$(LC_ALL=C lpstat -v "$p" 2>/dev/null | sed -n 's/^device for [^:]*: //p')
        case "$uri" in
            *2323AM*|*2823AM*) ;;
            *) continue ;;
        esac

        found=1
        grep -q "$MARK" "$f" 2>/dev/null && continue
        # -P 给文件而不是 -m 给驱动名：见 debian/postinst 里的说明，
        # cupsd 的 ppd_name 可能和磁盘上的 PPD 脱节，-m 会静默无效。
        if [ -f "$MODEL_FILE" ]; then
            lpadmin -p "$p" -P "$MODEL_FILE" >/dev/null 2>&1 || true
        else
            lpadmin -p "$p" -m "$MODEL_PPD" >/dev/null 2>&1 || true
        fi
    done

    # 队列已经出现（不管是对的还是要纠正的），处理完就收工
    [ "$found" -eq 1 ] && exit 0
done

exit 0
