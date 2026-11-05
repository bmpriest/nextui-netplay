#!/bin/sh
#
# Stock RetroArch on a spruce RG35XXH with extra arguments on every launch,
# for netplay against Netplay's RetroArch protocol. No Netplay session may be
# armed (its own hook covers the same binary).
#
#   ra-wrap.sh up ARG...    e.g. up --connect 192.168.0.143 --nick spruce-h700 -v --log-file /tmp/ratest/ra.log
#                           or   up --host --nick spruce-h700 -v --log-file /tmp/ratest/ra.log
#   ra-wrap.sh down         unmount, stop the capture
#
# A copy of the real binary and a wrapper that prepends ARG go to /tmp/ratest,
# the wrapper is bind-mounted over RetroArch/ra64.h700 until `down` or a
# reboot, and port 55435 is captured to /tmp/ratest/h700.pcap.

BIN=/mnt/sdcard/RetroArch/ra64.h700
case ${1:-} in
up)
	shift
	umount "$BIN" 2>/dev/null
	rm -rf /tmp/ratest; mkdir -p /tmp/ratest
	cp /tmp/netplay/ra/ra64.h700 /tmp/ratest/ra64.h700 2>/dev/null || cp "$BIN" /tmp/ratest/ra64.h700
	head -c 4 /tmp/ratest/ra64.h700 | grep -q ELF || { echo "not the RetroArch binary"; exit 1; }
	printf "%s\n" "$@" > /tmp/ratest/args
	cat > /tmp/ratest/wrap.sh <<'W'
#!/bin/sh
set -- $(cat /tmp/ratest/args) "$@"
exec /tmp/ratest/ra64.h700 "$@"
W
	chmod 755 /tmp/ratest/wrap.sh
	mount --bind /tmp/ratest/wrap.sh "$BIN"
	(/mnt/SDCARD/spruce/bin64/tcpdump -i wlan0 -s 0 -w /tmp/ratest/h700.pcap port 55435 >/dev/null 2>&1 &)
	echo "wrapper up: $*"
	;;
down)
	umount "$BIN" 2>/dev/null
	kill $(pidof tcpdump) 2>/dev/null
	echo "wrapper down"
	;;
*)
	sed -n '3,13p' "$0" >&2; exit 2 ;;
esac
