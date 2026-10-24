#!/system/bin/sh
# device side setup for diskinfer
#   setup.sh <path to disktui binary>
# works with or without root: it writes into the termux home and $PREFIX,
# and (when run as root) also makes the tools visible from root shells.
BIN="${1:?need the path to the disktui binary}"

H=/data/data/com.termux/files/home
PRE=/data/data/com.termux/files/usr
TMP=/data/local/tmp/llm

if [ ! -d "$H" ] || [ ! -x "$PRE/bin/bash" ]; then
  echo "Termux not ready (home=$H). open the Termux app once so it unpacks its bootstrap,"
  echo "then run this script again."
  exit 1
fi

mkdir -p $H/llm $TMP
cp -f "$BIN" $H/llm/disktui
chmod 755 $H/llm/disktui

for f in diskinfer termfont; do
  if [ -f "$TMP/$f" ]; then
    cp -f "$TMP/$f" "$PRE/bin/$f"
    chmod 755 "$PRE/bin/$f"
  fi
done

# ---- make the tools reachable from root shells -------------------------------
# magisk replaces PATH, so a root termux shell does not see $PREFIX/bin
RC=$H/.bashrc
if ! grep -q "termux/files/usr/bin" "$RC" 2>/dev/null; then
  {
    echo ''
    echo '# make the termux tools visible from root shells (magisk replaces PATH)'
    echo 'case ":$PATH:" in'
    echo '  *":/data/data/com.termux/files/usr/bin:"*) ;;'
    echo '  *) export PATH=/data/data/com.termux/files/usr/bin:$PATH ;;'
    echo 'esac'
  } >> "$RC"
  echo "added the path to $RC"
fi

BOOT=/data/adb/service.d/99-disktui.sh
mkdir -p /data/adb/service.d 2>/dev/null
cat > "$BOOT" <<'EOS' 2>/dev/null
#!/system/bin/sh
PRE=/data/data/com.termux/files/usr
for t in diskinfer termfont; do
  [ -x "$PRE/bin/$t" ] || continue
  for d in /debug_ramdisk /sbin; do
    [ -d "$d" ] && ln -sf "$PRE/bin/$t" "$d/$t" 2>/dev/null
  done
done
EOS
chmod 755 "$BOOT" 2>/dev/null
sh "$BOOT" 2>/dev/null

for d in /debug_ramdisk /sbin; do
  [ -d "$d" ] || continue
  for t in diskinfer termfont; do
    [ -x "$PRE/bin/$t" ] && ln -sf "$PRE/bin/$t" "$d/$t" 2>/dev/null && echo "$d/$t -> $PRE/bin/$t"
  done
done

# ownership: only needed when we run as root
if [ "$(id -u)" = "0" ]; then
  OWNER=$(ls -ld "$H" | sed -n 's/^[^ ]* [^ ]* \([^ ]*\).*/\1/p')
  [ -n "$OWNER" ] && chown -R "$OWNER" $H/llm 2>/dev/null
  [ -n "$OWNER" ] && chown "$OWNER" "$PRE/bin/diskinfer" "$PRE/bin/termfont" 2>/dev/null
fi

echo "installed:"
ls -la $PRE/bin/diskinfer $PRE/bin/termfont $H/llm/disktui
