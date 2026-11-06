#!/bin/sh
# Lossy-link A/B: hybrid UDP vs TCP-only input transport, over the same
# shaped loopback link.
#
#   ./lossy.sh
#   LOSS=5 DELAY_MS=40 FRAMES=1200 ./lossy.sh
#
# Six shared-screen sessions run: {hybrid, TCP-only} x {clean, shaped,
# burst}. The shaped runs sit in their own network namespace (unshare
# -rn, no privileges needed) with a netem qdisc on lo - real packet loss
# and delay applied by the kernel, identically to TCP segments and UDP
# datagrams. TCP-only is the same binary with NETLINK_TEST_NO_UDP=1, i.e.
# the pre-R1 transport. Clean runs are the control: shaping, not the
# transport, must be what moves the numbers.
#
# The shaped profile drops packets independently. The burst profile
# uses netem's Gilbert model instead: the link flips between a good
# state (no loss) and a bad state (total loss), averaging ~10% loss in
# bursts of ~11 packets - the shape interference and WiFi power-save
# actually produce, and the one that defeats frame-count redundancy
# windows. Inputs lost outright there must come back via
# CMD_INPUT_REQUEST; the burst hybrid run must still complete.
#
# Printed per run: frames run, frames stalled waiting for peer input
# (summed over both devices), the longest single stall (the gap-length
# measure that decides whether any redundancy window could have worked),
# how many inputs were retransmitted on request, and whether the session
# stayed in sync. The shaped hybrid run must stall less than the shaped
# TCP run - that is the claim R1 makes. Anything else is a failure.
#
# Host-only, like link.sh. Skips the shaped runs (with a note, and still
# runs the clean pair) where unshare/netem is unavailable.

set -u
cd "$(dirname "$0")"

CC="${CC:-cc}"
LOSS="${LOSS:-3}"            # percent, each way, both protocols
DELAY_MS="${DELAY_MS:-25}"   # one-way base delay added by netem
JITTER_MS="${JITTER_MS:-5}"
FRAMES="${FRAMES:-1800}"
FRAME_MS="${FRAME_MS:-5}"
OUT=$(mktemp -d)
PORT=${PORT:-$((42000 + ($$ % 18000)))}
cleanup() {
	if [ -n "${KEEP_TEST_OUTPUT:-}" ]; then echo "test logs kept at $OUT"; else rm -rf "$OUT"; fi
}
trap cleanup EXIT

echo "== building"
(cd .. && make native >/dev/null)
$CC fake_core.c -o "$OUT/fake_libretro.so" -shared -fPIC -I../include -O0 -std=gnu99
$CC harness.c   -o "$OUT/harness"          -I../include -O0 -std=gnu99 -ldl
SHIM=../../bin/native/netplay_shim.so

# One pair of harnesses. $1 = label, $2 = extra env for both sides
# ("" or NETLINK_TEST_NO_UDP=1). Logs land in $OUT/<label>-{host,client}.log.
cat > "$OUT/pair.sh" <<'EOF'
#!/bin/sh
if [ -n "${NETEM:-}" ]; then
	ip link set lo up || exit 3
	tc qdisc add dev lo root netem $NETEM || exit 3
fi
printf 'role=host\nport=%s\nmode=netplay\ninput_delay=8\n' "$PAIR_PORT" > "$PAIR_OUT/$PAIR_LABEL-host.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=netplay\ninput_delay=8\n' "$PAIR_PORT" > "$PAIR_OUT/$PAIR_LABEL-client.session"
env $PAIR_ENV HARNESS_BUTTONS=17 NETPLAY_REAL_CORE="$PAIR_OUT/fake_libretro.so" \
	NETPLAY_SESSION="$PAIR_OUT/$PAIR_LABEL-host.session" \
	"$PAIR_OUT/harness" "$PAIR_SHIM" "$PAIR_FRAMES" "$PAIR_FRAME_MS" \
	> "$PAIR_OUT/$PAIR_LABEL-host.log" 2>&1 &
