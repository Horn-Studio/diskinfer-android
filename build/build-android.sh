#!/usr/bin/env bash
# cross compile the diskinfer fork for android (static: x86_64 + arm64 + armv7)
#
#   NDK=/path/to/android-ndk ./build-android.sh              # all abis
#   NDK=... ./build-android.sh x86_64                        # one abi
#   NDK=... ./build-android.sh arm64-v8a
#
# source defaults to ../src/llama.cpp-fork (already patched, nothing to apply)
# output: ./out/<abi>/{disktui,diskrun,llama-completion}
# the windows host equivalent is build/build-android.ps1 (keep the two in sync).
set -e

NDK=${NDK:-${ANDROID_NDK_HOME:-}}
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
  echo "set NDK=/path/to/android-ndk (r26 or newer)" >&2
  exit 1
fi

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SRC=${DISKINFER_SRC:-$ROOT/src/llama.cpp-fork}
OUT=$HERE/out
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
STRIP=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip 2>/dev/null | head -1)

if [ ! -f "$SRC/src/llama-disk.cpp" ]; then
  echo "fork source not found at $SRC" >&2
  echo "expected src/llama.cpp-fork/src/llama-disk.cpp, override with DISKINFER_SRC=" >&2
  exit 1
fi

build_abi() {
  ABI=$1
  case "$ABI" in
    arm64-v8a|x86_64)
      CFLAGS="-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE"
      ;;
    armeabi-v7a)
      # the ndk does not add -mfpu=neon for this abi, without it everything runs scalar
      CFLAGS="-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -mfpu=neon-vfpv4 -mfloat-abi=softfp"
      ;;
    *)
      echo "unknown abi $ABI (use x86_64, arm64-v8a or armeabi-v7a)" >&2
      return 1
      ;;
  esac

  BUILD=$HERE/build-$ABI
  echo "=== $ABI ==="

  cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static \
    -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="-static" -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=ON -DLLAMA_BUILD_SERVER=OFF \
    -DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF \
    -DGGML_OPENMP=OFF -DGGML_NATIVE=OFF -DGGML_LLAMAFILE=OFF \
    -DDISKINFER_UI_VERSION=1.0.1

  cmake --build "$BUILD" --target llama-diskui llama-diskrun llama-completion -j "$JOBS"

  mkdir -p "$OUT/$ABI"
  cp "$BUILD/bin/llama-diskui"     "$OUT/$ABI/disktui"
  cp "$BUILD/bin/llama-diskrun"    "$OUT/$ABI/diskrun"
  cp "$BUILD/bin/llama-completion" "$OUT/$ABI/llama-completion"

  # the release binaries are stripped: 52 MB -> ~4.6 MB, 132 MB -> ~7.5 MB
  if [ -n "$STRIP" ] && [ -x "$STRIP" ]; then
    "$STRIP" --strip-all "$OUT/$ABI/disktui" "$OUT/$ABI/diskrun" "$OUT/$ABI/llama-completion"
  fi

  ls -la "$OUT/$ABI"
}

if [ -n "$1" ]; then
  build_abi "$1"
else
  build_abi x86_64
  build_abi arm64-v8a
  build_abi armeabi-v7a
fi

echo
echo "done, binaries in $OUT"
