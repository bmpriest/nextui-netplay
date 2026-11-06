#!/bin/sh
# Select spruce's DS emulator for one test ROM, preserving its config.
set -e
cfg=/mnt/SDCARD/Emu/NDS/config.json
saved=/tmp/netplay-devbridge/nds-config.json
jq=/mnt/SDCARD/spruce/bin64/jq
. /mnt/SDCARD/spruce/scripts/helperFunctions.sh
case "$PLATFORM" in
	Brick|BrickPro|SmartPro|SmartProS|Flip) key=Emulator_Flip ;;
	Anbernic*) key=Emulator_RGB30 ;;
	*) echo "unsupported DS test device: $PLATFORM" >&2; exit 1 ;;
esac
case "$1" in
	set)
		case "$2" in DraStic-original|DSperate) ;; *) exit 2 ;; esac
		[ -f "$saved" ] || cp "$cfg" "$saved"
		"$jq" --arg key "$key" --arg core "$2" \
			'.menuOptions[$key].overrides["mariokart.nds"] = $core' "$saved" > "$cfg.tmp"
		mv "$cfg.tmp" "$cfg"
		echo "NDS/mariokart.nds -> $2"
		;;
	restore)
		if [ -f "$saved" ]; then cp "$saved" "$cfg"; rm -f "$saved"; fi
		;;
	*) echo "usage: nds-core.sh set DraStic-original|DSperate | restore" >&2; exit 2 ;;
esac
