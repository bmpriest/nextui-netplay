#!/bin/sh
. "$(dirname "$0")/lib.sh"
for d in b h; do
    echo "DEVICE $d"
    on "$d" 'printf "event_ms="; cat /tmp/netplay-devbridge/event-ms; cat /sys/class/input/event1/device/name 2>/dev/null' || :
    on "$d" 'for n in /sys/class/input/event*/device/name; do case "$(cat "$n")" in ANBERNIC-keys|"TRIMUI Player1") echo "$n"; cat "${n%/name}/capabilities/abs" "${n%/name}/capabilities/key" ;; esac; done' || :
    on "$d" 'command -v python3 || :; ls /mnt/SDCARD/Emu/NDS/lib64/*SDL2*; ls -l /mnt/SDCARD/spruce/flip/bin/MainUI' || :
    on "$d" '. /mnt/SDCARD/spruce/scripts/helperFunctions.sh >/dev/null 2>&1; export_sdl_gamecontroller_map positional; printf "SDL_MAP=%s\n" "$SDL_GAMECONTROLLERCONFIG"' || :
    on "$d" 'grep " /dev/input/" /proc/mounts || :; pgrep -af "[d]evbridge-input"; grep -E "^N: Name|^H: Handlers" /proc/bus/input/devices; grep -n -A12 "^\[pad\]" /mnt/SDCARD/Saves/dsperate/no-sticks.ini /mnt/SDCARD/Saves/dsperate/two-sticks.ini' || :
done
