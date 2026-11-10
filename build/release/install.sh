#!/bin/sh
# diskinfer android installer - one script, two worlds:
#
#   Termux (phone):   sh install.sh
#       installs directly into $PREFIX/bin + ~/llm (and /data/local/tmp when rooted)
#
#   computer:         bash install.sh          (Git Bash / Linux / macOS)
#       asks where adb is, finds the phone, pushes the binaries and the flat
#       launcher scripts to /data/local/tmp/llm, installs through su when the phone
#       is rooted, otherwise stages everything on /sdcard/Download/diskinfer and
#       prints the one line to run inside Termux.
#
# no dependency except adb (PC mode). POSIX sh: no bashisms, safe on dash.
#
# layout it accepts: scripts next to install.sh (flat, the release zip) or in ./device/
# (the old v3 zip / a repo checkout). it always pushes the scripts flat to
# /data/local/tmp/llm so run_as_root.sh is where device/diskinfer expects it.
#
# messages: en / zh-Hans / zh-Hant, auto-detected (contract v5 §1.3):
#   --lang CLI > DISKINFER_LANG > system locale. android: persist.sys.locale >
#   ro.product.locale > $LANG > $LC_ALL (Termux exports an unrelated LANG);
#   desktop: $LC_ALL > $LC_MESSAGES > $LANG > /etc/default/locale.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
TMP=/data/local/tmp/llm
SDCARD=/sdcard/Download/diskinfer
VERSION=1.0.1
SERIAL=""
ADB=""

# ---------------------------------------------------------------------------
# i18n
# ---------------------------------------------------------------------------
lang_from_tag() {
  tag=$(printf '%s' "${1:-}" | tr '[:upper:]' '[:lower:]')
  case "$tag" in
    *hant*|*tw*|*hk*|*mo*) printf '%s' zh-hant ;;
    zh*)                   printf '%s' zh-hans ;;
    *)                     printf '%s' en ;;
  esac
}

probe_system_lang() {
  probe=""
  getprop_cmd=""
  if command -v getprop >/dev/null 2>&1; then
    getprop_cmd=getprop
  elif [ -x /system/bin/getprop ]; then
    getprop_cmd=/system/bin/getprop
  fi
  if [ -n "$getprop_cmd" ]; then
    # android: the product locale wins over Termux's own LANG (the TUI does the same)
    probe=$("$getprop_cmd" persist.sys.locale 2>/dev/null)
    if [ -z "$probe" ]; then
      probe=$("$getprop_cmd" ro.product.locale 2>/dev/null)
    fi
    [ -z "$probe" ] && probe=${LANG:-}
    [ -z "$probe" ] && probe=${LC_ALL:-}
  else
    # desktop: XDG-ish order, then /etc/default/locale
    probe=${LC_ALL:-}
    [ -z "$probe" ] && probe=${LC_MESSAGES:-}
    [ -z "$probe" ] && probe=${LANG:-}
    if [ -z "$probe" ] && [ -r /etc/default/locale ]; then
      probe=$(grep '^LANG=' /etc/default/locale 2>/dev/null | head -1 | sed 's/^LANG=//; s/^"//; s/"$//')
    fi
  fi
  printf '%s' "$probe"
}

detect_lang() {
  explicit=${1:-}
  if [ -z "$explicit" ] && [ -n "${DISKINFER_LANG:-}" ]; then
    explicit=$DISKINFER_LANG
  fi
  if [ -z "$explicit" ]; then
    explicit=$(probe_system_lang)
  fi
  lang_from_tag "$explicit"
}

# t <en> <zh-Hans> <zh-Hant>
t() {
  case "$L10N" in
    zh-hans) printf '%s' "$2" ;;
    zh-hant) printf '%s' "$3" ;;
    *)       printf '%s' "$1" ;;
  esac
}

# ------------------------------------------------------------------ global args
parse_global_args() {
  global_help=0
  global_lang=""
  prev=""
  for a in "$@"; do
    case "$prev" in
      --lang) global_lang=$a; prev=""; continue ;;
    esac
    case "$a" in
      --lang)   prev=--lang ;;
      --lang=*) global_lang=${a#*=} ;;
      -h|--help) global_help=1 ;;
    esac
    prev=""
  done
}

