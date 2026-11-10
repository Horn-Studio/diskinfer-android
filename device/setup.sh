#!/system/bin/sh
# diskinfer device-side setup
#
#   sh setup.sh [path/to/disktui]
#
# works with or without root, and with either install layout:
#   flat   : /data/local/tmp/llm/{setup.sh,diskinfer,termfont,run_as_root.sh,disktui,...}
#   legacy : /data/local/tmp/llm/device/{...}        (the old buggy fab push, still accepted)
# it can also be started straight from a folder on /sdcard (non-root install path).
#
# what it does:
#   * installs disktui (+ diskrun / llama-completion when present) into ~/llm
#   * installs diskinfer / termfont into $PREFIX/bin so they are on PATH in Termux
#   * makes sure diskinfer / termfont / run_as_root.sh / setup.sh are flat under
#     /data/local/tmp/llm (only when we may write there: root or the adb shell user)
#   * root only: oom/PATH tweaks, magisk boot script, /debug_ramdisk symlinks.
#     every magisk step fails silently and prints "skipped"; non-root never touches /data/adb
#   * prints a self-check at the end (what landed where) plus model-directory hints
#
# messages: en / zh-Hans / zh-Hant, auto-detected (contract v5 §1.3):
#   DISKINFER_LANG > getprop persist.sys.locale > ro.product.locale > $LANG > $LC_ALL
#
# exit codes: 0 = installed, 1 = Termux missing (nothing user-visible installed),
#             2 = disktui binary not found.

set -u

# ---------------------------------------------------------------------------
# i18n (POSIX only: this runs under /system/bin/sh / mksh)
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
  # on android the phone's language is the product locale; Termux may export its
  # own LANG (en_US.UTF-8 by default) which has nothing to do with the device
  # setting, so the system properties win (same order as the TUI i18n.cpp)
  if command -v getprop >/dev/null 2>&1; then
    probe=$(getprop persist.sys.locale 2>/dev/null)
  elif [ -x /system/bin/getprop ]; then
    probe=$(/system/bin/getprop persist.sys.locale 2>/dev/null)
  fi
  if [ -z "$probe" ]; then
    if command -v getprop >/dev/null 2>&1; then
      probe=$(getprop ro.product.locale 2>/dev/null)
    elif [ -x /system/bin/getprop ]; then
      probe=$(/system/bin/getprop ro.product.locale 2>/dev/null)
    fi
  fi
  [ -z "$probe" ] && probe=${LANG:-}
  [ -z "$probe" ] && probe=${LC_ALL:-}
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

layout_label() {
  case "$1" in
    flat)    t 'flat' '扁平' '扁平' ;;
    legacy)  t 'legacy' '旧布局' '舊佈局' ;;
    local)   t 'local' '本地' '本地' ;;
    *)       t 'missing' '缺失' '缺失' ;;
  esac
}

