#!/bin/sh
set -eu
. "$(dirname "$0")/lib.sh"
case ${1:-} in
deploy)
 for d in b h; do
  case $d in b) platform=tg5040 ;; *) platform=h700 ;; esac
  on "$d" "set -e; saved=/tmp/netplay-ui-housekeeping-original; mkdir -p \$saved; if [ ! -f \$saved/saved ]; then cp -a /mnt/SDCARD/App/Netplay/spruce_ui \$saved/; cp /mnt/SDCARD/App/Netplay/bin/$platform/netplay.elf \$saved/; cp /mnt/SDCARD/App/Netplay/bin/$platform/netplay-broker.elf \$saved/; cp /mnt/SDCARD/App/Netplay/bin/$platform/netplay_shim.so \$saved/; touch \$saved/saved; fi; mkdir -p /tmp/netplay-ui-stage"
  if [ "$d" = b ]; then
   adb -s "$BRICK_ADB" push "$ROOT/app/spruce/." /tmp/netplay-ui-stage/spruce_ui/ >/dev/null
   for name in netplay.elf netplay-broker.elf; do adb -s "$BRICK_ADB" push "$ROOT/bin/spruce/$platform/$name" /tmp/netplay-ui-stage/ >/dev/null; done
   adb -s "$BRICK_ADB" push "$ROOT/bin/$platform/netplay_shim.so" /tmp/netplay-ui-stage/ >/dev/null
  else
   tar -cf - -C "$ROOT/app" spruce -C "$ROOT/bin/spruce/$platform" netplay.elf netplay-broker.elf -C "$ROOT/bin/$platform" netplay_shim.so | on "$d" 'tar -xf - -C /tmp/netplay-ui-stage'
   on "$d" 'mv /tmp/netplay-ui-stage/spruce /tmp/netplay-ui-stage/spruce_ui'
  fi
  on "$d" "set -e; cp /tmp/netplay-ui-stage/spruce_ui/*.py /mnt/SDCARD/App/Netplay/spruce_ui/; cp /tmp/netplay-ui-stage/spruce_ui/CHEEVOS-LICENSE /mnt/SDCARD/App/Netplay/spruce_ui/; for name in netplay.elf netplay-broker.elf netplay_shim.so; do cp /tmp/netplay-ui-stage/\$name /mnt/SDCARD/App/Netplay/bin/$platform/\$name.new; chmod 755 /mnt/SDCARD/App/Netplay/bin/$platform/\$name.new; mv /mnt/SDCARD/App/Netplay/bin/$platform/\$name.new /mnt/SDCARD/App/Netplay/bin/$platform/\$name; done; sha256sum /mnt/SDCARD/App/Netplay/bin/$platform/netplay.elf"
 done
 ;;
defaults)
 for d in b h; do
  case $d in b) platform=tg5040 ;; *) platform=h700 ;; esac
  on "$d" ". /mnt/SDCARD/spruce/scripts/helperFunctions.sh >/dev/null 2>&1; export PLATFORM=$platform NETPLAY_STATE=/tmp/netplay-ui-setting-test NETPLAY_PAK=/tmp/netplay-ui-setting-test/pak SDCARD_PATH=/tmp/netplay-ui-setting-test/card; export LD_LIBRARY_PATH=/mnt/SDCARD/App/Netplay/bin/$platform:\${LD_LIBRARY_PATH:-}; mkdir -p \$NETPLAY_PAK \$SDCARD_PATH/.userdata/shared; engine=/mnt/SDCARD/App/Netplay/bin/$platform/netplay.elf; \$engine --ctl settings; \$engine --ctl set hybrid_network 0; \$engine --ctl settings; \$engine --ctl set hybrid_network 1; \$engine --ctl settings"
 done
 ;;
update-ui)
 adb -s "$BRICK_ADB" push "$ROOT/app/spruce/." /mnt/SDCARD/App/Netplay/spruce_ui/ >/dev/null
 tar -cf - -C "$ROOT/app/spruce" . | on h 'tar -xf - -C /mnt/SDCARD/App/Netplay/spruce_ui'
 ;;
open)
 for d in b h; do on "$d" "printf '%s\n' '/mnt/SDCARD/App/Netplay/launch.sh' > /tmp/DevBridge-request"; done
 ;;
reopen-host)
 on b 'printf "%s\n" "cd /mnt/SDCARD/App/Netplay; ./launch.sh" > /tmp/cmd_to_run.sh; for p in $(pidof MainUI); do tr "\000" " " < /proc/$p/cmdline | grep -q "[m]ainui.py" && kill -9 "$p"; done'
 ;;
keys)
 d=$2;shift 2
 for button in "$@"; do on "$d" "/mnt/SDCARD/App/DevBridge/bin/input tap $button ${UI_TAP_MS:-80}"; sleep "${UI_GAP:-.3}"; done
 ;;
shot)
 db exec all:SCREEN "${2:-ui-housekeeping}"
 ;;
input-status)
 for d in b h; do on "$d" 'for f in /sys/class/input/event*/device/name; do echo "$f: $(cat "$f")"; done; /mnt/SDCARD/App/DevBridge/bin/input release-all'; done
 ;;
logs)
 for d in b h; do on "$d" 'tail -25 /mnt/SDCARD/Saves/netplay/pyui.log; tail -15 /mnt/SDCARD/Saves/netplay/logs/netplay.txt'; done
 ;;
*) exit 2 ;;
esac