say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s ==\n' "$*"; }
die()  { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------ layout
if [ -f "$HERE/setup.sh" ]; then
  SRC=$HERE
elif [ -f "$HERE/device/setup.sh" ]; then
  SRC=$HERE/device
else
  SRC=""
fi

# ------------------------------------------------------------------ helpers
# paths have to survive: quoting, windows drag&drop with quotes, powershell "& '...'",
# escaped spaces from a shell drag, a directory instead of the exe, no ".exe", PATH.
towin() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

# adb wrapper: never let msys rewrite the remote /data or /sdcard paths
adb_do() {
  MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$ADB" -s "$SERIAL" "$@"
}

qf() { printf "%s" "$1" | sed "s/'/'\\\\''/g"; }

strip_quotes() {
  s=$1
  while :; do
    case "$s" in
      \"*\"|\'*\') s=${s#?}; s=${s%?} ;;
      *) break ;;
    esac
  done
  printf '%s' "$s"
}

# decode what a terminal hands us for a path:
#   "C:\Program Files\...\adb.exe"      (powershell / cmd drag)
#   C:\Program\ Files\...\adb.exe       (git-bash drag with escaped spaces)
#   & 'C:\...\adb.exe'                  (powershell "copy as path" + call operator)
#   /c/Users/.../adb.exe                (typed in git bash)
#   adb                                 (PATH)
normalize_adb() {
  raw=$1
  raw=$(printf '%s' "$raw" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
  # drop the powershell call operator / cmd "start" wrapper first: '& "C:\...\adb.exe"'
  raw=$(printf '%s' "$raw" | sed 's/^&[[:space:]]*//; s/^start[[:space:]]*"\{0,1\}//')
  # strip surrounding quotes (loop: the wrapper may have hidden one quote layer)
  while :; do
    bare=$(strip_quotes "$raw")
    [ "$bare" = "$raw" ] && break
    raw=$bare
  done
  raw=$(printf '%s' "$raw" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
  # git-bash escaped spaces: "C:\Program\ Files\x" -> "C:\Program Files\x"
  case "$raw" in
    *'\\ '*) raw=$(printf '%s' "$raw" | sed 's/\\\\ / /g') ;;
    *'\ '*)  raw=$(printf '%s' "$raw" | sed 's/\\ / /g') ;;
  esac
  # windows absolute path -> msys/posix path (git bash cannot exec C:\... reliably)
  case "$raw" in
    [A-Za-z]:[\\/]*)
      if command -v cygpath >/dev/null 2>&1; then
        raw=$(cygpath -u "$raw")
      fi
      ;;
  esac
  # a directory: use adb.exe/adb inside it
  if [ -d "$raw" ]; then
    if [ -f "$raw/adb.exe" ]; then raw=$raw/adb.exe
    elif [ -f "$raw/adb" ]; then raw=$raw/adb
    fi
  fi
  if [ ! -e "$raw" ] && [ -e "$raw.exe" ]; then
    raw=$raw.exe
  fi
  printf '%s' "$raw"
}

