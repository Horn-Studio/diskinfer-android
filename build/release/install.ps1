#requires -Version 5.1
<#
diskinfer android installer - windows side (same logic as install.sh adb mode).

  right-click -> "Run with PowerShell", or from a terminal:
      powershell -ExecutionPolicy Bypass -File .\install.ps1
      powershell -ExecutionPolicy Bypass -File .\install.ps1 -Adb "C:\...\adb.exe" -NoPause
      powershell -ExecutionPolicy Bypass -File .\install.ps1 -Lang zh-Hant

it asks where adb.exe is (a path with quotes / a drag & drop / plain "adb" from PATH all
work), finds the phone, pushes the binaries and the flat launcher scripts to
/data/local/tmp/llm, then:
  * root phone   -> runs setup.sh through su and you are done
  * no root      -> stages everything on /sdcard/Download/diskinfer and prints the one
                    line to run inside Termux (plus the Android 11+ storage permission)

messages: en / zh-Hans / zh-Hant, auto-detected (contract v5 §1.3):
  -Lang CLI > DISKINFER_LANG > LANG/LC_ALL > CurrentUICulture > en.
this file is saved as UTF-8 with BOM so Windows PowerShell 5.1 decodes CJK correctly.
#>
param(
    [string]$Adb = '',
    [string]$Device = '',
    [string]$Lang = '',
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$Here = $PSScriptRoot
if (-not $Here) { $Here = Split-Path -Parent $MyInvocation.MyCommand.Path }
$Tmp = '/data/local/tmp/llm'
$Sdcard = '/sdcard/Download/diskinfer'
$Version = '1.0.1'
$script:Serial = ''

# ---------------------------------------------------------------------------
# i18n
# ---------------------------------------------------------------------------
$script:L10N = 'en'

function Convert-LangTag([string]$Tag) {
    $t = ''
    if ($null -ne $Tag) { $t = $Tag.ToLowerInvariant() }
    if ($t -match 'hant|tw|hk|mo') { return 'zh-Hant' }
    if ($t -match '^zh') { return 'zh-Hans' }
    return 'en'
}

function Set-DiskinferLanguage([string]$CliLang) {
    $tag = $CliLang
    if (-not $tag) { $tag = $env:DISKINFER_LANG }
    if (-not $tag) { $tag = $env:LC_ALL }
    if (-not $tag) { $tag = $env:LANG }
    if (-not $tag) {
        try { $tag = [System.Globalization.CultureInfo]::CurrentUICulture.Name } catch { $tag = 'en' }
    }
    $script:L10N = Convert-LangTag $tag
    $env:DISKINFER_LANG = $script:L10N    # keep childs/launchers in sync
}

# T <en> <zh-Hans> <zh-Hant>
function T([string]$En, [string]$Hans, [string]$Hant) {
    switch ($script:L10N) {
        'zh-Hans' { return $Hans }
        'zh-Hant' { return $Hant }
        default   { return $En }
    }
}

Set-DiskinferLanguage $Lang

function Say  ([string]$s) { Write-Host $s }
function Step ([string]$s) { Write-Host ''; Write-Host (T "== $s ==" "== $s ==" "== $s ==") -ForegroundColor Cyan }

function Resolve-AdbPath([string]$Raw) {
    $p = $Raw
    if ($null -eq $p) { $p = '' }
    $p = $p.Trim()
    # "& ""C:\...\adb.exe""" / start "..." / plain / quoted / unquoted -- peel wrappers
    for ($i = 0; $i -lt 3; $i++) {
        $p = $p.Trim()
        $p = $p -replace '^&\s*', ''                       # powershell call operator
        $p = $p -replace '^start\s+(?:"\s*)?', ''          # cmd start "title"
        while ($true) {                                    # quotes from drag&drop / "copy as path"
            $t = $p.Trim().Trim('"').Trim("'").Trim()
            if ($t -eq $p) { break }
            $p = $t
        }
    }
    if ($p -match '^\$\{?env:([^}"'']+)\}?$') {       # $env:TEMP\platform-tools\adb.exe
        $p = [Environment]::GetEnvironmentVariable($Matches[1])
    }
    if ($p -match '^%([^%]+)%(.*)$') {                # %LOCALAPPDATA%\...\adb.exe
        $p = [Environment]::ExpandEnvironmentVariables('%' + $Matches[1] + '%') + $Matches[2]
    }
    $p = [Environment]::ExpandEnvironmentVariables($p)
    if ($p -eq '') { $p = 'adb' }
    if (Test-Path -LiteralPath $p -PathType Container) {
        foreach ($n in @('adb.exe', 'adb')) {
            $c = Join-Path $p $n
            if (Test-Path -LiteralPath $c) { $p = $c; break }
        }
    }
    if (-not (Test-Path -LiteralPath $p)) {
        if (Test-Path -LiteralPath ($p + '.exe')) { $p = $p + '.exe' }
        elseif (Get-Command $p -ErrorAction SilentlyContinue) { return $p }  # PATH
        else {
            throw (T "cannot find adb: $p`nDrag the full adb.exe path into this window, or add platform-tools to PATH." `
                     "找不到 adb: $p`n把 adb.exe 的完整路径拖进本窗口，或者把 platform-tools 加进 PATH。" `
                     "找不到 adb: $p`n把 adb.exe 的完整路徑拖進本視窗，或者把 platform-tools 加進 PATH。")
        }
    }
    $item = Get-Item -LiteralPath $p
    if ($item.PSIsContainer) {
        throw (T "this is a directory, not adb.exe: $p" "这是一个目录，不是 adb.exe: $p" "這是一個目錄，不是 adb.exe: $p")
    }
    return $item.FullName
}

function Push-One([string]$Local, [string]$RemoteDir) {
    & $script:Adb -s $script:Serial push $Local $RemoteDir 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw (T "adb push failed: $Local -> $RemoteDir" "adb push 失败: $Local -> $RemoteDir" "adb push 失敗: $Local -> $RemoteDir")
    }
}

