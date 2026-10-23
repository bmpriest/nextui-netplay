#!/bin/sh
#
# Per-frame cost for a running minarch, measured from CPU time.
#
# The debug HUD cannot answer this. Its A: field is wall-clock flip-to-flip
# (api.c GFX_flip), which spans GFX_sync's pacing sleep, so it pins at the frame
# period whenever the core keeps up -- a metronome, not a cost. And perf.cpu_usage
# is only ever written by PLAT_cpu_monitor, which my282 does not implement, so the
# HUD's CPU percentage is permanently 0 on this device.
#
# CPU time does not accrue during the pacing sleep. So at a locked frame rate:
#
#     ms of CPU per frame = (CPU seconds / wall seconds) * 1000 / fps
#
# NOTE this is process-wide: it is frontend + core, not core alone. On my282 the
# frontend (blit, scale, GL, audio) costs roughly 5-6ms of the 16.74ms budget
# regardless of which core is loaded. Pass that as arg 3 to get core-only and
# paired-core estimates out the far side.
#
# Usage:  frametime.sh [seconds] [fps] [frontend_ms]
#         defaults: 10s, 59.7275 (GBA/GBC under mGBA), no decomposition
#
#   sh frametime.sh 10 59.7275 5.1     # decompose against a 5.1ms frontend

WINDOW=${1:-10}
FPS=${2:-59.7275}
FRONTEND=${3:-0}
HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)

PID=$(ps | grep '[m]inarch.elf' | awk '{print $1}' | head -1)
if [ -z "$PID" ]; then
	echo "no minarch.elf running -- launch a game first"
	exit 1
fi

# Which arm are we actually measuring? sample_rate.txt is read by launch.sh at
# launch, so editing it under a running game changes nothing - the core is
# already loaded. Report what the *running* instance negotiated, straight from
# its log, so a stale arm cannot be mistaken for a null result.
report_arm() {
	# Newest log that actually carries the marker. Plain `ls -t | head -1` picks
	# whichever log was touched last, which on a live device is the SSH server's.
	log=$(ls -t /mnt/SDCARD/.userdata/*/logs/*.txt 2>/dev/null | while IFS= read -r f; do
		if grep -q "sample rate:" "$f" 2>/dev/null; then echo "$f"; break; fi
	done)
	[ -n "$log" ] || return 0
	req=$(sed -n 's/.*sample rate: \([0-9]*\) (req).*/\1/p' "$log" | tail -1)
	arm=$(sed -n 's/.*launcher: .*rate=\([^ ]*\).*/\1/p' "$log" | tail -1)
	[ -n "$req$arm" ] || return 0
	printf "core audio    %s Hz requested" "${req:-?}"
	[ -n "$arm" ] && printf "  (launched with rate=%s)" "$arm"
	printf "\n"
	if [ "${req:-0}" = "65536" ]; then
		echo "              ^ the core is resampling; relaunch the ROM after"
		echo "                changing sample_rate.txt for the change to apply"
	fi
}

read_cpu() {
	# fields 14,15 of /proc/PID/stat are utime,stime in jiffies, summed over
	# all threads of the process. comm may contain spaces, so cut after ')'.
	awk '{ n=split($0,a,") "); split(a[n],f," "); print f[12]+f[13] }' "/proc/$PID/stat"
}
read_wall() { awk '{print $1}' /proc/uptime; }

FREQ_MAX=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null || echo 0)
GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)

C0=$(read_cpu); W0=$(read_wall)
# sample the clock across the window: the `conservative` governor NextUI uses for
# CPU Speed=Auto only ramps above 80% load, and one mGBA instance sits just under
# that, so a measurement taken here can silently be at a fraction of full speed.
FSUM=0; FN=0; FMIN=99999999; FMAX=0
i=0
while [ "$i" -lt "$WINDOW" ]; do
	f=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
	FSUM=$((FSUM + f)); FN=$((FN + 1))
	[ "$f" -lt "$FMIN" ] && FMIN=$f
	[ "$f" -gt "$FMAX" ] && FMAX=$f
	sleep 1
	i=$((i + 1))
done
C1=$(read_cpu); W1=$(read_wall)

if [ ! -e "/proc/$PID/stat" ]; then
	echo "process exited during sampling"
	exit 1
fi

report_arm

awk -v c0="$C0" -v c1="$C1" -v w0="$W0" -v w1="$W1" -v hz="$HZ" -v fps="$FPS" \
    -v fsum="$FSUM" -v fn="$FN" -v fmin="$FMIN" -v fmax="$FMAX" -v fcap="$FREQ_MAX" \
    -v gov="$GOV" -v fe="$FRONTEND" '
BEGIN {
	cpu_s  = (c1 - c0) / hz
	wall_s = w1 - w0
	if (wall_s <= 0) { print "bad sample window"; exit 1 }
	load   = cpu_s / wall_s
	budget = 1000.0 / fps
	per_fr = load * 1000.0 / fps
	favg   = (fn > 0) ? fsum / fn : 0

	printf "sampled      %.2fs wall, %.2fs cpu\n", wall_s, cpu_s
	printf "governor     %s\n", gov
	printf "clock        avg %.0f MHz (min %.0f, max %.0f) of %.0f MHz\n", \
		favg/1000, fmin/1000, fmax/1000, fcap/1000
	printf "load         %.1f%% of one core\n", load * 100
	printf "frame budget %.2f ms at %.4f fps\n", budget, fps
	printf "cost/frame   %.2f ms  (process-wide: frontend + core)\n", per_fr

	# Cost scales with clock, but sub-linearly -- memory bandwidth does not rise
	# with core clock. Measured on this device: 648->1344 MHz (2.07x) gave 1.76x,
	# about 85% efficiency. Projection below applies that, and is still optimistic.
	if (fcap > 0 && favg > 0 && favg < fcap * 0.98) {
		proj = per_fr * (favg / fcap) / 0.85
		printf "cost/frame   ~%.2f ms  (projected at %.0f MHz, OPTIMISTIC -- re-measure pinned)\n", \
			proj, fcap/1000
		printf "\n  clock was %.0f%% of cap; set CPU Speed=Performance and run again.\n", \
			100*favg/fcap
	}

	if (fe > 0) {
		core = per_fr - fe
		printf "\ndecomposed against a %.2f ms frontend:\n", fe
		printf "  core alone       %.2f ms\n", core
		printf "  paired serial    %.2f ms  (frontend + 2x core) vs %.2f: %s\n", \
			fe + 2*core, budget, (fe + 2*core < budget ? "FITS" : "over")
		printf "  paired parallel  %.2f ms  (frontend + 1x core wall) vs %.2f: %s\n", \
			fe + core, budget, (fe + core < budget ? "FITS" : "over")
		printf "                   + barrier cost; needs 2 of this device 4 cores\n"
	} else {
		printf "\n  pass frontend ms as arg 3 to decompose (see header)\n"
	}
}'