msg() {
  case "$1" in
    setup_header)   printf "$(t 'diskinfer setup\n' 'diskinfer 安装\n' 'diskinfer 安裝\n')" ;;
    source_layout)  printf "$(t '  source layout : %s (%s)\n' '  脚本来源布局 : %s (%s)\n' '  腳本來源佈局 : %s (%s)\n')" "$2" "$3" ;;
    root_line)      printf "$(t '  root          : %s\n' '  root          : %s\n' '  root          : %s\n')" "$2" ;;
    yes)            t 'yes' '是' '是' ;;
    no)             t 'no' '否' '否' ;;
    binary_line)    printf "$(t '  binary        : %s\n' '  二进制        : %s\n' '  二進位        : %s\n')" "$2" ;;
    no_binary)      printf "$(t 'setup: disktui binary not found (looked in %s, %s, %s, %s)\n' 'setup: 没有找到 disktui 二进制（找过 %s, %s, %s, %s）\n' 'setup: 沒有找到 disktui 二進位（找過 %s, %s, %s, %s）\n')" "$2" "$3" "$4" "$5" ;;
    usage)          printf "$(t 'setup: usage: sh setup.sh /path/to/disktui\n' 'setup: 用法: sh setup.sh /path/to/disktui\n' 'setup: 用法: sh setup.sh /path/to/disktui\n')" ;;
    tmp_skipped)    printf "$(t '  [..]   no write access to %s (Termux user without root) - skipped the adb-side copy\n' '  [..]   对 %s 没有写权限（无 root 的 Termux 用户）- 跳过 adb 侧复制\n' '  [..]   對 %s 沒有寫入權限（無 root 的 Termux 使用者）- 略過 adb 端複製\n')" "$2" ;;
    bashrc_patched) printf "$(t '  [ok]   patched %s (PATH)\n' '  [ok]   已修补 %s（PATH）\n' '  [ok]   已修補 %s（PATH）\n')" "$2" ;;
    termux_missing1) printf "$(t '  [..]   Termux not ready (home=%s) - open the Termux app once so it unpacks its bootstrap,\n' '  [..]   Termux 未就绪（home=%s）- 先打开一次 Termux 让它解包 bootstrap，\n' '  [..]   Termux 未就緒（home=%s）- 先開啟一次 Termux 讓它解包 bootstrap，\n')" "$2" ;;
    termux_missing2) printf "$(t '  [..]   then run: system/bin/sh <sdcard>/Download/diskinfer/setup.sh <sdcard>/Download/diskinfer/disktui\n' '  [..]   然后运行: system/bin/sh <sdcard>/Download/diskinfer/setup.sh <sdcard>/Download/diskinfer/disktui\n' '  [..]   然後執行: system/bin/sh <sdcard>/Download/diskinfer/setup.sh <sdcard>/Download/diskinfer/disktui\n')" ;;
    magisk_installed) printf "$(t '  [ok]   magisk boot script -> /data/adb/service.d/99-disktui.sh\n' '  [ok]   magisk 开机脚本 -> /data/adb/service.d/99-disktui.sh\n' '  [ok]   magisk 開機腳本 -> /data/adb/service.d/99-disktui.sh\n')" ;;
    magisk_failed)  printf "$(t '  [..]   Magisk steps skipped (write failed)\n' '  [..]   Magisk 步骤已跳过（写入失败）\n' '  [..]   Magisk 步驟已略過（寫入失敗）\n')" ;;
    magisk_skipped) printf "$(t '  [..]   Magisk steps skipped (non-root, or no /data/adb)\n' '  [..]   Magisk 步骤已跳过（非 root 或没有 /data/adb）\n' '  [..]   Magisk 步驟已略過（非 root 或沒有 /data/adb）\n')" ;;
    selfcheck)      printf "$(t '== self check ==\n' '== 自检 ==\n' '== 自檢 ==\n')" ;;
    termux_bin_missing) printf "$(t '  [--]   Termux home / $PREFIX/bin (Termux not installed or not bootstrapped yet)\n' '  [--]   Termux home / $PREFIX/bin（Termux 未安装或还没初始化）\n' '  [--]   Termux home / $PREFIX/bin（Termux 未安裝或還沒初始化）\n')" ;;
    models)         printf "$(t '== models ==\n' '== 模型 ==\n' '== 模型 ==\n')" ;;
    root_model1)    printf "$(t '  adb push model.gguf /data/local/tmp/llm/models/\n' '  adb push model.gguf /data/local/tmp/llm/models/\n' '  adb push model.gguf /data/local/tmp/llm/models/\n')" ;;
    root_model2)    printf "$(t '  adb push model.gguf /storage/emulated/0/models/\n' '  adb push model.gguf /storage/emulated/0/models/\n' '  adb push model.gguf /storage/emulated/0/models/\n')" ;;
    root_model3)    printf "$(t '  (root launcher searches /data/local/tmp/llm/models first, then ~/models, then /storage)\n' '  （root 启动器依次搜索 /data/local/tmp/llm/models、~/models、/storage）\n' '  （root 啟動器依序搜尋 /data/local/tmp/llm/models、~/models、/storage）\n')" ;;
    user_model1)    printf "$(t '  non-root: put the model into the Termux private dir ~/models/ (always readable)\n' '  非 root：把模型放进 Termux 私有目录 ~/models/（永远可读）\n' '  非 root：把模型放進 Termux 私有目錄 ~/models/（永遠可讀）\n')" ;;
    user_model2)    printf "$(t '    cp /sdcard/Download/model.gguf ~/models/\n' '    cp /sdcard/Download/model.gguf ~/models/\n' '    cp /sdcard/Download/model.gguf ~/models/\n')" ;;
    user_model3)    printf "$(t '  or grant Termux storage permission and use the shared dir /storage/emulated/0/models/:\n' '  或者给 Termux 存储权限后用共享目录 /storage/emulated/0/models/：\n' '  或者給 Termux 儲存權限後用共享目錄 /storage/emulated/0/models/：\n')" ;;
    user_model4)    printf "$(t '    adb shell appops set com.termux MANAGE_EXTERNAL_STORAGE allow\n' '    adb shell appops set com.termux MANAGE_EXTERNAL_STORAGE allow\n' '    adb shell appops set com.termux MANAGE_EXTERNAL_STORAGE allow\n')" ;;
    user_model5)    printf "$(t '    adb shell pm grant com.termux android.permission.READ_EXTERNAL_STORAGE\n' '    adb shell pm grant com.termux android.permission.READ_EXTERNAL_STORAGE\n' '    adb shell pm grant com.termux android.permission.READ_EXTERNAL_STORAGE\n')" ;;
  esac
}

