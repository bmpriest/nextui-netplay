#!/bin/sh
# Temporary transport experiment deployment; preserve the installed binaries.
set -eu
. "$(dirname "$0")/lib.sh"
worktree=${TRANSPORT_WORKTREE:-$ROOT/../.worktrees/netplay-net-traffic}
case $1 in
deploy)
 for d in b h; do
  case $d in b) platform=tg5040 ;; h) platform=h700 ;; esac
  on "$d" "set -e; mkdir -p /tmp/netplay-transport-original; test -e /tmp/netplay-transport-original/netplay_shim.so || cp /mnt/SDCARD/App/Netplay/bin/$platform/netplay_shim.so /tmp/netplay-transport-original/netplay_shim.so; test -e /tmp/netplay-transport-original/retroarch.sh || cp /mnt/SDCARD/App/Netplay/launcher/retroarch.sh /tmp/netplay-transport-original/retroarch.sh"
  if [ "$d" = b ]; then
   adb -s "$BRICK_ADB" push "$worktree/bin/$platform/netplay_shim.so" /tmp/netplay-transport-shim.so >/dev/null
  else
   tar -cf - -C "$worktree/bin/$platform" netplay_shim.so | on "$d" 'tar -xf - -C /tmp'
   on "$d" 'mv /tmp/netplay_shim.so /tmp/netplay-transport-shim.so'
  fi
  on "$d" "set -e; cp /tmp/netplay-transport-shim.so /mnt/SDCARD/App/Netplay/bin/$platform/netplay_shim.so; sed '1a [ ! -f /tmp/netplay-transport.env ] || . /tmp/netplay-transport.env' /tmp/netplay-transport-original/retroarch.sh > /mnt/SDCARD/App/Netplay/launcher/retroarch.sh.tmp; chmod 755 /mnt/SDCARD/App/Netplay/launcher/retroarch.sh.tmp; mv /mnt/SDCARD/App/Netplay/launcher/retroarch.sh.tmp /mnt/SDCARD/App/Netplay/launcher/retroarch.sh; sha256sum /mnt/SDCARD/App/Netplay/bin/$platform/netplay_shim.so"
 done
 ;;
tcp|hybrid)
 for d in b h; do
  if [ "$1" = tcp ]; then command='export NETLINK_TEST_NO_UDP=1'; else command='unset NETLINK_TEST_NO_UDP'; fi
  on "$d" "printf '%s\n' '$command' > /tmp/netplay-transport.env"
 done
 ;;
restore)
 for d in b h; do
  case $d in b) platform=tg5040 ;; h) platform=h700 ;; esac
  on "$d" "set -e; cp /tmp/netplay-transport-original/netplay_shim.so /mnt/SDCARD/App/Netplay/bin/$platform/netplay_shim.so; cp /tmp/netplay-transport-original/retroarch.sh /mnt/SDCARD/App/Netplay/launcher/retroarch.sh; rm -f /tmp/netplay-transport.env; sha256sum /mnt/SDCARD/App/Netplay/bin/$platform/netplay_shim.so"
  on "$d" '. /mnt/SDCARD/spruce/scripts/helperFunctions.sh >/dev/null 2>&1; . /mnt/SDCARD/App/Netplay/launcher/firmware.sh; PLATFORM=$(np_spruce_platform "$PLATFORM"); export PLATFORM; sh /mnt/SDCARD/App/Netplay/launcher/ra-hook.sh down; sh /mnt/SDCARD/App/Netplay/launcher/ra-hook.sh up'
 done
 ;;
*) exit 2 ;;
esac