H=$!
sleep 0.5
env $PAIR_ENV HARNESS_BUTTONS=34 NETPLAY_REAL_CORE="$PAIR_OUT/fake_libretro.so" \
	NETPLAY_SESSION="$PAIR_OUT/$PAIR_LABEL-client.session" \
	"$PAIR_OUT/harness" "$PAIR_SHIM" "$PAIR_FRAMES" "$PAIR_FRAME_MS" \
	> "$PAIR_OUT/$PAIR_LABEL-client.log" 2>&1 &
C=$!
wait $H; echo $? > "$PAIR_OUT/$PAIR_LABEL-host.rc"
wait $C; echo $? > "$PAIR_OUT/$PAIR_LABEL-client.rc"
EOF
chmod +x "$OUT/pair.sh"

run_pair() { # <label> <env> <netem spec or "">
	label=$1; envv=$2; netem=$3
	PAIR_LABEL=$label PAIR_ENV=$envv PAIR_OUT=$OUT PAIR_SHIM=$SHIM \
	PAIR_PORT=$PORT PAIR_FRAMES=$FRAMES PAIR_FRAME_MS=$FRAME_MS \
	NETEM=$netem sh "$OUT/pair.sh"
	PORT=$((PORT + 2))
}

run_shaped_pair() { # <label> <env> <netem loss spec> <harness iterations>
	label=$1; envv=$2; spec=$3; iters=$4
	if command -v unshare >/dev/null && \
	   unshare -rn sh -c 'ip link set lo up && tc qdisc add dev lo root netem loss 0% && tc qdisc del dev lo root' 2>/dev/null; then
		PAIR_LABEL=$label PAIR_ENV=$envv PAIR_OUT=$OUT PAIR_SHIM=$SHIM \
		PAIR_PORT=$PORT PAIR_FRAMES=$iters PAIR_FRAME_MS=$FRAME_MS \
		NETEM="$spec delay ${DELAY_MS}ms ${JITTER_MS}ms" \
			unshare -rn sh "$OUT/pair.sh"
		PORT=$((PORT + 2))
	else
		echo "  SKIP shaped $label: unshare/netem unavailable"
		touch "$OUT/$label-skipped"
	fi
}

echo "== clean link (control)"
run_pair clean-hybrid "" ""
run_pair clean-tcp "NETLINK_TEST_NO_UDP=1" ""

echo "== shaped link: loss ${LOSS}%, delay ${DELAY_MS}ms +/- ${JITTER_MS}ms"
run_shaped_pair lossy-hybrid "" "loss ${LOSS}%" "$FRAMES"
run_shaped_pair lossy-tcp "NETLINK_TEST_NO_UDP=1" "loss ${LOSS}%" "$FRAMES"

echo "== burst link: Gilbert loss (good->bad ${BURST_P:-1}%, bad->good ${BURST_R:-9}%, ~10% in bursts)"
# Burst sessions run at whatever fraction of full speed the link allows;
# give them three times the harness iterations so "completed" below
# measures frames advanced against the same target as every other run.
run_shaped_pair burst-hybrid "" "loss gemodel ${BURST_P:-1}% ${BURST_R:-9}% 100% 0%" "$((FRAMES * 3))"
run_shaped_pair burst-tcp "NETLINK_TEST_NO_UDP=1" "loss gemodel ${BURST_P:-1}% ${BURST_R:-9}% 100% 0%" "$((FRAMES * 3))"

