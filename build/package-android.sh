#!/usr/bin/env bash
# assemble the 1.0.1 release zips for android from build/out/<abi>
#
#   ./build/package-android.sh                    # all three abis
#   ./build/package-android.sh arm64-v8a          # one abi
#   VERSION=1.0.1 ./build/package-android.sh         # customize the zip name
#   OUT_DIR=/tmp/fake-out DIST_DIR=/tmp/dist ./build/package-android.sh x86_64   # dry run
#
# expects (produced by build/build-android.ps1 or build/build-android.sh):
#   build/out/<abi>/{disktui,diskrun,llama-completion}
# release metadata:
#   build/release/{install.sh,install.ps1}
#   device/{setup.sh,diskinfer,termfont,run_as_root.sh,99-disktui.sh}
#
# output:
#   dist/diskinfer-1.0.1-<abi>.zip    flat: binaries + launcher scripts + installer
#                                   + SHA256SUMS.txt at the zip root
#   dist/SHA256SUMS.txt            checksums of the zips
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT=${OUT_DIR:-$HERE/out}
DIST=${DIST_DIR:-$ROOT/dist}
REL=$HERE/release
VERSION=${VERSION:-1.0.1}

BINS=(disktui diskrun llama-completion)
DEVICE_FILES=(setup.sh diskinfer termfont run_as_root.sh 99-disktui.sh)
ZIP_EXEC=(disktui diskrun llama-completion install.sh install.ps1 setup.sh diskinfer termfont run_as_root.sh 99-disktui.sh)

ABIS=("$@")
if [ ${#ABIS[@]} -eq 0 ]; then
  ABIS=(x86_64 arm64-v8a armeabi-v7a)
fi

[ -n "$VERSION" ] || { echo "VERSION is empty" >&2; exit 1; }

for f in install.sh install.ps1; do
  [ -f "$REL/$f" ] || { echo "missing build/release/$f" >&2; exit 1; }
done
for f in "${DEVICE_FILES[@]}"; do
  [ -f "$ROOT/device/$f" ] || { echo "missing device/$f" >&2; exit 1; }
done

# windows git-bash cannot hand a /f/... path to a native python/zip.exe
towin() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi; }

find_python() {
  for c in python3 python; do
    if command -v "$c" >/dev/null 2>&1 && "$c" -c 'import zipfile' >/dev/null 2>&1; then
      printf '%s' "$c"
      return 0
    fi
  done
  return 1
}

# deterministic zip: sorted names, fixed mtime, unix modes preserved for exec bits
zip_via_python() { # <stage dir> <zip path> <python>
  local stage=$1 zip=$2 py=$3 zip_win
  zip_win=$(towin "$zip")
  ( cd "$stage" && "$py" - "$zip_win" "${ZIP_EXEC[*]}" <<'PY'
import os
import sys
import zipfile

out = sys.argv[1]
execs = set(sys.argv[2].split())
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(n for n in os.listdir(".") if os.path.isfile(n)):
        data = open(name, "rb").read()
        info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = ((0o100755 if name in execs else 0o100644) << 16)
        z.writestr(info, data)
PY
  )
}

write_sha256sums() { # <stage dir>, creates SHA256SUMS.txt in it (flat layout)
  local stage=$1
  if command -v sha256sum >/dev/null 2>&1; then
    # normalize the "hash *name" (binary mode) form to "hash  name" so `sha256sum -c`
    # and `shasum -c` both accept the file
    ( cd "$stage" && ls -1 | grep -v '^SHA256SUMS.txt$' | LC_ALL=C sort | \
        while IFS= read -r n; do sha256sum "$n" | sed 's/^\([0-9A-Fa-f]\{64\}\) \*/\1  /'; done > SHA256SUMS.txt )
  elif command -v shasum >/dev/null 2>&1; then
    ( cd "$stage" && ls -1 | grep -v '^SHA256SUMS.txt$' | LC_ALL=C sort | \
        while IFS= read -r n; do shasum -a 256 "$n"; done > SHA256SUMS.txt )
  else
    local py
    py=$(find_python) || { echo "cannot compute sha256 (no sha256sum/shasum/python)" >&2; return 1; }
    ( cd "$stage" && "$py" - <<'PY'
import hashlib
import os

names = sorted(n for n in os.listdir(".") if os.path.isfile(n) and n != "SHA256SUMS.txt")
with open("SHA256SUMS.txt", "w", newline="\n") as out:
    for name in names:
        with open(name, "rb") as fh:
            out.write(f"{hashlib.sha256(fh.read()).hexdigest()}  {name}\n")
PY
    )
  fi
}

mkdir -p "$DIST"

for ABI in "${ABIS[@]}"; do
  SRC=$OUT/$ABI
  for b in "${BINS[@]}"; do
    [ -f "$SRC/$b" ] || {
      echo "missing $SRC/$b" >&2
      echo "build them first:  pwsh -File build/build-android.ps1 -Abi $ABI" >&2
      echo "(or: NDK=/path/to/ndk ./build/build-android.sh $ABI)" >&2
      exit 1
    }
  done

  STAGE=$DIST/stage-$ABI
  rm -rf "$STAGE"
  mkdir -p "$STAGE"

  # flat layout: everything at the zip root, no device/ subdir (that was the v3 bug)
  for b in "${BINS[@]}"; do
    cp "$SRC/$b" "$STAGE/$b"
    chmod 755 "$STAGE/$b"
  done
  for f in "${DEVICE_FILES[@]}"; do
    cp "$ROOT/device/$f" "$STAGE/$f"
    chmod 755 "$STAGE/$f"
  done
  cp "$REL/install.sh"  "$STAGE/install.sh";  chmod 755 "$STAGE/install.sh"
  cp "$REL/install.ps1" "$STAGE/install.ps1"; chmod 755 "$STAGE/install.ps1"


  write_sha256sums "$STAGE" || exit 1

  ZIP=$DIST/diskinfer-$VERSION-$ABI.zip
  rm -f "$ZIP"

  if command -v zip >/dev/null 2>&1; then
    ( cd "$STAGE" && zip -qr "$ZIP" . )
  elif PY=$(find_python); then
    zip_via_python "$STAGE" "$ZIP" "$PY"
  else
    echo "no zip and no python3/python available; cannot package $ABI" >&2
    echo "install zip (apt install zip) or python3 and retry" >&2
    exit 1
  fi

  echo "built $ZIP ($(du -h "$ZIP" | cut -f1))"
  echo "  $(cd "$STAGE" && ls -1 | wc -l | tr -d ' ') files: $(cd "$STAGE" && ls -1 | LC_ALL=C sort | tr '\n' ' ')"
  rm -rf "$STAGE"
done

if command -v sha256sum >/dev/null 2>&1; then
  ( cd "$DIST" && sha256sum diskinfer-*.zip | sed 's/^\([0-9A-Fa-f]\{64\}\) \*/\1  /' > SHA256SUMS.txt )
  echo "wrote $DIST/SHA256SUMS.txt"
elif command -v shasum >/dev/null 2>&1; then
  ( cd "$DIST" && shasum -a 256 diskinfer-*.zip > SHA256SUMS.txt )
  echo "wrote $DIST/SHA256SUMS.txt"
fi

echo
echo "done, packages in $DIST"