ask_adb() {
  say "$(t "diskinfer $VERSION installer (adb mode)" "diskinfer $VERSION 安装器（adb 模式）" "diskinfer $VERSION 安裝器（adb 模式）")"
  say "$(t 'tip: drag adb.exe into this window (quotes are fine); press Enter to use the adb on PATH.' '提示：可以把 adb.exe 直接拖进本窗口（带引号也行），直接回车则使用 PATH 里的 adb。' '提示：可以把 adb.exe 直接拖進本視窗（帶引號也行），直接按 Enter 則使用 PATH 裡的 adb。')"
  printf '%s' "$(t 'adb path: ' 'adb 路径: ' 'adb 路徑: ')"
  if IFS= read -r input; then :; else input=""; fi
  if [ -z "$input" ]; then
    ADB=adb
  else
    ADB=$(normalize_adb "$input")
  fi
  if [ -n "$ADB" ] && [ "$ADB" != adb ] && [ ! -f "$ADB" ]; then
    die "$(t "cannot find adb: $ADB (drag the full path of adb.exe in, or add it to PATH)" "找不到 adb: $ADB（把 adb.exe 的完整路径拖进来，或者把它加进 PATH）" "找不到 adb: $ADB（把 adb.exe 的完整路徑拖進來，或者把它加進 PATH）")"
  fi
  if [ "$ADB" = adb ] && ! command -v adb >/dev/null 2>&1; then
    die "$(t 'there is no adb on PATH; re-run and drag the adb.exe path in' 'PATH 里没有 adb；请重新运行并把 adb.exe 的路径拖进来' 'PATH 裡沒有 adb；請重新執行並把 adb.exe 的路徑拖進來')"
  fi
  say "$(t "using: $ADB" "使用: $ADB" "使用: $ADB")"
}

# ------------------------------------------------------------------ Termux mode
in_termux() {
  if [ -n "${PREFIX:-}" ] && [ -x "${PREFIX:-}/bin/bash" ]; then return 0; fi
  if [ -x /data/data/com.termux/files/usr/bin/bash ] && [ -d /data/data/com.termux/files/home ]; then
    return 0
  fi
  return 1
}

msg_termux_done() {
  case "$L10N" in
    zh-hans)
      say '模型放这里：~/models/  或（授了存储权限后）/storage/emulated/0/models/'
      say '  例：cp /sdcard/Download/model.gguf ~/models/'
      say '然后运行：'
      say '  diskinfer                  # 打开 TUI'
      say '  diskinfer -m ~/models/x.gguf'
      say '  diskinfer --api-only -m ~/models/x.gguf   # /v1 API，端口 4000'
      say '（新开的 Termux 窗口里 diskinfer 才在 PATH 上）'
      ;;
    zh-hant)
      say '模型放這裡：~/models/  或（授了儲存權限後）/storage/emulated/0/models/'
      say '  例：cp /sdcard/Download/model.gguf ~/models/'
      say '然後執行：'
      say '  diskinfer                  # 開啟 TUI'
      say '  diskinfer -m ~/models/x.gguf'
      say '  diskinfer --api-only -m ~/models/x.gguf   # /v1 API，連接埠 4000'
      say '（新開的 Termux 視窗裡 diskinfer 才在 PATH 上）'
      ;;
    *)
      say 'put the model here: ~/models/  or (after granting storage) /storage/emulated/0/models/'
      say '  e.g. cp /sdcard/Download/model.gguf ~/models/'
      say 'then run:'
      say '  diskinfer                  # open the TUI'
      say '  diskinfer -m ~/models/x.gguf'
      say '  diskinfer --api-only -m ~/models/x.gguf   # /v1 API on port 4000'
      say '(open a new Termux window so diskinfer is on PATH)'
      ;;
  esac
}

