#!/system/bin/sh
# diskinfer launcher (runs as root)
#
#   run_as_root.sh <model.gguf> [--arena MiB] [engine args...]
#
# --arena N : how much RAM the weight streaming arena may use, clamped to what this
#             device can actually spare. bigger = fewer re-reads, and if it can hold the
#             whole model the run is as fast as a normal in-RAM inference.
#
# POSIX /system/bin/sh (mksh) only: no arrays, no bashisms. the remaining engine
# arguments are passed through as positional parameters, so quoting/whitespace in
# model paths survives.
#
# messages: en / zh-Hans / zh-Hant, auto-detected (contract v5 §1.3):
#   DISKINFER_LANG > getprop persist.sys.locale > ro.product.locale > $LANG > $LC_ALL

# ---------------------------------------------------------------------------
# i18n (POSIX only: no arrays, no local, no bashisms)
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

msg() {
  case "$1" in
    no_model)       printf "$(t 'diskinfer: no .gguf model found\n' 'diskinfer: 没有找到 .gguf 模型\n' 'diskinfer: 沒有找到 .gguf 模型\n')" ;;
    push_tmp)       printf "$(t '  adb push model.gguf /data/local/tmp/llm/models/\n' '  adb push model.gguf /data/local/tmp/llm/models/\n' '  adb push model.gguf /data/local/tmp/llm/models/\n')" ;;
    push_sdcard)    printf "$(t '  adb push model.gguf /storage/emulated/0/models/\n' '  adb push model.gguf /storage/emulated/0/models/\n' '  adb push model.gguf /storage/emulated/0/models/\n')" ;;
    or_m)           printf "$(t '  or: diskinfer -m /path/to/model.gguf\n' '  或者直接指定：diskinfer -m /path/to/model.gguf\n' '  或者直接指定：diskinfer -m /path/to/model.gguf\n')" ;;
    no_disktui)     printf "$(t 'diskinfer: disktui not installed (expected %s; run setup.sh)\n' 'diskinfer: 没有安装 disktui（预期在 %s；先运行 setup.sh）\n' 'diskinfer: 沒有安裝 disktui（預期在 %s；先執行 setup.sh）\n')" "$2" ;;
    arena_number)   printf "$(t 'diskinfer: --arena needs a number in MiB (128..2048)\n' 'diskinfer: --arena 需要一个数字（MiB，128..2048）\n' 'diskinfer: --arena 需要一個數字（MiB，128..2048）\n')" ;;
    arena_floor)    printf "$(t 'diskinfer: --arena %s is below the 128 MiB floor, using 128\n' 'diskinfer: --arena %s 低于 128 MiB 下限，改用 128\n' 'diskinfer: --arena %s 低於 128 MiB 下限，改用 128\n')" "$2" ;;
    arena_cap)      printf "$(t 'diskinfer: --arena %s is above the 2048 MiB cap, using 2048\n' 'diskinfer: --arena %s 超过 2048 MiB 上限，改用 2048\n' 'diskinfer: --arena %s 超過 2048 MiB 上限，改用 2048\n')" "$2" ;;
    arena_clamped)  printf "$(t 'diskinfer: --arena %s MiB clamped to %s MiB (MemAvailable %s MiB - 220 MiB reserve)\n' 'diskinfer: --arena %s MiB 收窄为 %s MiB（MemAvailable %s MiB - 220 MiB 预留）\n' 'diskinfer: --arena %s MiB 收窄為 %s MiB（MemAvailable %s MiB - 220 MiB 預留）\n')" "$2" "$3" "$4" ;;
  esac
}

L10N=$(detect_lang)

# su gives us a very small PATH, so point it at the termux tools first
export PATH=/data/data/com.termux/files/usr/bin:/system/bin:/system/xbin:/vendor/bin:/sbin:/usr/bin:/bin
export HOME=/data/data/com.termux/files/home

TMP=/data/local/tmp/llm
L=$HOME/llm
LOGDIR=$TMP

# read one value out of /proc/meminfo without calling out to sed/awk
meminfo_kb() {
  while read -r key val _; do
    if [ "${key%:}" = "$1" ]; then
      echo "$val"
      return
    fi
  done < /proc/meminfo
}

# only hand the engine the directories that exist; ':' separated (contract v2 §3)
DIRS=""
for d in "$TMP/models" "$HOME/models" "/storage/emulated/0/models"; do
  [ -d "$d" ] || continue
  if [ -z "$DIRS" ]; then DIRS=$d; else DIRS=$DIRS:$d; fi
done
[ -n "$DIRS" ] && export DISKINFER_MODEL_DIRS=$DIRS

M=""
if [ $# -gt 0 ]; then
  case "$1" in
    -m) M=$2; shift 2 ;;
    -*) : ;;
    *)  M=$1; shift ;;
  esac
fi

# --arena is normally the first engine argument (the termux launcher puts it there);
# parse it here so we can clamp it, then hand the rest through untouched.
ARENA=""
if [ $# -gt 0 ]; then
  case "$1" in
    --arena)   ARENA=$2; shift 2 ;;
    --arena=*) ARENA=${1#*=}; shift ;;
  esac
fi

if [ "${DISKINFER_NO_AUTOMODEL:-0}" = "1" ]; then
  # the termux launcher found several models and wants the TUI picker instead
  :
elif [ -z "$M" ]; then
  for d in "$TMP/models" "$HOME/models" "/storage/emulated/0/models"; do
    [ -d "$d" ] || continue
    for f in "$d"/*.gguf; do
      [ -f "$f" ] && M=$f && break
    done
    [ -n "$M" ] && break
  done
fi

if [ -z "$M" ] && [ "${DISKINFER_NO_AUTOMODEL:-0}" != "1" ]; then
  msg no_model
  msg push_tmp
  msg push_sdcard
  msg or_m
  exit 1
fi

BIN=$L/disktui
if [ ! -x "$BIN" ]; then
  if [ -x "$TMP/disktui" ]; then BIN=$TMP/disktui; else
    msg no_disktui "$L/disktui"
    exit 1
  fi
fi

# clamp: 128 MiB .. 2048 MiB, then down to what the device can actually spare.
# the process needs roughly 220 MiB on top of the arena (vocab, kv, ssm state, compute
# buffers), so keep that as the reserve.
if [ -n "$ARENA" ]; then
  case "$ARENA" in
    *[!0-9]*) msg arena_number; exit 1 ;;
  esac
  if [ "$ARENA" -lt 128 ]; then
    msg arena_floor "$ARENA"
    ARENA=128
  fi
  if [ "$ARENA" -gt 2048 ]; then
    msg arena_cap "$ARENA"
    ARENA=2048
  fi

  avail_kb=$(meminfo_kb MemAvailable)
  if [ -n "$avail_kb" ]; then
    budget=$(( avail_kb / 1024 - 220 ))
    if [ "$budget" -lt 176 ]; then budget=176; fi
    if [ "$ARENA" -gt "$budget" ]; then
      msg arena_clamped "$ARENA" "$budget" "$(( avail_kb / 1024 ))"
      ARENA=$budget
    fi
  fi
  export LLAMA_DISK_STREAM_MB=$ARENA
fi

mkdir -p "$LOGDIR" 2>/dev/null
ulimit -l unlimited 2>/dev/null
echo -1000 > /proc/self/oom_score_adj 2>/dev/null

# performance governor on whatever cores exist
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  echo performance > "$g" 2>/dev/null
done

export LLAMA_DISK_LOG=$LOGDIR/diskinfer.log

if [ -n "$M" ]; then
  exec "$BIN" -m "$M" "$@"
else
  exec "$BIN" "$@"
fi
