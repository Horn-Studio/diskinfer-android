#!/system/bin/sh
# diskinfer launcher (runs as root)
#
#   run_as_root.sh <model.gguf> [--arena MiB] [engine args...]
#
# --arena N : how much RAM the weight streaming arena may use, clamped to what this
#             device can actually spare. bigger = fewer re-reads, and if it can hold the
#             whole model the run is as fast as a normal in-RAM inference.

# su gives us a very small PATH, so point it at the termux tools first
export PATH=/data/data/com.termux/files/usr/bin:/system/bin:/system/xbin:/vendor/bin:/sbin:/usr/bin:/bin
export HOME=/data/data/com.termux/files/home

# read one value out of /proc/meminfo without calling out to sed/awk
meminfo_kb() {
  while read -r key val _; do
    if [ "${key%:}" = "$1" ]; then
      echo "$val"
      return
    fi
  done < /proc/meminfo
}

M="$1"
shift

L=/data/data/com.termux/files/home/llm
LOGDIR=/data/local/tmp/llm

ARENA=""
ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --arena)   ARENA="$2"; shift 2 ;;
    --arena=*) ARENA="${1#*=}"; shift ;;
    *) ARGS+=("$1"); shift ;;
  esac
done

# clamp: 128 MiB .. 2048 MiB, then down to what the device can actually spare.
# the process needs roughly 220 MiB on top of the arena (vocab, kv, ssm state, compute
# buffers), so keep that as the reserve.
if [ -n "$ARENA" ]; then
  case "$ARENA" in
    *[!0-9]*) echo "diskinfer: --arena needs a number in MiB (128..2048)"; exit 1 ;;
  esac
  if [ "$ARENA" -lt 128 ]; then
    echo "diskinfer: --arena $ARENA is below the 128 MiB floor, using 128"
    ARENA=128
  fi
  if [ "$ARENA" -gt 2048 ]; then
    echo "diskinfer: --arena $ARENA is above the 2048 MiB cap, using 2048"
    ARENA=2048
  fi

  avail_kb=$(meminfo_kb MemAvailable)
  if [ -n "$avail_kb" ]; then
    budget=$(( avail_kb / 1024 - 220 ))
    if [ "$budget" -lt 176 ]; then budget=176; fi
    if [ "$ARENA" -gt "$budget" ]; then
      echo "diskinfer: --arena $ARENA MiB clamped to $budget MiB (MemAvailable $(( avail_kb / 1024 )) MiB - 220 MiB reserve)"
      ARENA=$budget
    fi
  fi
  export LLAMA_DISK_STREAM_MB=$ARENA
fi

mkdir -p $LOGDIR
ulimit -l unlimited 2>/dev/null
echo -1000 > /proc/self/oom_score_adj 2>/dev/null

# performance governor on whatever cores exist
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  echo performance > "$g" 2>/dev/null
done

export LLAMA_DISK_LOG=$LOGDIR/diskinfer.log

exec $L/disktui -m "$M" "${ARGS[@]}"