L10N=$(detect_lang)

H=/data/data/com.termux/files/home
PRE=/data/data/com.termux/files/usr
TMP=/data/local/tmp/llm
BINARG=${1:-}

SDIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || SDIR=.
IS_ROOT=0
[ "$(id -u)" = "0" ] && IS_ROOT=1

ok()   { printf '  [ok]   %s\n' "$1"; }
miss() { printf '  [--]   %s\n' "$1"; }
note() { printf '  [..]   %s\n' "$1"; }

# ---------------------------------------------------------------- source layout
# prefer the flat layout (the v4 installer), then the legacy device/ subdir,
# then whatever directory this script itself lives in (e.g. /sdcard/Download/diskinfer).
if [ -f "$TMP/setup.sh" ]; then
  SRC=$TMP
  LAYOUT=flat
elif [ -f "$TMP/device/setup.sh" ]; then
  SRC=$TMP/device
  LAYOUT=legacy
elif [ -f "$SDIR/setup.sh" ]; then
  SRC=$SDIR
  LAYOUT=local
else
  SRC=$TMP
  LAYOUT=missing
fi

msg setup_header
msg source_layout "$(layout_label "$LAYOUT")" "$SRC"
if [ "$IS_ROOT" = 1 ]; then ROOT_TXT=$(msg yes); else ROOT_TXT=$(msg no); fi
msg root_line "$ROOT_TXT"

# ---------------------------------------------------------------- disktui binary
BIN=""
if [ -n "$BINARG" ] && [ -f "$BINARG" ]; then
  BIN=$BINARG
else
  for c in "$SRC/disktui" "$TMP/disktui" "$SDIR/disktui" "$H/llm/disktui"; do
    [ -f "$c" ] && BIN=$c && break
  done
fi
if [ -z "$BIN" ]; then
  msg no_binary "$SRC" "$TMP" "$SDIR" "$H/llm" >&2
  msg usage >&2
  exit 2
fi
msg binary_line "$BIN"

# ---------------------------------------------------------------- /data/local/tmp
# the adb shell user owns /data/local/tmp, the Termux app user does not: check write
# access instead of the uid, so a root shell and a plain adb shell both work here.
TMP_OK=0
mkdir -p "$TMP" 2>/dev/null
[ -w "$TMP" ] && TMP_OK=1

if [ "$TMP_OK" = 1 ]; then
  # first install: copy the launcher scripts next to the binary, flat.
  for f in setup.sh diskinfer termfont run_as_root.sh 99-disktui.sh; do
    if [ -f "$SRC/$f" ] && [ "$SRC/$f" != "$TMP/$f" ]; then
      cp -f "$SRC/$f" "$TMP/$f" 2>/dev/null || true
    fi
    [ -f "$TMP/$f" ] && chmod 755 "$TMP/$f" 2>/dev/null
  done
  # upgrade a legacy device/ directory in place: the flat copies above win, and
  # the stale subdir is left alone (it may be someone else's dev folder).
  for f in disktui diskrun llama-completion; do
    if [ -f "$SRC/$f" ] && [ "$SRC/$f" != "$TMP/$f" ]; then
      cp -f "$SRC/$f" "$TMP/$f" 2>/dev/null && chmod 755 "$TMP/$f" 2>/dev/null
    fi
  done
  [ -f "$BIN" ] && [ "$BIN" != "$TMP/disktui" ] && \
    cp -f "$BIN" "$TMP/disktui" 2>/dev/null && chmod 755 "$TMP/disktui" 2>/dev/null
  [ -f "$TMP/disktui" ] && chmod 755 "$TMP/disktui" 2>/dev/null
else
  msg tmp_skipped "$TMP"
fi