termux_install() {
  [ -n "$SRC" ] || die "$(t 'cannot find the device/ scripts (setup.sh); run install.sh from the extracted release dir' '没有找到 device/ 脚本（setup.sh）；请在解压后的发布包目录里运行 install.sh' '沒有找到 device/ 腳本（setup.sh）；請在解壓後的發佈包目錄裡執行 install.sh')"
  BIN=""
  for c in "$HERE/disktui" "$SRC/disktui"; do
    [ -f "$c" ] && BIN=$c && break
  done
  [ -n "$BIN" ] || die "$(t 'cannot find disktui (run install.sh from the release dir, or unzip the release first)' '没有找到 disktui（请从发布包目录里运行，或者先解压 zip）' '沒有找到 disktui（請從發佈包目錄裡執行，或者先解壓 zip）')"

  say "$(t "diskinfer $VERSION installer (Termux mode)" "diskinfer $VERSION 安装器（Termux 模式）" "diskinfer $VERSION 安裝器（Termux 模式）")"
  say "$(t "  scripts: $SRC" "  脚本  : $SRC" "  腳本  : $SRC")"
  say "$(t "  binary : $BIN" "  二进制: $BIN" "  二進位: $BIN")"

  if [ "$(id -u)" = "0" ]; then
    step "$(t 'root: installing into $PREFIX/bin, ~/llm and /data/local/tmp/llm' 'root: 安装到 $PREFIX/bin、~/llm 和 /data/local/tmp/llm' 'root: 安裝到 $PREFIX/bin、~/llm 和 /data/local/tmp/llm')"
    sh "$SRC/setup.sh" "$BIN"
  elif su -c true >/dev/null 2>&1; then
    step "$(t 'installing as root through su (Termux user + su)' '通过 su 以 root 安装（Termux 用户 + su）' '透過 su 以 root 安裝（Termux 使用者 + su）')"
    su -c "sh '$(qf "$SRC/setup.sh")' '$(qf "$BIN")'"
  else
    step "$(t 'installing as the plain Termux user (no root: no oom tweak / no mlock)' '普通 Termux 用户安装（无 root：不设 oom / 不强制 mlock）' '一般 Termux 使用者安裝（無 root：不設 oom / 不強制 mlock）')"
    sh "$SRC/setup.sh" "$BIN"
  fi

  step "$(t 'done' '完成' '完成')"
  msg_termux_done
}

# ------------------------------------------------------------------ PC mode
pick_device() {
  out=$("$ADB" devices 2>&1 | tr -d '\r')
  if [ -n "${ANDROID_SERIAL:-}" ]; then
    SERIAL=$ANDROID_SERIAL
    say "$(t "using ANDROID_SERIAL=$SERIAL" "使用 ANDROID_SERIAL=$SERIAL" "使用 ANDROID_SERIAL=$SERIAL")"
    return 0
  fi
  SERIAL=$(printf '%s\n' "$out" | sed -n 's/^\([^[:space:]]\{1,\}\)[[:space:]][[:space:]]*device$/\1/p' | head -1)
  if [ -z "$SERIAL" ]; then
    say "$out" >&2
    return 1
  fi
  return 0
}

push_file() { # push_file <local> <remote-dir>
  if ! adb_do push "$(towin "$1")" "$2" >/dev/null 2>&1; then
    die "$(t "adb push failed: $1 -> $2" "adb push 失败: $1 -> $2" "adb push 失敗: $1 -> $2")"
  fi
}

msg_root_done() {
  case "$L10N" in
    zh-hans)
      say '在手机上打开 Termux，运行：'
      say '  diskinfer                  # 打开 TUI（新窗口才在 PATH 上）'
      say '  diskinfer -m /storage/emulated/0/models/x.gguf'
      say '模型目录：/data/local/tmp/llm/models/ 或 /storage/emulated/0/models/'
      ;;
    zh-hant)
      say '在手機上開啟 Termux，執行：'
      say '  diskinfer                  # 開啟 TUI（新視窗才在 PATH 上）'
      say '  diskinfer -m /storage/emulated/0/models/x.gguf'
      say '模型目錄：/data/local/tmp/llm/models/ 或 /storage/emulated/0/models/'
      ;;
    *)
      say 'open Termux on the phone and run:'
      say '  diskinfer                  # open the TUI (new window to get it on PATH)'
      say '  diskinfer -m /storage/emulated/0/models/x.gguf'
      say 'model dir: /data/local/tmp/llm/models/ or /storage/emulated/0/models/'
      ;;
  esac
}

