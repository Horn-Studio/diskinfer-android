#!/system/bin/sh
# keep diskinfer/termfont reachable from root shells after a reboot
# (magisk replaces PATH, so $PREFIX/bin is not visible to a root termux shell)
PRE=/data/data/com.termux/files/usr
for t in diskinfer termfont; do
  [ -x "$PRE/bin/$t" ] || continue
  for d in /debug_ramdisk /sbin; do
    [ -d "$d" ] && ln -sf "$PRE/bin/$t" "$d/$t" 2>/dev/null
  done
done