# ---------------------------------------------------------------- Termux
TERMUX_OK=0
if [ -d "$H" ] && [ -x "$PRE/bin/bash" ]; then
  mkdir -p "$H/llm" "$H/models" 2>/dev/null
  if [ "$BIN" != "$H/llm/disktui" ]; then
    cp -f "$BIN" "$H/llm/disktui" 2>/dev/null || true
  fi
  chmod 755 "$H/llm/disktui" 2>/dev/null
  for f in diskrun llama-completion; do
    if [ -f "$SRC/$f" ]; then
      cp -f "$SRC/$f" "$H/llm/$f" 2>/dev/null && chmod 755 "$H/llm/$f" 2>/dev/null
    fi
  done
  for f in diskinfer termfont; do
    if [ -f "$SRC/$f" ]; then
      cp -f "$SRC/$f" "$PRE/bin/$f" 2>/dev/null && chmod 755 "$PRE/bin/$f" 2>/dev/null
    elif [ -f "$TMP/$f" ]; then
      cp -f "$TMP/$f" "$PRE/bin/$f" 2>/dev/null && chmod 755 "$PRE/bin/$f" 2>/dev/null
    fi
  done

  # make the termux tools reachable from root shells (magisk replaces PATH)
  RC=$H/.bashrc
  if ! grep -q "termux/files/usr/bin" "$RC" 2>/dev/null; then
    {
      echo ''
      echo '# make the termux tools visible from root shells (magisk replaces PATH)'
      echo 'case ":$PATH:" in'
      echo '  *":/data/data/com.termux/files/usr/bin:"*) ;;'
      echo '  *) export PATH=/data/data/com.termux/files/usr/bin:$PATH ;;'
      echo 'esac'
    } >> "$RC" 2>/dev/null && msg bashrc_patched "$RC"
  fi
  TERMUX_OK=1
else
  msg termux_missing1 "$H"
  msg termux_missing2
fi

# ---------------------------------------------------------------- runtime files
# log dir + model dir under /data/local/tmp (root launcher writes there)
if [ "$TMP_OK" = 1 ]; then
  mkdir -p "$TMP/models" 2>/dev/null
fi

# ---------------------------------------------------------------- magisk (root only)
# all of this is best effort: no /data/adb, no magisk, no symlink target -> silently skipped.
MAGISK=skipped
if [ "$IS_ROOT" = 1 ] && [ -d /data/adb ]; then
  if [ -f "$SRC/99-disktui.sh" ] && mkdir -p /data/adb/service.d 2>/dev/null && \
     cp -f "$SRC/99-disktui.sh" /data/adb/service.d/99-disktui.sh 2>/dev/null && \
     chmod 755 /data/adb/service.d/99-disktui.sh 2>/dev/null; then
    sh /data/adb/service.d/99-disktui.sh >/dev/null 2>&1 || true
    MAGISK=installed
  else
    MAGISK=failed
  fi
  for d in /debug_ramdisk /sbin; do
    [ -d "$d" ] || continue
    for t in diskinfer termfont; do
      [ -x "$PRE/bin/$t" ] && ln -sf "$PRE/bin/$t" "$d/$t" 2>/dev/null && echo "  [ok]   $d/$t -> $PRE/bin/$t"
    done
  done
fi
case "$MAGISK" in
  installed) msg magisk_installed ;;
  failed)    msg magisk_failed ;;
  *)         msg magisk_skipped ;;
esac

# ---------------------------------------------------------------- ownership (root)
if [ "$IS_ROOT" = 1 ] && [ "$TERMUX_OK" = 1 ]; then
  OWNER=$(ls -ld "$H" 2>/dev/null | sed -n 's/^[^ ]* [^ ]* \([^ ]*\).*/\1/p')
  if [ -n "${OWNER:-}" ]; then
    chown -R "$OWNER" "$H/llm" 2>/dev/null || true
    chown "$OWNER" "$PRE/bin/diskinfer" "$PRE/bin/termfont" 2>/dev/null || true
  fi
fi

# ---------------------------------------------------------------- self check
printf '\n'
msg selfcheck
BAD=0
for f in "$TMP/disktui" "$TMP/setup.sh" "$TMP/diskinfer" "$TMP/termfont" "$TMP/run_as_root.sh"; do
  if [ -f "$f" ]; then ok "$f"; else miss "$f"; [ "$f" = "$TMP/disktui" ] && [ "$TMP_OK" = 0 ] || BAD=$((BAD + 1)); fi
done
if [ "$TERMUX_OK" = 1 ]; then
  for f in "$H/llm/disktui" "$PRE/bin/diskinfer" "$PRE/bin/termfont"; do
    if [ -f "$f" ]; then ok "$f"; else miss "$f"; BAD=$((BAD + 1)); fi
  done
else
  msg termux_bin_missing
fi

printf '\n'
msg models
if [ "$IS_ROOT" = 1 ]; then
  msg root_model1
  msg root_model2
  msg root_model3
else
  msg user_model1
  msg user_model2
  msg user_model3
  msg user_model4
  msg user_model5
fi

if [ "$TERMUX_OK" = 0 ] && [ "$IS_ROOT" = 0 ]; then
  exit 1
fi
[ "$BAD" -gt 0 ] && exit 1
exit 0