msg_nonroot_steps() {
  case "$L10N" in
    zh-hans)
      say '1) 打开 Termux，先给它存储权限（Android 11+ 必须，二选一）：'
      say '     Termux 里执行： termux-setup-storage        （弹窗点允许）'
      say '   或者电脑上执行：'
      say "     \"$(towin "$ADB")\" -s $SERIAL shell appops set com.termux MANAGE_EXTERNAL_STORAGE allow"
      say "     \"$(towin "$ADB")\" -s $SERIAL shell pm grant com.termux android.permission.READ_EXTERNAL_STORAGE"
      say ''
      say '2) Termux 里执行这一行（安装 diskinfer 命令）：'
      say "     sh $SDCARD/setup.sh $SDCARD/disktui"
      say ''
      say '装完后在 Termux 里：'
      say '  diskinfer                              # 打开 TUI'
      say '  cp /sdcard/Download/x.gguf ~/models/   # 模型放 Termux 私有目录，永远可读'
      say '  或者授权后 adb push x.gguf /storage/emulated/0/models/'
      say ''
      say "提示：$TMP 下已经放好 disktui/diskrun/llama-completion 和扁平脚本，"
      say "      adb shell 里可以直接跑 $TMP/llama-completion（Termux 读不到这里，所以还要上面那一步）。"
      ;;
    zh-hant)
      say '1) 開啟 Termux，先給它儲存權限（Android 11+ 必須，二選一）：'
      say '     Termux 裡執行： termux-setup-storage        （彈窗點允許）'
      say '   或者在電腦上執行：'
      say "     \"$(towin "$ADB")\" -s $SERIAL shell appops set com.termux MANAGE_EXTERNAL_STORAGE allow"
      say "     \"$(towin "$ADB")\" -s $SERIAL shell pm grant com.termux android.permission.READ_EXTERNAL_STORAGE"
      say ''
      say '2) Termux 裡執行這一行（安裝 diskinfer 指令）：'
      say "     sh $SDCARD/setup.sh $SDCARD/disktui"
      say ''
      say '裝完後在 Termux 裡：'
      say '  diskinfer                              # 開啟 TUI'
      say '  cp /sdcard/Download/x.gguf ~/models/   # 模型放 Termux 私有目錄，永遠可讀'
      say '  或者授權後 adb push x.gguf /storage/emulated/0/models/'
      say ''
      say "提示：$TMP 下已經放好 disktui/diskrun/llama-completion 和扁平腳本，"
      say "      adb shell 裡可以直接跑 $TMP/llama-completion（Termux 讀不到這裡，所以還需要上面那一步）。"
      ;;
    *)
      say '1) open Termux and grant it storage access first (required on Android 11+, pick one):'
      say '     in Termux: termux-setup-storage        (tap Allow in the dialog)'
      say '   or from the computer:'
      say "     \"$(towin "$ADB")\" -s $SERIAL shell appops set com.termux MANAGE_EXTERNAL_STORAGE allow"
      say "     \"$(towin "$ADB")\" -s $SERIAL shell pm grant com.termux android.permission.READ_EXTERNAL_STORAGE"
      say ''
      say '2) run this one line in Termux (installs the diskinfer command):'
      say "     sh $SDCARD/setup.sh $SDCARD/disktui"
      say ''
      say 'then, in Termux:'
      say '  diskinfer                              # open the TUI'
      say '  cp /sdcard/Download/x.gguf ~/models/   # model -> Termux private dir, always readable'
      say '  or, after granting access: adb push x.gguf /storage/emulated/0/models/'
      say ''
      say "note: $TMP already has disktui/diskrun/llama-completion and the flat scripts,"
      say "      adb shell can run $TMP/llama-completion directly (Termux cannot read there, hence step 2)."
      ;;
  esac
}