echo
printf '%-14s %8s %9s %9s %9s %6s  %s\n' case frames stalled "stall%" longest reqs result
fail=0
stalled_of() { # <label> -> summed stalled frames, or empty
	s=$(sed -n 's/.*session totals: [0-9]* frames, stalled \([0-9]*\).*/\1/p' \
		"$OUT/$1-host.log" "$OUT/$1-client.log" 2>/dev/null | awk '{t+=$1} END {if (NR) print t}')
	echo "$s"
}
report() { # <label>
	label=$1
	if [ -f "$OUT/$label-skipped" ]; then
		printf '%-14s %8s %9s %9s %9s %6s  %s\n' "$label" - - - - - skipped
		return
	fi
	frames=$(sed -n 's/.*session totals: \([0-9]*\) frames.*/\1/p' "$OUT/$label-host.log" | tail -1)
	stalled=$(stalled_of "$label")
	# Gap length: the longest run of consecutive frames that had to be
	# asked for again, over both sides. Pacing lines only appear every
	# 600 frames, which a stuck session never reaches, so the requested
	# frame numbers themselves are the honest source.
	longest=$(sed -n 's/.*requested missing input frame \([0-9]*\).*/\1/p' "$OUT/$label-host.log" "$OUT/$label-client.log" 2>/dev/null | sort -n -u | awk '{if (NR==1 || $1!=p+1) run=1; else run++; p=$1; if (run>m) m=run} END {print m+0}')
	reqs=$(grep -c "requested missing input frame" "$OUT/$label-host.log" "$OUT/$label-client.log" 2>/dev/null | awk -F: '{t+=$2} END {print t+0}')
	desyncs=$(grep -c "DESYNC" "$OUT/$label-host.log" "$OUT/$label-client.log" 2>/dev/null | awk -F: '{t+=$2} END {print t+0}')
	ports=$(grep -l "core:ports p0=17 p1=34" "$OUT/$label-host.log" "$OUT/$label-client.log" 2>/dev/null | wc -l)
	pct="-"
	[ -n "${frames:-}" ] && [ -n "$stalled" ] && [ "$frames" -gt 0 ] \
		&& pct=$(awk -v s="$stalled" -v f="$frames" 'BEGIN {printf "%.1f", 100*s/(s+2*f)}')
	result=ok
	# A session stuck waiting for a lost input still shows early ports and
	# no DESYNC; it gives itself away by never advancing. Demand most of
	# the scheduled frames actually ran.
	[ "${frames:-0}" -ge $((FRAMES * 3 / 4)) ] && [ "$ports" -eq 2 ] && [ "$desyncs" -eq 0 ] || result=BROKEN
	# burst-tcp is the comparison, not a claim: at ~10% burst loss the
	# pre-R1 transport may legitimately fail to sync at all (its 5 s
	# read deadline fires mid-transfer). Only the hybrid burst run,
	# and every clean/shaped run, must complete.
	[ "$result" = ok ] || [ "$label" = burst-tcp ] || fail=1
	printf '%-14s %8s %9s %9s %9s %6s  %s\n' "$label" "${frames:--}" "${stalled:--}" "$pct" "$longest" "$reqs" "$result"
	var=$(echo "$label" | tr - _)
	eval "STALLED_$var=$stalled"
	eval "RESULT_$var=$result"
	eval "REQS_$var=$reqs"
}
report clean-hybrid
report clean-tcp
report lossy-hybrid
report lossy-tcp
report burst-hybrid
report burst-tcp

echo
if [ -f "$OUT/lossy-hybrid-skipped" ]; then
	echo "shaped runs skipped - no verdict on the transport claim"
	exit $fail
fi
lh=$(eval echo \$STALLED_lossy_hybrid); lt=$(eval echo \$STALLED_lossy_tcp)
if [ -n "$lh" ] && [ -n "$lt" ] && [ "$lt" -gt 0 ] && [ "$lh" -lt "$lt" ]; then
	echo "  ok   shaped link: hybrid stalled $lh frames vs TCP-only $lt"
else
	echo "  MISS shaped link: hybrid stalled ${lh:-?} frames vs TCP-only ${lt:-?}"
	fail=1
fi

if [ ! -f "$OUT/burst-hybrid-skipped" ]; then
	bh=$(eval echo \$RESULT_burst_hybrid); br=$(eval echo \$REQS_burst_hybrid)
	if [ "$bh" = ok ] && [ "${br:-0}" -gt 0 ]; then
		echo "  ok   burst link: hybrid completed, $br input(s) recovered by retransmit"
	else
		echo "  MISS burst link: hybrid result=$bh retransmits=${br:-0} (must complete via retransmit)"
		fail=1
	fi
fi

[ "$fail" -eq 0 ] && echo PASS || echo FAIL
exit $fail