function Pick-Device {
    $out = (& $script:Adb devices 2>&1 | Out-String)
    if ($Device) {
        $script:Serial = $Device
        Say (T "using -Device $Device" "使用 -Device $Device" "使用 -Device $Device")
        return
    }
    if ($env:ANDROID_SERIAL) {
        $script:Serial = $env:ANDROID_SERIAL
        Say (T "using ANDROID_SERIAL=$($script:Serial)" "使用 ANDROID_SERIAL=$($script:Serial)" "使用 ANDROID_SERIAL=$($script:Serial)")
        return
    }
    $s = $null
    foreach ($line in ($out -split "`r?`n")) {
        $f = $line.Trim() -split '\s+'
        if ($f.Count -ge 2 -and $f[1] -eq 'device') { $s = $f[0]; break }
    }
    if (-not $s) {
        Say $out
        throw (T "no device detected. Enable USB debugging (or adb connect 127.0.0.1:5555) until 'adb devices' shows a device row, then retry." `
                 "没有检测到设备。打开 USB 调试（或先 adb connect 127.0.0.1:5555），直到 adb devices 里出现 device 一列。" `
                 "沒有偵測到裝置。開啟 USB 偵錯（或先 adb connect 127.0.0.1:5555），直到 adb devices 裡出現 device 一列。")
    }
    $script:Serial = $s
}

function Write-RootDone {
    switch ($script:L10N) {
        'zh-Hans' { Say '在手机上打开 Termux，运行：'
                    Say '  diskinfer                  # 打开 TUI（新窗口才在 PATH 上）'
                    Say '  diskinfer -m /storage/emulated/0/models/x.gguf'
                    Say '模型目录：/data/local/tmp/llm/models/ 或 /storage/emulated/0/models/' }
        'zh-Hant' { Say '在手機上開啟 Termux，執行：'
                    Say '  diskinfer                  # 開啟 TUI（新視窗才在 PATH 上）'
                    Say '  diskinfer -m /storage/emulated/0/models/x.gguf'
                    Say '模型目錄：/data/local/tmp/llm/models/ 或 /storage/emulated/0/models/' }
        default   { Say 'open Termux on the phone and run:'
                    Say '  diskinfer                  # open the TUI (new window to get it on PATH)'
                    Say '  diskinfer -m /storage/emulated/0/models/x.gguf'
                    Say 'model dir: /data/local/tmp/llm/models/ or /storage/emulated/0/models/' }
    }
}

