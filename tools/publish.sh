#!/usr/bin/env bash
# 把本地仓库发布到 GitHub。
#
# 用法:
#   ./tools/publish.sh --check              # 只体检，不改任何东西
#   ./tools/publish.sh --ssh                # SSH 方式（需先把公钥加到 GitHub 账号）
#   GH_TOKEN=ghp_xxx ./tools/publish.sh --token   # HTTPS + PAT，自动建仓库
#
# 环境变量:
#   GH_USER    GitHub 用户名，默认 keast666
#   GH_REPO    仓库名，默认 estudio2323am-cups
#   GH_TOKEN   Personal Access Token（--token 模式必填，需要 repo 权限）
set -euo pipefail

GH_USER="${GH_USER:-kesat666}"
GH_REPO="${GH_REPO:-estudio2323am-cups}"
BRANCH="${BRANCH:-main}"
API="https://api.github.com"

cd "$(dirname "$0")/.."

die() { printf '\n[FAIL] %s\n' "$1" >&2; exit 1; }
info() { printf '[ .. ] %s\n' "$1"; }
ok()   { printf '[ OK ] %s\n' "$1"; }

# 用 SSH 握手确认 GitHub 侧真实用户名（欢迎语里带的就是它）
ssh_user() {
    ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes -o ConnectTimeout=15 \
        -T git@github.com 2>&1 | sed -n 's/^Hi \([^!]*\)!.*/\1/p' | head -1
}

# ---------- 体检 ----------
check() {
    info "本地仓库"
    git rev-parse --git-dir >/dev/null 2>&1 || die "当前目录不是 git 仓库"
    local n
    n=$(git rev-list --count "HEAD" 2>/dev/null || echo 0)
    ok "提交数 $n，分支 $(git rev-parse --abbrev-ref HEAD)"
    local dirty
    dirty=$(git status --porcelain | wc -l | tr -d ' ')
    if [ "$dirty" != "0" ]; then
        printf '       注意: 有 %s 个未提交改动\n' "$dirty"
    fi

    info "提交身份"
    ok "$(git log -1 --format='%an <%ae>')"

    info "GitHub 账号 $GH_USER"
    local code
    code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 15 -A 'Mozilla/5.0' \
           "https://github.com/$GH_USER" 2>/dev/null || echo 000)
    case "$code" in
        200) ok "账号存在" ;;
        404) printf '       账号不存在（HTTP 404）。先在 github.com 注册或核对用户名。\n' ;;
        *)   printf '       无法确认（HTTP %s），可能是网络或代理问题。\n' "$code" ;;
    esac

    info "仓库 $GH_USER/$GH_REPO"
    code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 15 -A 'Mozilla/5.0' \
           "https://github.com/$GH_USER/$GH_REPO" 2>/dev/null || echo 000)
    case "$code" in
        200) ok "仓库已存在" ;;
        404) printf '       仓库尚不存在。先在 https://github.com/new 建同名空仓库（不要勾选 README / .gitignore）\n' ;;
        *)   printf '       无法确认（HTTP %s）\n' "$code" ;;
    esac

    info "SSH 密钥"
    if [ -f "$HOME/.ssh/id_ed25519.pub" ]; then
        ok "公钥已生成: $(cut -d' ' -f1,2 "$HOME/.ssh/id_ed25519.pub")"
        local fp
        fp=$(ssh-keygen -lf "$HOME/.ssh/id_ed25519.pub" 2>/dev/null | awk '{print $2}')
        printf '       指纹: %s\n' "$fp"
    else
        printf '       未生成。运行: ssh-keygen -t ed25519 -N "" -C "你的邮箱"\n'
    fi

    info "SSH 认证"
    local real
    real=$(ssh_user || true)
    if [ -n "$real" ]; then
        ok "GitHub 侧账号为 $real"
        if [ "$real" != "$GH_USER" ]; then
            printf '       注意: 与 GH_USER=%s 不一致，请用 GH_USER=%s 运行\n' "$GH_USER" "$real"
        fi
    else
        printf '       未通过。把公钥加到 GitHub → Settings → SSH and GPG keys\n'
    fi

    info "远端"
    if git remote get-url origin >/dev/null 2>&1; then
        ok "origin = $(git remote get-url origin)"
    else
        printf '       未设置\n'
    fi
}

# ---------- 建仓库 ----------
create_repo() {
    [ -n "${GH_TOKEN:-}" ] || die "缺少 GH_TOKEN"
    info "创建仓库 $GH_USER/$GH_REPO"
    local body resp
    body=$(printf '{"name":"%s","private":false,"has_issues":true,"has_wiki":false,"auto_init":false}' "$GH_REPO")
    resp=$(curl -sS --max-time 30 -X POST \
        -H "Authorization: Bearer $GH_TOKEN" \
        -H "Accept: application/vnd.github+json" \
        -d "$body" "$API/user/repos")

    case "$resp" in
        *'"full_name"'*) ok "仓库已创建" ;;
        *'already exists'*) ok "仓库已存在，跳过创建" ;;
        *) printf '%s\n' "$resp" | head -5; die "创建仓库失败（token 权限不足？需要 repo 作用域）" ;;
    esac
}

# ---------- 推送 ----------
push_https() {
    [ -n "${GH_TOKEN:-}" ] || die "缺少 GH_TOKEN"
    local auth
    auth=$(printf 'x-access-token:%s' "$GH_TOKEN" | base64 | tr -d '\n')
    info "推送到 $GH_USER/$GH_REPO"
    # token 只出现在这一次调用的参数里，不写进 .git/config
    git -c http.extraheader="Authorization: Basic $auth" \
        push "https://github.com/$GH_USER/$GH_REPO.git" "HEAD:refs/heads/$BRANCH"
    git remote remove origin 2>/dev/null || true
    git remote add origin "https://github.com/$GH_USER/$GH_REPO.git"
    ok "完成，origin 已设为干净 URL（不含 token）"
}

push_ssh() {
    info "测试 SSH 连通性"
    local out
    out=$(ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes -T git@github.com 2>&1 || true)
    case "$out" in
        *"successfully authenticated"*)
            ok "SSH 认证通过" ;;
        *)
            printf '%s\n' "$out" | head -3
            die "SSH 认证未通过。把 ~/.ssh/id_ed25519.pub 的内容加到
       GitHub → Settings → SSH and GPG keys → New SSH key
       然后重跑本脚本。" ;;
    esac

    git remote remove origin 2>/dev/null || true
    git remote add origin "git@github.com:$GH_USER/$GH_REPO.git"
    info "推送到 $GH_USER/$GH_REPO"
    git push -u origin "$BRANCH"
    ok "完成"
}

# ---------- 入口 ----------
case "${1:-}" in
    --check)  check ;;
    --ssh)    check >/dev/null; push_ssh ;;
    --token)  check >/dev/null; create_repo; push_https ;;
    *)        sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//' ;;
esac
