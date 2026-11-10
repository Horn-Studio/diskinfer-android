#!/usr/bin/env bash
# read-only smoke checks for an installed diskinfer device (v4: x86_64 + arm64 + armv7).
#
#   ADB=/path/to/adb ./test/android-smoke.sh
#   ADB=... ./test/android-smoke.sh                 # also tries the /v1 API if it is up
#   SMOKE_429=1 ADB=... ./test/android-smoke.sh     # additionally exercise the 429 busy path
#
# only adb shell commands that *read* state are used: nothing is pushed,
# installed or killed. exception: the optional API section creates an adb forward
# (removed again) when disktui is already running with the API on.
# exit 0 = all checks pass, 1 = at least one failed, 2 = no device.
set -u

ADB=${ADB:-adb}
TMP=/data/local/tmp/llm
H=/data/data/com.termux/files/home
PRE=/data/data/com.termux/files/usr
LOG1=$TMP/diskinfer.log
LOG2=$H/diskinfer.log
PORT=4000

ok=0
bad=0

pass() { ok=$((ok + 1));  printf '[PASS] %s\n' "$1"; }
fail() { bad=$((bad + 1)); printf '[FAIL] %s\n' "$1"; }
note() { printf '       %s\n' "$1"; }

if ! "$ADB" devices 2>/dev/null | grep -q "device$"; then
    echo "no adb device connected (check: adb devices)" >&2
    exit 2
fi
pass "adb device connected"

printf '\n== 1. platform / which package to install ==\n'
ABILIST=$("$ADB" shell getprop ro.product.cpu.abilist | tr -d '\r')
PRIMARY=$("$ADB" shell getprop ro.product.cpu.abi | tr -d '\r')
SDK=$("$ADB" shell getprop ro.build.version.sdk | tr -d '\r')
printf 'abilist: %s\nprimary: %s\nsdk    : %s\n' "$ABILIST" "$PRIMARY" "$SDK"
# the primary abi is what natively runs; an emulator also reports arm64 (translation)
case "$PRIMARY" in
    x86_64)      note "→ 用 diskinfer-v4-x86_64.zip（模拟器 / Android-x86 / ChromeOS）" ;;
    arm64-v8a)   note "→ 用 diskinfer-v4-arm64-v8a.zip（64 位 ARM：手机/平板/盒子）" ;;
    armeabi-v7a) note "→ 用 diskinfer-v4-armeabi-v7a.zip（32 位 ARM 老设备）" ;;
    *)
        case ",$ABILIST," in
            *,x86_64,*)    note "→ 用 diskinfer-v4-x86_64.zip（x86_64 设备 / 模拟器）" ;;
            *,arm64-v8a,*) note "→ 用 diskinfer-v4-arm64-v8a.zip（64 位 ARM）" ;;
            *,armeabi-v7a,*) note "→ 用 diskinfer-v4-armeabi-v7a.zip（32 位 ARM）" ;;
            *) note "未知 abilist，按上面的输出选包" ;;
        esac
        ;;
esac
if [ -n "$SDK" ] && [ "$SDK" -ge 24 ] 2>/dev/null; then
    pass "android $SDK >= api 24 (v4 binaries target android-24)"
else
    fail "android sdk '$SDK' is below api 24"
fi

printf '\n== 2. device shell is mksh and supports the root launcher syntax ==\n'
shell_out=$("$ADB" shell 'echo "version=$KSH_VERSION"; f() { echo fn=$1; }; f hi' 2>&1 | tr -d '\r')
printf '%s\n' "$shell_out"
if printf '%s\n' "$shell_out" | grep -q '^fn=hi$'; then
    pass "mksh function/positional parameters work (run_as_root.sh is POSIX-only)"
else
    fail "mksh function call broken: run_as_root.sh must stay POSIX"
fi

printf '\n== 3. install completeness + flat layout (v3 device/ bug) ==\n'
"$ADB" shell "ls -l $TMP $H/llm $PRE/bin/diskinfer $PRE/bin/termfont" 2>&1 | tr -d '\r'
missing=""
for f in disktui diskrun llama-completion setup.sh diskinfer termfont run_as_root.sh; do
    if "$ADB" shell "[ -f $TMP/$f ]" 2>/dev/null; then
        :
    else
        missing="$missing $f"
    fi
done
if [ -z "$missing" ]; then
    pass "flat layout complete: $TMP/{disktui,diskrun,llama-completion,setup.sh,diskinfer,termfont,run_as_root.sh}"