pc_install() {
  [ -n "$SRC" ] || die "$(t 'cannot find the device/ scripts (setup.sh); run install.sh from the extracted release dir' '没有找到 device/ 脚本（setup.sh）；请在解压后的发布包目录里运行 install.sh' '沒有找到 device/ 腳本（setup.sh）；請在解壓後的發佈包目錄裡執行 install.sh')"
  [ -f "$HERE/disktui" ] || die "$(t 'cannot find disktui; run install.sh from the extracted release dir' '没有找到 disktui；请在解压后的发布包目录里运行 install.sh' '沒有找到 disktui；請在解壓後的發佈包目錄裡執行 install.sh')"
  ask_adb

  step "$(t 'finding the device' '查找设备' '尋找裝置')"
  "$ADB" start-server >/dev/null 2>&1 || true
  if ! pick_device; then
    die "$(t 'no device detected. Enable USB debugging (or adb connect 127.0.0.1:5555), until adb devices lists it as device, then retry.' '没有检测到设备。请打开 USB 调试（或无线调试 adb connect 127.0.0.1:5555），确认 adb devices 显示 device 后再试。' '沒有偵測到裝置。請開啟 USB 偵錯（或無線偵錯 adb connect 127.0.0.1:5555），確認 adb devices 顯示 device 後再試。')"
  fi
  say "$(t "device: $SERIAL" "设备: $SERIAL" "裝置: $SERIAL")"
  say "$(t "Android SDK $(adb_do shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r') / $(adb_do shell getprop ro.product.cpu.abilist 2>/dev/null | tr -d '\r')" "Android SDK $(adb_do shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r') / $(adb_do shell getprop ro.product.cpu.abilist 2>/dev/null | tr -d '\r')" "Android SDK $(adb_do shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r') / $(adb_do shell getprop ro.product.cpu.abilist 2>/dev/null | tr -d '\r')")"

  ROOT=0
  if adb_do shell 'su -c id' 2>/dev/null | tr -d '\r' | grep -q 'uid=0'; then
    ROOT=1
  fi
  if [ "$ROOT" = 1 ]; then
    say "$(t 'root: yes (install to /data/local/tmp/llm and run setup.sh directly)' 'root: 有（安装到 /data/local/tmp/llm 并直接执行 setup.sh）' 'root: 有（安裝到 /data/local/tmp/llm 並直接執行 setup.sh）')"
  else
    say "$(t 'root: no (after the push, run setup.sh once in Termux)' 'root: 无（推送后在 Termux 里执行一次 setup.sh）' 'root: 無（推送後在 Termux 裡執行一次 setup.sh）')"
  fi

  step "$(t "pushing binaries to $TMP" "推送二进制到 $TMP" "推送二進位到 $TMP")"
  adb_do shell "mkdir -p $TMP" || die "$(t "cannot create $TMP" "无法创建 $TMP" "無法建立 $TMP")"
  adb_do shell "rm -f $TMP/disktui.tmp" >/dev/null 2>&1 || true
  for f in disktui diskrun llama-completion; do
    if [ -f "$HERE/$f" ]; then
      push_file "$HERE/$f" "$TMP/"
      say "  $f"
    fi
  done
  [ -f "$HERE/disktui" ] || die "$(t 'disktui is missing' '缺少 disktui' '缺少 disktui')"

  # the fix for the reported bug: push the *contents* of device/ flat, never device/
  # itself. explicit list so a flat release package doesn't push install.sh/README too.
  step "$(t 'pushing launcher scripts (flat layout)' '推送启动器脚本（扁平布局）' '推送啟動器腳本（扁平佈局）')"
  for f in setup.sh diskinfer termfont run_as_root.sh 99-disktui.sh; do
    if [ -f "$SRC/$f" ]; then
      push_file "$SRC/$f" "$TMP/"
      say "  $f"
    fi
  done
  [ -f "$SRC/setup.sh" ] || die "$(t 'setup.sh is missing' '缺少 setup.sh' '缺少 setup.sh')"
  adb_do shell "chmod 755 $TMP/disktui $TMP/diskrun $TMP/llama-completion $TMP/setup.sh $TMP/diskinfer $TMP/termfont $TMP/run_as_root.sh 2>/dev/null; true"

  if [ "$ROOT" = 1 ]; then
    step "$(t 'installing as root (setup.sh)' '以 root 安装（setup.sh）' '以 root 安裝（setup.sh）')"
    adb_do shell "su -c 'sh $TMP/setup.sh $TMP/disktui'"
    say ""
    adb_do shell "ls -l $TMP" | tr -d '\r'
    step "$(t 'done' '完成' '完成')"
    msg_root_done
  else
    step "$(t "no root: staging the package into the Termux-readable $SDCARD" "无 root：把安装包放到 Termux 可读的 $SDCARD" "無 root：把安裝包放到 Termux 可讀的 $SDCARD")"
    adb_do shell "mkdir -p $SDCARD" || die "$(t "cannot create $SDCARD" "无法创建 $SDCARD" "無法建立 $SDCARD")"
    for f in disktui diskrun llama-completion; do
      [ -f "$HERE/$f" ] && push_file "$HERE/$f" "$SDCARD/"
    done
    for f in setup.sh diskinfer termfont run_as_root.sh 99-disktui.sh; do
      [ -f "$SRC/$f" ] && push_file "$SRC/$f" "$SDCARD/"
    done
    adb_do shell "chmod 755 $SDCARD/* 2>/dev/null; true"
    say ""
    adb_do shell "ls -l $TMP $SDCARD" | tr -d '\r'

    TERMUX_INSTALLED=0
    adb_do shell 'pm list packages com.termux' 2>/dev/null | grep -q 'package:com.termux' && TERMUX_INSTALLED=1

    step "$(t 'install it on the phone (2 steps)' '在手机上装（2 步）' '在手機上裝（2 步）')"
    if [ "$TERMUX_INSTALLED" = 0 ]; then
      say "$(t '0) install Termux first (F-Droid build): https://f-droid.org/packages/com.termux/' '0) 先安装 Termux（F-Droid 版）：https://f-droid.org/packages/com.termux/' '0) 先安裝 Termux（F-Droid 版）：https://f-droid.org/packages/com.termux/')"
    fi
    msg_nonroot_steps
  fi
}

# ------------------------------------------------------------------ usage
usage() {
  case "$L10N" in
    zh-hans)
      cat <<EOF
用法: sh install.sh [-h|--help] [--lang en|zh-Hans|zh-Hant|auto]

  在 Termux 里运行 -> 把 diskinfer/termfont 装到 \$PREFIX/bin、disktui 装到 ~/llm
  在电脑上运行   -> 先问 adb 在哪，再把当前连接的手机装好
                   （有 root：通过 su/setup.sh；无 root：放到 /sdcard/Download/diskinfer）

语言: --lang TAG 或环境变量 DISKINFER_LANG 覆盖系统语言；
      TAG 支持 auto / en / zh / zh-Hans / zh-Hant / zh-CN / zh-TW。

release: $VERSION   files: $( [ -n "$SRC" ] && echo "$SRC" || echo "missing device scripts" )
EOF
      ;;
    zh-hant)
      cat <<EOF
