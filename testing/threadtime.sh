#!/bin/sh
#
# Per-THREAD CPU breakdown for a running minarch.
#
# Why this exists: frametime.sh reports process-wide CPU, which is the sum over
# every thread. But threads run concurrently on this device's 4 cores, so that
# sum is NOT the frame's critical path. A background worker burning 2ms/frame
# adds 2ms to frametime.sh's number while costing the frame nothing.
#
# For the paired-core budget question only the emulation thread's cost gets
# doubled -- a second console does not duplicate the audio callback or any
# worker. So the number that matters is the biggest thread here, not the total.
#
# Usage:  threadtime.sh [seconds] [fps]     defaults: 10s, 59.7275

WINDOW=${1:-10}
FPS=${2:-59.7275}
CORE=${3:-0}
HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)

PID=$(ps | grep '[m]inarch.elf' | awk '{print $1}' | head -1)
if [ -z "$PID" ]; then
	echo "no minarch.elf running -- launch a game first"
	exit 1
fi

snap() {
	for s in /proc/$PID/task/*/stat; do
		[ -r "$s" ] || continue
		awk '{ n=split($0,a,") "); split(a[n],f," ");
		       name=$0; sub(/^[0-9]+ \(/,"",name); sub(/\).*$/,"",name);
		       print $1, name, f[12]+f[13] }' "$s" 2>/dev/null
	done
}

T0=/tmp/.tt0.$$; T1=/tmp/.tt1.$$
snap > "$T0"
W0=$(awk '{print $1}' /proc/uptime)
sleep "$WINDOW"
W1=$(awk '{print $1}' /proc/uptime)
snap > "$T1"

awk -v w0="$W0" -v w1="$W1" -v hz="$HZ" -v fps="$FPS" -v core="$CORE" '
NR==FNR { was[$1]=$3; name[$1]=$2; next }
{
	d = $3 - (($1 in was) ? was[$1] : 0)
	if (d < 0) d = 0
	tid[$1] = d; name[$1] = $2
}
END {
	wall = w1 - w0
	if (wall <= 0) { print "bad sample window"; exit 1 }
	budget = 1000.0/fps
	total = 0; maxms = 0; maxname = "-"
	printf "%-8s %-18s %10s %8s\n", "TID", "THREAD", "ms/frame", "%core"
	for (t in tid) {
		ms = (tid[t]/hz) / wall * 1000.0 / fps
		if (ms < 0.005) continue
		total += ms
		if (ms > maxms) { maxms = ms; maxname = name[t] }
		printf "%-8s %-18s %10.2f %7.1f%%\n", t, name[t], ms, (tid[t]/hz)/wall*100
	}
	printf "\n%-30s %10.2f ms\n", "sum of all threads", total
	printf "%-30s %10.2f ms  <- the critical path\n", "busiest (" maxname ")", maxms
	printf "%-30s %10.2f ms\n", "frame budget", budget
	printf "%-30s %10.1f %%\n", "budget used today", 100*maxms/budget

	# The sum is NOT the critical path -- other threads run on other cores.
	# Only the emulation thread gates the frame, and pairing adds one more
	# retro_run to it. The frontend work already on that thread is paid once:
	#
	#   paired serial   = busiest + core      (busiest = frontend_sync + core)
	#   paired parallel = busiest + barrier   (2nd console on a 2nd core)
	#
	# Pass the core-only cost as arg 3 to resolve these.
	if (core > 0) {
		ser = maxms + core
		printf "\nwith core-only = %.2f ms (frontend on that thread = %.2f ms):\n", \
			core, maxms - core
		printf "  paired serial    %6.2f ms  %5.1f%% of budget  %s   ~%.0f fps\n", \
			ser, 100*ser/budget, (ser < budget ? "FITS" : "OVER"), \
			(ser < budget ? fps : 1000.0/ser)
		printf "  paired parallel  %6.2f ms  %5.1f%% of budget  %s   + barrier cost\n", \
			maxms, 100*maxms/budget, (maxms < budget ? "FITS" : "OVER")
	} else {
		printf "\n  pass core-only ms as arg 3 for the paired estimate\n"
	}
}' "$T0" "$T1"

rm -f "$T0" "$T1"
