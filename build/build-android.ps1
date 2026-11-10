#requires -Version 5.1
<#
android cross compile for the diskinfer fork, on a *windows* host using the android NDK.
(the matching POSIX script is build/build-android.sh)

  pwsh -File build\build-android.ps1                          # all abis
  pwsh -File build\build-android.ps1 -Abi x86_64              # one abi
  pwsh -File build\build-android.ps1 -Clean                   # wipe build/build-<abi> first
  $env:ANDROID_NDK_HOME='D:\android-ndk-r27c'; pwsh -File build\build-android.ps1

output: build/out/<abi>/{disktui,diskrun,llama-completion}
#>
param(
    [string[]]$Abi = @('x86_64', 'arm64-v8a', 'armeabi-v7a'),
    [string]$Ndk = $(if ($env:ANDROID_NDK_HOME) { $env:ANDROID_NDK_HOME } elseif (Test-Path 'C:\dev\android-ndk-r27c') { 'C:\dev\android-ndk-r27c' } else { '' }),
    [string]$BuildRoot = '',
    [string[]]$Target = @('llama-diskui', 'llama-diskrun', 'llama-completion'),
    [int]$Jobs = 0,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
# `-File script.ps1 -Abi arm64-v8a,armeabi-v7a` arrives as one string: split the lists by hand
$Abi = @($Abi | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$Target = @($Target | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $Here
$Src  = Join-Path $Root 'src/llama.cpp-fork'
# -BuildRoot isolates BOTH the cmake tree and the output, so several people can build in parallel
if ($BuildRoot) {
    $OutRoot = $BuildRoot
} else {
    $OutRoot = Join-Path $Here 'out'
}

if (-not $Ndk -or -not (Test-Path $Ndk)) {
    throw "android NDK not found; pass -Ndk <path> or set ANDROID_NDK_HOME (needs r26 or newer)"
}
$toolchain = Join-Path $Ndk 'build/cmake/android.toolchain.cmake'
if (-not (Test-Path $toolchain)) { throw "not an android NDK: $Ndk" }

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue)
if (-not $cmake) { throw 'cmake not found in PATH' }
$ninja = (Get-Command ninja -ErrorAction SilentlyContinue)
if (-not $ninja) {
    $cand = Join-Path $env:LOCALAPPDATA 'Programs/Python/Python313/Scripts/ninja.exe'
    if (Test-Path $cand) { $ninja = Get-Item $cand } else { throw 'ninja not found in PATH' }
}
$strip = Get-ChildItem (Join-Path $Ndk 'toolchains/llvm/prebuilt') -Directory |
    ForEach-Object { Join-Path $_.FullName 'bin/llvm-strip.exe' } |
    Where-Object { Test-Path $_ } | Select-Object -First 1

if ($Jobs -le 0) { $Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 2) }

Write-Host "ndk    : $Ndk"
Write-Host "cmake  : $($cmake.Source)"
Write-Host "ninja  : $($ninja.Source)"
Write-Host "jobs   : $Jobs"

function Build-Abi([string]$abi) {
    switch ($abi) {
        'arm64-v8a'   { $cflags = '-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE' }
        'x86_64'      { $cflags = '-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE' }
        'armeabi-v7a' { $cflags = '-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -mfpu=neon-vfpv4 -mfloat-abi=softfp' }
        default { throw "unsupported abi: $abi (use arm64-v8a, armeabi-v7a or x86_64)" }
    }

    $build = if ($BuildRoot) { Join-Path $BuildRoot "build-$abi" } else { Join-Path $Here "build-$abi" }
    if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

    Write-Host ''
    Write-Host "=== $abi ===" -ForegroundColor Cyan
    & $cmake.Source -S $Src -B $build -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$($ninja.Source)" `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        "-DANDROID_ABI=$abi" `
        '-DANDROID_PLATFORM=android-24' `
        '-DANDROID_STL=c++_static' `
        "-DCMAKE_C_FLAGS=$cflags" `
        "-DCMAKE_CXX_FLAGS=$cflags" `
        '-DCMAKE_EXE_LINKER_FLAGS=-static' `
        '-DBUILD_SHARED_LIBS=OFF' `
        '-DCMAKE_BUILD_TYPE=Release' `
        '-DLLAMA_BUILD_TESTS=OFF' `
        '-DLLAMA_BUILD_EXAMPLES=ON' `
        '-DLLAMA_BUILD_SERVER=OFF' `
        '-DLLAMA_CURL=OFF' `
        '-DLLAMA_OPENSSL=OFF' `
        '-DGGML_OPENMP=OFF' `
        '-DGGML_NATIVE=OFF' `
        '-DGGML_LLAMAFILE=OFF' `
        '-DDISKINFER_UI_VERSION=1.0.1'
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $abi" }

    & $cmake.Source --build $build --target @Target -- -j $Jobs
    if ($LASTEXITCODE -ne 0) { throw "build failed for $abi" }

    $outDir = Join-Path $OutRoot $abi
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $map = @{
        'llama-diskui'       = 'disktui'
        'llama-diskrun'      = 'diskrun'
        'llama-completion'   = 'llama-completion'
    }
    foreach ($t in $Target) {
        $exe = Join-Path $build "bin/$t.exe"
        if (-not (Test-Path $exe)) { $exe = Join-Path $build "bin/$t" }
        if (-not (Test-Path $exe)) { throw "missing build output: $exe" }
        $dst = Join-Path $outDir $(if ($map.ContainsKey($t)) { $map[$t] } else { $t })
        Copy-Item $exe $dst -Force
        if ($strip) { & $strip --strip-all $dst | Out-Null }
        $sizeMb = [math]::Round((Get-Item $dst).Length / 1MB, 2)
        Write-Host ("  {0,-18} {1,8} MB" -f (Split-Path -Leaf $dst), $sizeMb)
    }
}

foreach ($abi in $Abi) { Build-Abi $abi }
Write-Host ''
Write-Host "done, binaries in $OutRoot" -ForegroundColor Green