function Write-NonRootSteps {
    switch ($script:L10N) {
        'zh-Hans' {
            Say '1) 打开 Termux，先给它存储权限（Android 11+ 必须，二选一）：'
            Say '     Termux 里执行： termux-setup-storage        （弹窗点允许）'
            Say '   或者电脑上执行：'
            Say "     & `"$($script:Adb)`" -s $($script:Serial) shell appops set com.termux MANAGE_EXTERNAL_STORAGE allow"
            Say "     & `"$($script:Adb)`" -s $($script:Serial) shell pm grant com.termux android.permission.READ_EXTERNAL_STORAGE"
            Say ''
            Say '2) Termux 里执行这一行（安装 diskinfer 命令）：'
            Say "     sh $Sdcard/setup.sh $Sdcard/disktui"
            Say ''
            Say '装完后在 Termux 里：'
            Say '  diskinfer                              # 打开 TUI'
            Say '  cp /sdcard/Download/x.gguf ~/models/   # 模型放 Termux 私有目录，永远可读'
            Say '  或者授权后 adb push x.gguf /storage/emulated/0/models/'
            Say ''
            Say "提示：$Tmp 下已经放好 disktui/diskrun/llama-completion 和扁平脚本，"
            Say "      adb shell 里可以直接跑 $Tmp/llama-completion（Termux 读不到这里，所以还要上面那一步）。"
        }
        'zh-Hant' {
            Say '1) 開啟 Termux，先給它儲存權限（Android 11+ 必須，二選一）：'
            Say '     Termux 裡執行： termux-setup-storage        （彈窗點允許）'
            Say '   或者在電腦上執行：'
            Say "     & `"$($script:Adb)`" -s $($script:Serial) shell appops set com.termux MANAGE_EXTERNAL_STORAGE allow"
            Say "     & `"$($script:Adb)`" -s $($script:Serial) shell pm grant com.termux android.permission.READ_EXTERNAL_STORAGE"
            Say ''
            Say '2) Termux 裡執行這一行（安裝 diskinfer 指令）：'
            Say "     sh $Sdcard/setup.sh $Sdcard/disktui"
            Say ''
            Say '裝完後在 Termux 裡：'
            Say '  diskinfer                              # 開啟 TUI'
            Say '  cp /sdcard/Download/x.gguf ~/models/   # 模型放 Termux 私有目錄，永遠可讀'
            Say '  或者授權後 adb push x.gguf /storage/emulated/0/models/'
            Say ''
            Say "提示：$Tmp 下已經放好 disktui/diskrun/llama-completion 和扁平腳本，"
            Say "      adb shell 裡可以直接跑 $Tmp/llama-completion（Termux 讀不到這裡，所以還需要上面那一步）。"
        }
        default {
            Say '1) open Termux and grant it storage access first (required on Android 11+, pick one):'
            Say '     in Termux: termux-setup-storage        (tap Allow in the dialog)'
            Say '   or from the computer:'
            Say "     & `"$($script:Adb)`" -s $($script:Serial) shell appops set com.termux MANAGE_EXTERNAL_STORAGE allow"
            Say "     & `"$($script:Adb)`" -s $($script:Serial) shell pm grant com.termux android.permission.READ_EXTERNAL_STORAGE"
            Say ''
            Say '2) run this one line in Termux (installs the diskinfer command):'
            Say "     sh $Sdcard/setup.sh $Sdcard/disktui"
            Say ''
            Say 'then, in Termux:'
            Say '  diskinfer                              # open the TUI'
            Say '  cp /sdcard/Download/x.gguf ~/models/   # model -> Termux private dir, always readable'
            Say '  or, after granting access: adb push x.gguf /storage/emulated/0/models/'
            Say ''
            Say "note: $Tmp already has disktui/diskrun/llama-completion and the flat scripts,"
            Say "      adb shell can run $Tmp/llama-completion directly (Termux cannot read there, hence step 2)."
        }
    }
}