else
    fail "missing/not-flat in $TMP:$missing"
    if "$ADB" shell "[ -f $TMP/device/setup.sh ]" 2>/dev/null; then
        note "found the v3 buggy layout $TMP/device/ - re-run setup.sh (it accepts both layouts and flattens)"
    fi
fi
# run_as_root.sh must land where device/diskinfer looks for it
if "$ADB" shell "[ -f $TMP/run_as_root.sh ]" 2>/dev/null; then
    pass "$TMP/run_as_root.sh is where device/diskinfer expects it"
else
    fail "$TMP/run_as_root.sh missing: rooted launcher would fall back to the termux user"
fi

printf '\n== 4. root ==\n'
if "$ADB" shell su -c id 2>/dev/null | grep -q 'uid=0'; then
    pass "root available (oom_score_adj / mlock / governor will work)"
else
    note "no root: no oom protection, mlock may fail, 1-2 GB devices will be killed by lmkd"
fi

printf '\n== 5. last run: which io path was used ==\n'
logs=$("$ADB" shell "cat $LOG1 $LOG2 2>/dev/null | grep -E 'streaming weights|unbuffered reads|read failed|out of arena' | tail -5" 2>&1 | tr -d '\r')
if [ -z "$logs" ]; then
    note "no diskinfer.log yet (run diskinfer once first)"
else
    printf '%s\n' "$logs"
    if printf '%s\n' "$logs" | grep -q 'direct io = 1'; then
        pass "unbuffered reads in use (direct io = 1)"
    else
        note "direct io = 0: O_DIRECT was refused on this volume (e.g. /storage fuses), see README"
    fi
    if printf '%s\n' "$logs" | grep -q 'read failed'; then
        fail "read failures in the log: check the model file and the storage location"
    fi
fi

printf '\n== 6. running process (if any) ==\n'
pid=$("$ADB" shell pidof disktui 2>/dev/null | tr -d '\r')
if [ -n "$pid" ]; then
    "$ADB" shell "cat /proc/$pid/oom_score_adj; grep VmLck /proc/$pid/status" 2>&1 | tr -d '\r'
    pass "disktui running (pid $pid)"
else
    note "disktui is not running"
fi

printf '\n== 7. /v1 API smoke (optional) ==\n'
if [ -z "$pid" ]; then
    note "skipped: disktui is not running (start it with --api-only -m model.gguf first)"
elif ! command -v curl >/dev/null 2>&1; then
    note "skipped: curl not found on this host"
else
    "$ADB" forward "tcp:$PORT" "tcp:$PORT" >/dev/null 2>&1 || true
    models=$(curl -s -m 5 "http://127.0.0.1:$PORT/v1/models" 2>/dev/null || true)
    if printf '%s' "$models" | grep -q '"object"[[:space:]]*:[[:space:]]*"list"'; then
        pass "GET /v1/models answered"
        printf '       %s\n' "$(printf '%s' "$models" | cut -c1-160)"
    elif [ -n "$models" ]; then
        fail "GET /v1/models answered but not an OpenAI list: $(printf '%s' "$models" | cut -c1-120)"
    else
        note "no answer on 127.0.0.1:$PORT/v1/models (API switch off? run: diskinfer --api-only -m ...)"
    fi
    if [ "${SMOKE_429:-0}" = "1" ]; then
        # opt-in: two overlapping completions, the second must hit the busy path (429)
        req='{"model":"smoke","messages":[{"role":"user","content":"count to 64"}],"max_tokens":64}'
        curl -s -m 60 "http://127.0.0.1:$PORT/v1/chat/completions" \
            -H 'Content-Type: application/json' -d "$req" >/tmp/smoke-429-a.$$ 2>/dev/null &
        first=$!
        sleep 1
        code=$(curl -s -m 10 -o /tmp/smoke-429-b.$$ -w '%{http_code}' \
            "http://127.0.0.1:$PORT/v1/chat/completions" \
            -H 'Content-Type: application/json' -d "$req" 2>/dev/null || true)
        wait "$first" 2>/dev/null || true
        if [ "$code" = "429" ]; then
            pass "concurrent request while busy -> 429"
        else
            fail "concurrent request got HTTP '$code', expected 429"
        fi
        rm -f /tmp/smoke-429-a.$$ /tmp/smoke-429-b.$$
    else
        note "429 path not exercised (set SMOKE_429=1 to run it; it needs a real model load)"
    fi
    "$ADB" forward --remove "tcp:$PORT" >/dev/null 2>&1 || true
fi

printf '\n%d passed, %d failed\n' "$ok" "$bad"
[ "$bad" -eq 0 ]