用法: sh install.sh [-h|--help] [--lang en|zh-Hans|zh-Hant|auto]

  在 Termux 裡執行 -> 把 diskinfer/termfont 裝到 \$PREFIX/bin、disktui 裝到 ~/llm
  在電腦上執行   -> 先問 adb 在哪，再把目前連線的手機裝好
                   （有 root：透過 su/setup.sh；無 root：放到 /sdcard/Download/diskinfer）

語言: --lang TAG 或環境變數 DISKINFER_LANG 覆蓋系統語言；
      TAG 支援 auto / en / zh / zh-Hans / zh-Hant / zh-CN / zh-TW。

release: $VERSION   files: $( [ -n "$SRC" ] && echo "$SRC" || echo "missing device scripts" )
EOF
      ;;
    *)
      cat <<EOF
usage: sh install.sh [-h|--help] [--lang en|zh-Hans|zh-Hant|auto]

  in Termux  -> installs diskinfer/termfont to \$PREFIX/bin and disktui to ~/llm
  on a PC    -> asks for the adb path, then installs on the connected phone
                (root: through su/setup.sh, non-root: staged in /sdcard/Download/diskinfer)

language: --lang TAG or the DISKINFER_LANG env var overrides the system locale;
          TAG accepts auto / en / zh / zh-Hans / zh-Hant / zh-CN / zh-TW.

release: $VERSION   files: $( [ -n "$SRC" ] && echo "$SRC" || echo "missing device scripts" )
EOF
      ;;
  esac
}

# ------------------------------------------------------------------ main
parse_global_args "$@"
L10N=$(detect_lang "$global_lang")
export DISKINFER_LANG=$L10N

if [ "$global_help" = 1 ]; then
  usage
  exit 0
fi

if in_termux; then
  termux_install
else
  pc_install
fi