function Main {
    Step (T "diskinfer $Version installer (windows / adb mode)" "diskinfer $Version 安装器（Windows / adb 模式）" "diskinfer $Version 安裝器（Windows / adb 模式）")

    # ---- script layout: flat release zip / legacy device/ subdir
    $Src = $null
    if (Test-Path (Join-Path $Here 'setup.sh')) { $Src = $Here }
    elseif (Test-Path (Join-Path $Here 'device/setup.sh')) { $Src = (Join-Path $Here 'device') }
    if (-not $Src) {
        throw (T 'cannot find the device scripts (setup.sh): run install.ps1 from the extracted release dir' `
                 '没有找到 device 脚本（setup.sh）：请在解压后的 diskinfer 发布包目录里运行 install.ps1' `
                 '沒有找到 device 腳本（setup.sh）：請在解壓後的 diskinfer 發佈包目錄裡執行 install.ps1')
    }
    $bin = Join-Path $Here 'disktui'
    if (-not (Test-Path $bin)) {
        throw (T 'cannot find disktui: run install.ps1 from the extracted release dir' `
                 '没有找到 disktui：请在解压后的 diskinfer 发布包目录里运行 install.ps1' `
                 '沒有找到 disktui：請在解壓後的 diskinfer 發佈包目錄裡執行 install.ps1')
    }

    # ---- adb
    if ($Adb) {
        $script:Adb = Resolve-AdbPath $Adb
        Say (T "using: $($script:Adb)" "使用: $($script:Adb)" "使用: $($script:Adb)")
    } else {
        Say (T 'tip: drag adb.exe into this window; press Enter to use the adb on PATH.' `
                 '提示：可以把 adb.exe 直接拖进本窗口，直接回车则使用 PATH 里的 adb。' `
                 '提示：可以把 adb.exe 直接拖進本視窗，直接按 Enter 則使用 PATH 裡的 adb。')
        $inputPath = Read-Host (T 'adb path' 'adb 路径' 'adb 路徑')
        $script:Adb = Resolve-AdbPath $inputPath
        Say (T "using: $($script:Adb)" "使用: $($script:Adb)" "使用: $($script:Adb)")
    }

    Step (T 'finding the device' '查找设备' '尋找裝置')
    (& $script:Adb start-server 2>&1 | Out-Null)
    Pick-Device
    Say (T "device: $($script:Serial)" "设备: $($script:Serial)" "裝置: $($script:Serial)")
    $sdk = (& $script:Adb -s $script:Serial shell getprop ro.build.version.sdk | Out-String).Trim()
    $abis = (& $script:Adb -s $script:Serial shell getprop ro.product.cpu.abilist | Out-String).Trim()
    Say (T "Android SDK $sdk / $abis" "Android SDK $sdk / $abis" "Android SDK $sdk / $abis")

    $rootOut = ''
    try { $rootOut = (& $script:Adb -s $script:Serial shell 'su -c id' 2>$null | Out-String) } catch { $rootOut = '' }
    $isRoot = $rootOut -match 'uid=0'
    if ($isRoot) {
        Say (T 'root: yes (install to /data/local/tmp/llm and run setup.sh directly)' `
                 'root: 有（装到 /data/local/tmp/llm 并直接执行 setup.sh）' `
                 'root: 有（裝到 /data/local/tmp/llm 並直接執行 setup.sh）')
    } else {
        Say (T 'root: no (after the push, run setup.sh once in Termux)' `
                 'root: 无（推完后在 Termux 里执行一次 setup.sh）' `
                 'root: 無（推送後在 Termux 裡執行一次 setup.sh）')
    }

    # ---- binaries
    Step (T "pushing binaries to $Tmp" "推送二进制到 $Tmp" "推送二進位到 $Tmp")
    & $script:Adb -s $script:Serial shell "mkdir -p $Tmp" | Out-Null
    foreach ($f in @('disktui', 'diskrun', 'llama-completion')) {
        $p = Join-Path $Here $f
        if (Test-Path $p) { Push-One $p "$Tmp/"; Say "  $f" }
    }
    (& $script:Adb -s $script:Serial shell "chmod 755 $Tmp/disktui $Tmp/diskrun $Tmp/llama-completion 2>/dev/null; true" | Out-Null)

    # ---- launcher scripts: push the *contents* of device/, flat (the bug fix)
    Step (T 'pushing launcher scripts (flat layout)' '推送启动器脚本（扁平布局）' '推送啟動器腳本（扁平佈局）')
    foreach ($f in @('setup.sh', 'diskinfer', 'termfont', 'run_as_root.sh', '99-disktui.sh')) {
        $p = Join-Path $Src $f
        if (Test-Path $p) { Push-One $p "$Tmp/"; Say "  $f" }
    }
    (& $script:Adb -s $script:Serial shell "chmod 755 $Tmp/setup.sh $Tmp/diskinfer $Tmp/termfont $Tmp/run_as_root.sh 2>/dev/null; true" | Out-Null)

    if ($isRoot) {
        Step (T 'installing as root (setup.sh)' '以 root 安装（setup.sh）' '以 root 安裝（setup.sh）')
        & $script:Adb -s $script:Serial shell "su -c 'sh $Tmp/setup.sh $Tmp/disktui'"
        Say ''
        & $script:Adb -s $script:Serial shell "ls -l $Tmp"
        Step (T 'done' '完成' '完成')
        Write-RootDone
    } else {
        Step (T "no root: staging the package into the Termux-readable $Sdcard" "无 root：把安装包放到 Termux 可读的 $Sdcard" "無 root：把安裝包放到 Termux 可讀的 $Sdcard")
        & $script:Adb -s $script:Serial shell "mkdir -p $Sdcard" | Out-Null
        foreach ($f in @('disktui', 'diskrun', 'llama-completion')) {
            $p = Join-Path $Here $f
            if (Test-Path $p) { Push-One $p "$Sdcard/" }
        }
        foreach ($f in @('setup.sh', 'diskinfer', 'termfont', 'run_as_root.sh', '99-disktui.sh')) {
            $p = Join-Path $Src $f
            if (Test-Path $p) { Push-One $p "$Sdcard/" }
        }
        (& $script:Adb -s $script:Serial shell "chmod 755 $Sdcard/* 2>/dev/null; true" | Out-Null)
        Say ''
        & $script:Adb -s $script:Serial shell "ls -l $Tmp $Sdcard"

        $termuxOut = ''
        try { $termuxOut = (& $script:Adb -s $script:Serial shell 'pm list packages com.termux' 2>$null | Out-String) } catch { $termuxOut = '' }
        $termux = $termuxOut -match 'package:com\.termux'

        Step (T 'install it on the phone (2 steps)' '在手机上装（2 步）' '在手機上裝（2 步）')
        if (-not $termux) {
            Say (T '0) install Termux first (F-Droid build): https://f-droid.org/packages/com.termux/' `
                     '0) 先安装 Termux（F-Droid 版）：https://f-droid.org/packages/com.termux/' `
                     '0) 先安裝 Termux（F-Droid 版）：https://f-droid.org/packages/com.termux/')
        }
        Write-NonRootSteps
    }
}

try {
    Main
} catch {
    Write-Host ''
    Write-Host ((T 'install.ps1: ' 'install.ps1: ' 'install.ps1: ') + $_.Exception.Message) -ForegroundColor Red
    if (-not $NoPause) { Read-Host (T 'press Enter to exit' '按回车退出' '按 Enter 離開') | Out-Null }
    exit 1
}

if (-not $NoPause) { Read-Host (T 'press Enter to exit' '按回车退出' '按 Enter 離開') | Out-Null }
exit 0
