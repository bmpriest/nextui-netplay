#!/bin/sh
# Two shim instances over loopback, one host one client, each with a core that
# uses the netpacket interface. Asserts the interface is accepted, the session
# starts on both sides with the right client ids, real packets cross in both
# directions, and teardown reaches the core.
#
#   ./link.sh
#
# Host-only. No device, no NextUI, no frontend that knows netplay exists - which
# is the point: a stock minarch would answer SET_NETPACKET_INTERFACE with false
# and the core would disable link entirely.

set -e
cd "$(dirname "$0")"

CC="${CC:-cc}"
OUT=$(mktemp -d)
PORT=${PORT:-$((40000 + ($$ % 20000)))}
cleanup() {
	kill $HOST_PID $CLIENT_PID $MFH_PID $MFC_PID 2>/dev/null || true
	if [ -n "$KEEP_TEST_OUTPUT" ]; then echo "test logs kept at $OUT"; else rm -rf "$OUT"; fi
}
trap cleanup EXIT

echo "== building"
(cd .. && make native >/dev/null)
$CC fake_core.c -o "$OUT/fake_libretro.so" -shared -fPIC -I../include -O0 -std=gnu99
$CC harness.c   -o "$OUT/harness"          -I../include -O0 -std=gnu99 -ldl

SHIM=../../bin/native/netplay_shim.so

printf 'role=host\nport=%s\nmode=link\noption.gpsp_serial=mul_aw2\n' "$PORT" > "$OUT/host.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=link\n' "$PORT" > "$OUT/client.session"

echo "== running host and client for ~3s"
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/host.session" \
	"$OUT/harness" "$SHIM" 200 15 > "$OUT/host.log" 2>&1 &
HOST_PID=$!

sleep 0.5   # let the host bind before the client dials

NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/client.session" \
	"$OUT/harness" "$SHIM" 200 15 > "$OUT/client.log" 2>&1 &
CLIENT_PID=$!

HOST_RC=0;   wait $HOST_PID   || HOST_RC=$?
CLIENT_RC=0; wait $CLIENT_PID || CLIENT_RC=$?

fail=0
expect() { # <file> <pattern> <description>
	if grep -q "$2" "$1"; then
		echo "  ok   $3"
	else
		echo "  MISS $3"
		fail=1
	fi
}

echo
echo "== interface"
expect "$OUT/host.log"   "core:netpacket_accepted" "host: core's interface accepted"
expect "$OUT/client.log" "core:netpacket_accepted" "client: core's interface accepted"

echo
echo "== session"
# libretro assigns the host client_id 0; the other side must see the peer's id.
expect "$OUT/host.log"   "core:np_start id=0"      "host: started as client_id 0"
expect "$OUT/client.log" "core:np_start id=1"      "client: started as client_id 1"
expect "$OUT/host.log"   "core:np_connected id=1"  "host: told the client joined"
expect "$OUT/client.log" "core:np_connected id=0"  "client: told the host is there"

echo
echo "== data actually crossed"
# The client sends P1-*, so seeing it in the host's log means it went out over
# TCP, through the shim, and into the peer core's receive callback.
expect "$OUT/host.log"   "core:np_recv from=1 .*data=P1-" "host received the client's packets"
expect "$OUT/client.log" "core:np_recv from=0 .*data=P0-" "client received the host's packets"

HOST_RX=$(grep -c "core:np_recv" "$OUT/host.log" || true)
CLIENT_RX=$(grep -c "core:np_recv" "$OUT/client.log" || true)
echo "  ..   host received $HOST_RX packets, client received $CLIENT_RX"
[ "$HOST_RX" -gt 10 ] && [ "$CLIENT_RX" -gt 10 ] \
	&& echo "  ok   sustained both directions" \
	|| { echo "  MISS expected a sustained stream"; fail=1; }

echo
echo "== core option forced by the session"
# gpSP link modes are per-game; both peers must agree, and the frontend's own
# value (typically "auto") must not win.
expect "$OUT/host.log"   "core:option gpsp_serial=mul_aw2" "host: shim answered the option"
expect "$OUT/client.log" "core:option gpsp_serial=<unset>" "client: no override, frontend answers"

echo
echo "== teardown"
expect "$OUT/host.log"   "core:np_stop" "host: core told the session ended"
expect "$OUT/client.log" "core:np_stop" "client: core told the session ended"

echo
echo "== no packets dropped"
if grep -q "were dropped" "$OUT/host.log" "$OUT/client.log"; then
	grep -h "were dropped" "$OUT/host.log" "$OUT/client.log" | sed 's/^/  WARN /'
	fail=1
else
	echo "  ok   queue never overflowed"
fi

echo
echo "== harness assertions still pass under a live link"
[ "$HOST_RC" -eq 0 ] && echo "  ok   host exited 0"   || { echo "  MISS host rc=$HOST_RC"; fail=1; }
[ "$CLIENT_RC" -eq 0 ] && echo "  ok   client exited 0" || { echo "  MISS client rc=$CLIENT_RC"; fail=1; }

echo
echo "== a stalled frontend must pause its peer, not overflow it"
# The menu case seen on device: one side stops calling retro_run, the other keeps
# running and fills a queue nobody drains. The peer should be told to hold.
PORT3=$((PORT + 2))
printf 'role=host\nport=%s\nmode=link\n'                  "$PORT3" > "$OUT/h3.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=link\n' "$PORT3" > "$OUT/c3.session"

NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h3.session" \
	"$OUT/harness" "$SHIM" 600 15 > "$OUT/h3.log" 2>&1 &
H3=$!
sleep 0.5
HARNESS_STALL_AT=150 HARNESS_STALL_MS=4000 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c3.session" \
	"$OUT/harness" "$SHIM" 600 15 > "$OUT/c3.log" 2>&1 &
C3=$!
wait $H3 2>/dev/null || true
wait $C3 2>/dev/null || true

expect "$OUT/c3.log" "fe:stall_begin"                  "client frontend stalled"
expect "$OUT/c3.log" "frontend stalled - told peer"    "client announced the stall"
expect "$OUT/h3.log" "peer paused"                     "host was told to hold"
expect "$OUT/h3.log" "peer resumed"                    "host was released"
expect "$OUT/c3.log" "frontend resumed - told peer"    "client announced recovery"

if grep -q "were dropped" "$OUT/h3.log" "$OUT/c3.log"; then
	grep -h "were dropped" "$OUT/h3.log" "$OUT/c3.log" | sed 's/^/  WARN /'
	fail=1
else
	echo "  ok   a 4s stall cost no packets"
fi

# The host is frame-skipping for ~4s. If the shim did not present anything, the
# screen would just freeze - visually identical to a crash.
h3_video=$(sed -n 's/.*fe:totals video=\([0-9]*\).*/\1/p' "$OUT/h3.log")
h3_audio=$(sed -n 's/.*audio=\([0-9]*\).*/\1/p' "$OUT/h3.log")
[ "${h3_video:-0}" -ge 590 ] \
	&& echo "  ok   host kept presenting while paused ($h3_video presented)" \
	|| { echo "  MISS host stopped presenting ($h3_video frames of 600)"; fail=1; }
[ "${h3_audio:-0}" -ge 590 ] \
	&& echo "  ok   host kept feeding audio while paused ($h3_audio batches)" \
	|| { echo "  MISS audio gap while paused ($h3_audio batches of 600)"; fail=1; }

h3_input=$(sed -n 's/.*input=\([0-9]*\).*/\1/p' "$OUT/h3.log")
# Input is polled only from inside core.run(). Skipping it without polling
# manually leaves the frontend unable to see any button, including MENU.
[ "${h3_input:-0}" -ge 590 ] \
	&& echo "  ok   host kept polling input while paused ($h3_input polls)" \
	|| { echo "  MISS input unpolled while paused ($h3_input of 600) - UI would be frozen"; fail=1; }

echo
echo "== a peer that quits while paused must not strand us"
# The device bug: peer opens a menu (we pause), then quits. CMD_RESUME never
# comes, so we waited forever with a dead UI.
PORT4=$((PORT + 3))
printf 'role=host\nport=%s\nmode=link\n'                  "$PORT4" > "$OUT/h4.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=link\n' "$PORT4" > "$OUT/c4.session"

NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h4.session" \
	"$OUT/harness" "$SHIM" 700 15 > "$OUT/h4.log" 2>&1 &
H4=$!
sleep 0.5
# Client stalls, then exits while still stalled - never sending CMD_RESUME.
HARNESS_STALL_AT=100 HARNESS_STALL_MS=1500 HARNESS_DIE_IN_STALL=1 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c4.session" \
	"$OUT/harness" "$SHIM" 700 15 > "$OUT/c4.log" 2>&1 &
C4=$!
wait $C4 2>/dev/null || true
wait $H4 2>/dev/null || true

expect "$OUT/c4.log" "fe:died_in_stall" "client quit while still paused"
expect "$OUT/h4.log" "peer paused"       "host paused as expected"
h4_runs=$(grep -c "^core:run$" "$OUT/h4.log" || true)
# Count core runs, not presented frames: the shim presents held frames while
# paused, so video would reach 700 even if the core never advanced again.
# Allow for the ~100 frames legitimately paused before the peer vanished; a
# stranded host stops near 150 and never recovers.
[ "${h4_runs:-0}" -ge 550 ] \
	&& echo "  ok   host resumed after the peer vanished ($h4_runs core runs)" \
	|| { echo "  MISS host stranded at $h4_runs of 700 core runs"; fail=1; }
grep -q "RESULT: ok" "$OUT/h4.log" \
	&& echo "  ok   host completed its run" || { echo "  MISS host never finished"; fail=1; }

echo
echo "== a core blocked inside retro_run is not a stalled frontend"
# The GB link deadlock: gambatte blocks in retro_run waiting on its peer. Read
# as a frontend stall, that pauses the peer - removing the data the blocked core
# is waiting for, so neither side can proceed.
PORT5=$((PORT + 4))
printf 'role=host\nport=%s\nmode=link\n'                  "$PORT5" > "$OUT/h5.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=link\n' "$PORT5" > "$OUT/c5.session"

NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h5.session" \
	"$OUT/harness" "$SHIM" 500 15 > "$OUT/h5.log" 2>&1 &
H5=$!
sleep 0.5
FAKE_CORE_BLOCK_AT=100 FAKE_CORE_BLOCK_MS=3000 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c5.session" \
	"$OUT/harness" "$SHIM" 500 15 > "$OUT/c5.log" 2>&1 &
C5=$!
wait $H5 2>/dev/null || true
wait $C5 2>/dev/null || true

expect "$OUT/c5.log" "core:block_begin" "client core blocked inside retro_run"
if grep -q "frontend stalled" "$OUT/c5.log"; then
	echo "  MISS a blocked core was reported as a frontend stall"
	fail=1
else
	echo "  ok   blocked core not mistaken for a stall"
fi
if grep -q "peer paused" "$OUT/h5.log"; then
	echo "  MISS host paused while its peer was mid-frame"
	fail=1
else
	echo "  ok   host never paused"
fi

echo
echo "== a genuinely blocked core call trips the starvation watchdog"
# Queue silence is normal in menus, but one retro_run call which does not return
# is an actual core-side failure. Lower the field threshold for this regression.
PORT19=$((PORT + 18))
printf 'role=host\nport=%s\nmode=link\n'                  "$PORT19" > "$OUT/h19.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=link\n' "$PORT19" > "$OUT/c19.session"

NETPLAY_LINK_STARVE_MS=500 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h19.session" \
	"$OUT/harness" "$SHIM" 250 10 > "$OUT/h19.log" 2>&1 &
H19=$!
sleep 0.3
NETPLAY_LINK_STARVE_MS=500 FAKE_CORE_BLOCK_AT=100 FAKE_CORE_BLOCK_MS=1000 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c19.session" \
	"$OUT/harness" "$SHIM" 250 10 > "$OUT/c19.log" 2>&1 &
C19=$!
wait $H19 2>/dev/null || true
wait $C19 2>/dev/null || true

expect "$OUT/c19.log" "link starved: one core call blocked" "blocked call was identified"
expect "$OUT/c19.log" "Other player's game stopped sending link data" "blocked call reached recovery UI"
if grep -q "link starved:" "$OUT/h19.log"; then
	echo "  MISS normally running host tripped the watchdog"
	fail=1
else
	echo "  ok   normally running host did not trip the watchdog"
fi

echo
echo "== menu still pauses the peer for a core that owns its own link"
# gambatte never registers netpacket, but a menu on one device should still hold
# the other - otherwise it sits blocked on a serial read with no idea why.
PORT6=$((PORT + 5))
printf 'role=host\nport=%s\nmode=link\n'                  "$PORT6" > "$OUT/h6.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=link\n' "$PORT6" > "$OUT/c6.session"

FAKE_CORE_NO_NETPACKET=1 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h6.session" \
	"$OUT/harness" "$SHIM" 500 15 > "$OUT/h6.log" 2>&1 &
H6=$!
sleep 0.5
FAKE_CORE_NO_NETPACKET=1 HARNESS_STALL_AT=120 HARNESS_STALL_MS=2500 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c6.session" \
	"$OUT/harness" "$SHIM" 500 15 > "$OUT/c6.log" 2>&1 &
C6=$!
wait $H6 2>/dev/null || true
wait $C6 2>/dev/null || true

expect "$OUT/c6.log" "core:netpacket_skipped"      "core carries its own link"
expect "$OUT/c6.log" "frontend stalled - told peer" "menu still announced"
expect "$OUT/h6.log" "peer paused: frontend"        "peer paused, with a reason"
expect "$OUT/h6.log" "peer resumed"                 "peer released"

echo
echo "== shared-screen netplay: both ports served, from both devices"
# One instance each, same game, inputs synced. Distinct from link play: the
# cores exchange nothing themselves, the shim carries the inputs.
PORT7=$((PORT + 6))
printf 'role=host\nport=%s\nmode=netplay\n'                  "$PORT7" > "$OUT/h7.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=netplay\n' "$PORT7" > "$OUT/c7.session"

FAKE_CORE_NAME=PCSX-ReARMed FAKE_SRAM_BYTE=17 FAKE_RTC_BYTE=18 \
	HARNESS_EXPECT_CORE_NAME=PCSX-ReARMed HARNESS_BUTTONS=17 \
	NETPLAY_COMPAT_CORE=1 HARNESS_EXPECT_NO_STATES=1 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h7.session" \
	"$OUT/harness" "$SHIM" 400 10 > "$OUT/h7.log" 2>&1 &
H7=$!
sleep 0.5
FAKE_CORE_NAME=PCSX-ReARMed FAKE_SRAM_BYTE=99 FAKE_RTC_BYTE=100 \
	HARNESS_EXPECT_CORE_NAME=PCSX-ReARMed HARNESS_BUTTONS=34 \
	NETPLAY_COMPAT_CORE=1 HARNESS_EXPECT_NO_STATES=1 \
	HARNESS_EXPECT_NO_PERSISTENCE=1 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c7.session" \
	"$OUT/harness" "$SHIM" 400 10 > "$OUT/c7.log" 2>&1 &
C7=$!
wait $H7 2>/dev/null || true
wait $C7 2>/dev/null || true

expect "$OUT/h7.log" "authoritative initial state sent" "host shipped its state"
expect "$OUT/c7.log" "authoritative resync .* adopted"  "client adopted it"
expect "$OUT/h7.log" "RESULT: ok" "frontend states blocked without breaking netplay state sync"
expect "$OUT/c7.log" "RESULT: ok" "guest SRAM and RTC are hidden from frontend persistence"
expect "$OUT/h7.log" "core:option pcsx_rearmed_memcard1=libretro" "PCSX card 1 is frontend-managed"
expect "$OUT/h7.log" "core:option pcsx_rearmed_memcard2=none" "PCSX card 2 is disabled for the session"
expect "$OUT/c7.log" "core:persistent sram=17 rtc=18" "guest adopted host raw SRAM and RTC"
expect "$OUT/h7.log" "core:save_directory=/tmp$" "host core keeps the frontend save directory"
expect "$OUT/c7.log" "core:save_directory=/tmp/netplay-guest-" "guest core-managed saves are volatile"

# Host holds 17, client holds 34. Both must see the same pairing: p0 is the
# host's input, p1 the client's, on both devices.
for side in h7 c7; do
	if grep -q "core:ports p0=17 p1=34" "$OUT/$side.log"; then
		echo "  ok   $side sees p0=host p1=client"
	else
		echo "  MISS $side port mapping wrong"
		grep -m2 "core:ports" "$OUT/$side.log" | sed 's/^/       /'
		fail=1
	fi
done

# Port 1 non-zero can only have come over the wire.
grep -q "core:ports p0=17 p1=34" "$OUT/h7.log" && grep -q "core:ports p0=17 p1=34" "$OUT/c7.log" \
	&& echo "  ok   second player arrived over the network" \
	|| { echo "  MISS second player never arrived"; fail=1; }

if grep -q "DESYNC" "$OUT/h7.log" "$OUT/c7.log"; then
	grep -h "DESYNC" "$OUT/h7.log" "$OUT/c7.log" | sed 's/^/  WARN /'
	fail=1
else
	echo "  ok   no divergence reported"
fi

echo
echo "== a desync is recovered from the host's authoritative state"
PORT11=$((PORT + 10))
printf 'role=host\nport=%s\nmode=netplay\n'                    "$PORT11" > "$OUT/h11.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=netplay\n' "$PORT11" > "$OUT/c11.session"

HARNESS_BUTTONS=17 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/h11.session" \
	"$OUT/harness" "$SHIM" 1000 5 > "$OUT/h11.log" 2>&1 &
H11=$!
sleep 0.5
FAKE_CORE_CORRUPT_AT=80 HARNESS_BUTTONS=34 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c11.session" \
	"$OUT/harness" "$SHIM" 1000 5 > "$OUT/c11.log" 2>&1 &
C11=$!
wait $H11 2>/dev/null || true
wait $C11 2>/dev/null || true

expect "$OUT/c11.log" "core:state_corrupted" "client state was deliberately corrupted"
expect "$OUT/c11.log" "DESYNC at frame" "client detected the divergence"
expect "$OUT/c11.log" "requested authoritative state" "client requested recovery"
expect "$OUT/h11.log" "authoritative resync .* sent" "host sent an authoritative snapshot"
expect "$OUT/c11.log" "authoritative resync .* adopted" "client adopted the snapshot"
expect "$OUT/h11.log" "authoritative resync .* committed" "host committed the recovered timeline"
expect "$OUT/c11.log" "authoritative resync .* committed" "client resumed only after commit"

desyncs=$(grep -c "DESYNC at frame" "$OUT/c11.log" || true)
if [ "$desyncs" -eq 1 ] && grep -Eq "in sync at frame (900|[1-9][0-9]{3,})" "$OUT/c11.log"; then
	echo "  ok   later checkpoint is synchronized after one recovery"
else
	echo "  MISS recovery did not stay synchronized (desyncs=$desyncs)"
	grep -E "DESYNC|resync|in sync" "$OUT/c11.log" | sed 's/^/       /'
	fail=1
fi

echo
echo "== shared-screen reset is host-authoritative"
PORT16=$((PORT + 15))
printf 'role=host\nport=%s\nmode=netplay\n'                    "$PORT16" > "$OUT/h16.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=netplay\n' "$PORT16" > "$OUT/c16.session"

HARNESS_RESET_AT=260 HARNESS_BUTTONS=17 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/h16.session" \
	"$OUT/harness" "$SHIM" 1100 5 > "$OUT/h16.log" 2>&1 &
H16=$!
sleep 0.3
HARNESS_RESET_AT=180 HARNESS_BUTTONS=34 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/c16.session" \
	"$OUT/harness" "$SHIM" 1100 5 > "$OUT/c16.log" 2>&1 &
C16=$!
wait $H16 2>/dev/null || true
wait $C16 2>/dev/null || true

expect "$OUT/c16.log" "guest reset rejected" "guest reset was rejected visibly"
expect "$OUT/h16.log" "core:reset" "host alone reset its core"
if grep -q "core:reset" "$OUT/c16.log"; then
	echo "  MISS guest core was reset locally"
	fail=1
else
	echo "  ok   guest core was not reset locally"
fi
expect "$OUT/h16.log" "authoritative reset .* sent" "host sent its post-reset state"
expect "$OUT/c16.log" "authoritative reset .* adopted" "guest adopted the host reset"
expect "$OUT/h16.log" "authoritative reset .* committed" "host committed reset after guest ACK"
expect "$OUT/c16.log" "authoritative reset .* committed" "guest resumed only after reset commit"
if grep -q "DESYNC" "$OUT/h16.log" "$OUT/c16.log"; then
	echo "  MISS authoritative reset caused a later desync"
	fail=1
else
	echo "  ok   reset timeline stayed synchronized"
fi

echo
echo "== a restarted guest rejoins the live host"
PORT12=$((PORT + 11))
SID12=$(printf '%032x' "$PORT12")
printf 'role=host\nport=%s\nmode=netplay\nsession_id=%s\n' "$PORT12" "$SID12" > "$OUT/h12.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=netplay\nsession_id=%s\n' "$PORT12" "$SID12" > "$OUT/c12.session"

HARNESS_BUTTONS=17 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h12.session" \
	"$OUT/harness" "$SHIM" 1700 5 > "$OUT/h12.log" 2>&1 &
H12=$!
sleep 0.3
HARNESS_DIE_AT=450 HARNESS_BUTTONS=34 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/c12.session" "$OUT/harness" "$SHIM" 900 5 > "$OUT/c12a.log" 2>&1 &
C12=$!
wait $C12 2>/dev/null || true
HARNESS_BUTTONS=34 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c12.session" \
	"$OUT/harness" "$SHIM" 900 5 > "$OUT/c12b.log" 2>&1 &
C12=$!
wait $H12 2>/dev/null || true
wait $C12 2>/dev/null || true

expect "$OUT/c12a.log" "fe:process_crash" "first guest process crashed"
expect "$OUT/h12.log" "authoritative live state sent for connection 2" "live host sent current state to replacement guest"
expect "$OUT/c12b.log" "ROM and core identity match peer" "replacement guest identity was validated"
expect "$OUT/c12b.log" "authoritative resync .* committed" "replacement guest rejoined"
expect "$OUT/c12b.log" "core:ports p0=17 p1=34" "replacement guest resumed shared inputs"

echo
echo "== a restarted host rewinds both peers to its confirmed checkpoint"
PORT13=$((PORT + 12))
SID13=$(printf '%032x' "$PORT13")
printf 'role=host\nport=%s\nmode=netplay\nsession_id=%s\n' "$PORT13" "$SID13" > "$OUT/h13.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=netplay\nsession_id=%s\n' "$PORT13" "$SID13" > "$OUT/c13.session"

HARNESS_DIE_AT=700 HARNESS_BUTTONS=17 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/h13.session" "$OUT/harness" "$SHIM" 1200 5 > "$OUT/h13a.log" 2>&1 &
H13=$!
sleep 0.3
HARNESS_BUTTONS=34 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c13.session" \
	"$OUT/harness" "$SHIM" 2000 5 > "$OUT/c13.log" 2>&1 &
C13=$!
wait $H13 2>/dev/null || true
HARNESS_BUTTONS=17 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h13.session" \
	"$OUT/harness" "$SHIM" 1100 5 > "$OUT/h13b.log" 2>&1 &
H13=$!
wait $H13 2>/dev/null || true
wait $C13 2>/dev/null || true

expect "$OUT/h13a.log" "confirmed host checkpoint promoted at frame [1-9][0-9]*" "guest confirmed a periodic checkpoint"
expect "$OUT/h13a.log" "fe:process_crash" "first host process crashed"
expect "$OUT/h13b.log" "restored last confirmed host checkpoint from frame [1-9][0-9]*" "replacement host loaded only the confirmed checkpoint"
expect "$OUT/h13b.log" "authoritative checkpoint state sent" "replacement host remained authoritative"
expect "$OUT/c13.log" "connection 2 requires an authoritative sync" "surviving guest detected replacement host"
expect "$OUT/c13.log" "authoritative resync .* committed" "surviving guest adopted restored host state"

echo
echo "== a slow first echo does not size the input delay"
# The first probe of a connection can be answered late by a peer still loading
# its game. On hardware that one sample (280-800 ms) used to set 20 frames.
PORT32=$((PORT + 31))
printf 'role=host\nport=%s\nmode=netplay\ninput_delay=3\ninput_delay_auto=1\n' "$PORT32" > "$OUT/h32.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=netplay\ninput_delay=3\ninput_delay_auto=1\n' "$PORT32" > "$OUT/c32.session"
NETLINK_TEST_SLOW_FIRST_RTT_MS=600 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/h32.session" "$OUT/harness" "$SHIM" 400 5 > "$OUT/h32.log" 2>&1 &
H32=$!
sleep 0.2
NETLINK_TEST_SLOW_FIRST_RTT_MS=600 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/c32.session" "$OUT/harness" "$SHIM" 400 5 > "$OUT/c32.log" 2>&1 &
C32=$!
wait $H32 2>/dev/null || true
wait $C32 2>/dev/null || true
for side in h32 c32; do
	n=$(sed -n 's/.*link rtt over \([0-9]*\) samples.*/\1/p' "$OUT/$side.log" | head -1)
	[ -n "$n" ] && [ "$n" -ge 3 ] \
		&& echo "  ok   $side proposed from $n samples, not the first alone" \
		|| { echo "  MISS $side proposed from '${n:-no}' samples"; fail=1; }
	delay=$(sed -n 's/.*agreed input delay \([0-9]*\).*/\1/p' "$OUT/$side.log" | tail -1)
	[ "$delay" = "3" ] \
		&& echo "  ok   $side kept its 3-frame delay" \
		|| { echo "  MISS $side negotiated delay ${delay:-none} instead of 3"; fail=1; }
done

echo
echo "== a checkpoint the relaunched core cannot load is discarded, not fatal"
PORT31=$((PORT + 30))
SID31=$(printf '%032x' "$PORT31")
printf 'role=host\nport=%s\nmode=netplay\nsession_id=%s\n' "$PORT31" "$SID31" > "$OUT/h31.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=netplay\nsession_id=%s\n' "$PORT31" "$SID31" > "$OUT/c31.session"
FAKE_NO_SAVE_RAM=1 HARNESS_BUTTONS=17 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/h31.session" "$OUT/harness" "$SHIM" 700 5 > "$OUT/h31a.log" 2>&1 &
H31=$!
sleep 0.3
FAKE_NO_SAVE_RAM=1 HARNESS_BUTTONS=34 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/c31.session" "$OUT/harness" "$SHIM" 700 5 > "$OUT/c31a.log" 2>&1 &
C31=$!
wait $H31 2>/dev/null || true
wait $C31 2>/dev/null || true
HARNESS_BUTTONS=17 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/h31.session" "$OUT/harness" "$SHIM" 400 5 > "$OUT/h31b.log" 2>&1 &
H31=$!
sleep 0.3
HARNESS_BUTTONS=34 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/c31.session" "$OUT/harness" "$SHIM" 400 5 > "$OUT/c31b.log" 2>&1 &
C31=$!
wait $H31 2>/dev/null || true
wait $C31 2>/dev/null || true
expect "$OUT/h31a.log" "confirmed host checkpoint promoted at frame [1-9][0-9]*" "first run left a checkpoint"
expect "$OUT/h31b.log" "state rejected: sizes" "relaunched host named the mismatch"
expect "$OUT/h31b.log" "discarded unusable host checkpoint" "relaunched host dropped the checkpoint"
expect "$OUT/h31b.log" "authoritative initial state sent" "relaunched host synchronized live state"
expect "$OUT/c31b.log" "authoritative resync .* committed" "relaunched guest joined"
if grep -q "could not adopt synchronization state" "$OUT/h31b.log"; then
	echo "  FAIL relaunched host still failed the barrier"; fail=1
else
	echo "  ok   no barrier failure after the discarded checkpoint"
fi
if [ "$(grep -c 'state rejected' "$OUT/h31b.log")" -le 1 ]; then
	echo "  ok   the rejection was not retried every frame"
else
	echo "  FAIL rejection repeated $(grep -c 'state rejected' "$OUT/h31b.log") times"; fail=1
fi

echo
echo "== ROM-content mismatch is rejected before state loading"
PORT14=$((PORT + 13))
printf 'role=host\nport=%s\nmode=netplay\n' "$PORT14" > "$OUT/h14.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=netplay\n' "$PORT14" > "$OUT/c14.session"
HARNESS_ROM_ID=rom-a NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h14.session" \
	"$OUT/harness" "$SHIM" 400 5 > "$OUT/h14.log" 2>&1 &
H14=$!
sleep 0.2
HARNESS_ROM_ID=rom-b NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c14.session" \
	"$OUT/harness" "$SHIM" 400 5 > "$OUT/c14.log" 2>&1 &
C14=$!
wait $H14 2>/dev/null || true
wait $C14 2>/dev/null || true
expect "$OUT/h14.log" "refusing peer: ROM content hashes differ" "host rejected wrong ROM"
expect "$OUT/c14.log" "refusing peer: ROM content hashes differ" "guest rejected wrong ROM"
if grep -q "core:run" "$OUT/h14.log" "$OUT/c14.log"; then
	echo "  MISS a core advanced despite ROM mismatch"; fail=1
else
	echo "  ok   no emulator frame advanced after rejection"
fi

echo
echo "== a stall must not change an input already sent"
# The Streets of Rage desync: a stall returned without advancing the frame, but
# re-sampled and re-sent input for the same frame. If the peer had committed the
# first value, the two sides ran that frame from different inputs.
PORT8=$((PORT + 7))
# input_delay=1 leaves almost no slack, so ordinary scheduling jitter produces
# frequent input stalls on both sides - the condition the bug needs.
printf 'role=host\nport=%s\nmode=netplay\ninput_delay=1\n'                  "$PORT8" > "$OUT/h8.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=netplay\ninput_delay=1\n' "$PORT8" > "$OUT/c8.session"

# Stalls must happen *inside* retro_run - a frontend stall stops it being called
# at all and never re-samples. A one-frame delay window plus inputs that change
# every frame is what makes a re-sample produce a different value for a frame
# the peer may already have committed.
HARNESS_VARY_INPUT=1 HARNESS_BUTTONS=100 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h8.session" \
	"$OUT/harness" "$SHIM" 1500 2 > "$OUT/h8.log" 2>&1 &
H8=$!
sleep 0.5
HARNESS_VARY_INPUT=1 HARNESS_BUTTONS=200 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c8.session" \
	"$OUT/harness" "$SHIM" 1500 2 > "$OUT/c8.log" 2>&1 &
C8=$!
wait $H8 2>/dev/null || true
wait $C8 2>/dev/null || true

# Compare the input checksum at the last frame count both sides reached.
sed -n 's/.*core:inputsum frames=\([0-9]*\).*/\1/p' "$OUT/h8.log" | sort -n | uniq > "$OUT/h8.frames"
sed -n 's/.*core:inputsum frames=\([0-9]*\).*/\1/p' "$OUT/c8.log" | sort -n | uniq > "$OUT/c8.frames"
common=$(comm -12 "$OUT/h8.frames" "$OUT/c8.frames" | tail -1)
if [ -n "$common" ]; then
	hsum=$(grep "core:inputsum frames=$common " "$OUT/h8.log" | tail -1 | grep -oE "sum=[0-9a-f]+")
	csum=$(grep "core:inputsum frames=$common " "$OUT/c8.log" | tail -1 | grep -oE "sum=[0-9a-f]+")
	if [ "$hsum" = "$csum" ] && [ -n "$hsum" ]; then
		echo "  ok   both ran identical inputs through $common frames ($hsum)"
	else
		echo "  MISS input streams diverged at frame $common (host $hsum, client $csum)"
		fail=1
	fi
else
	echo "  MISS no common frame count to compare"
	fail=1
fi

# Pacing has to be in the log or a slow session cannot be attributed after the
# fact - which is exactly the hole that made an on-device report undiagnosable.
# The host must adopt the state it sends. A core whose freshly-booted state is
# not a serialize/unserialize fixpoint leaves the two sides one byte apart from
# frame 0 otherwise, and every divergence check then reports DESYNC for a
# session running perfectly in step.
if grep -q "DESYNC" "$OUT/c7.log" 2>/dev/null; then
	echo "  MISS client reported DESYNC in a lockstep session"
	grep -m2 DESYNC "$OUT/c7.log" | sed 's/^/       /'
	fail=1
else
	echo "  ok   no false DESYNC across the handshake"
fi

expect "$OUT/h8.log" "pacing: .* fps.*stalled" "host reported pacing"
expect "$OUT/c8.log" "pacing: .* fps.*stalled" "client reported pacing"

echo
echo "== one-sided traffic must not starve the heartbeat"
# The regression that shipped: the heartbeat lived in the poll()-timeout branch,
# so a peer sending steadily kept POLLIN set, the idle branch never ran, and the
# quiet side got timed out mid-session. Both sides sending every frame - as the
# test above does - hides it completely.
PORT2=$((PORT + 1))
printf 'role=host\nport=%s\nmode=link\n'                  "$PORT2" > "$OUT/h2.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=link\n' "$PORT2" > "$OUT/c2.session"

# Host talks constantly; client stays silent for well over the 5s timeout.
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h2.session" \
	"$OUT/harness" "$SHIM" 700 15 > "$OUT/h2.log" 2>&1 &
H2=$!
sleep 0.5
FAKE_CORE_QUIET=1 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c2.session" \
	"$OUT/harness" "$SHIM" 700 15 > "$OUT/c2.log" 2>&1 &
C2=$!
wait $H2 2>/dev/null || true
wait $C2 2>/dev/null || true

for side in h2 c2; do
	if grep -q "silence timeout" "$OUT/$side.log"; then
		echo "  MISS $side timed out despite an active peer"
		grep "peer lost" "$OUT/$side.log" | sed 's/^/       /'
		fail=1
	else
		echo "  ok   $side stayed connected"
	fi
	if grep -q "link starved:\|Other player's game stopped sending link data" "$OUT/$side.log"; then
		echo "  MISS $side treated a normally running quiet core as blocked"
		grep -E "link starved:|Other player's game" "$OUT/$side.log" | sed 's/^/       /'
		fail=1
	else
		echo "  ok   $side allowed normal zero-packet frames"
	fi
done
# One clean teardown each at the end is expected; more means it flapped.
flaps=$(grep -c "netpacket session started" "$OUT/c2.log" || true)
[ "$flaps" -eq 1 ] && echo "  ok   session never flapped" \
	|| { echo "  MISS session restarted $flaps times"; fail=1; }

echo
echo "== a session still waiting for its peer must present something"
# The device bug: shared-screen netplay gates ahead of the first core.run(), so
# there is no last frame to dim and the shim presented nothing at all. The
# frontend kept showing black, which is indistinguishable from a hang - the game
# "never loaded" while the menu still worked.
PORT10=$((PORT + 9))
printf 'role=host\nport=%s\nmode=netplay\n' "$PORT10" > "$OUT/w1.session"
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/w1.session" \
	NETPLAY_NEGOTIATE_TIMEOUT_MS=100 \
	"$OUT/harness" "$SHIM" 120 15 > "$OUT/w1.log" 2>&1 || true

w1_video=$(sed -n 's/.*fe:totals video=\([0-9]*\).*/\1/p' "$OUT/w1.log")
w1_run=$(grep -c "core:run" "$OUT/w1.log" || true)
[ "${w1_video:-0}" -ge 110 ] \
	&& echo "  ok   kept presenting while waiting ($w1_video frames)" \
	|| { echo "  MISS presented $w1_video of 120 - screen would be black"; fail=1; }
# The frame it presents must be the waiting message, not a stale game frame.
[ "${w1_run:-0}" -eq 0 ] \
	&& echo "  ok   core never ran without a peer" \
	|| { echo "  MISS core ran $w1_run times unsynced"; fail=1; }

echo
echo "== an unavailable shared-screen peer can continue solo"
PORT14=$((PORT + 13))
printf 'role=host\nport=%s\nmode=netplay\n' "$PORT14" > "$OUT/solo.session"
HARNESS_BUTTONS=256 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/solo.session" NETPLAY_PEER_TIMEOUT_MS=100 \
	"$OUT/harness" "$SHIM" 100 10 > "$OUT/solo.log" 2>&1 || true
expect "$OUT/solo.log" "user continued this game solo" "A chose process-local solo play"
solo_runs=$(grep -c "core:run" "$OUT/solo.log" || true)
[ "$solo_runs" -gt 20 ] \
	&& echo "  ok   local game resumed ($solo_runs frames)" \
	|| { echo "  MISS local game only ran $solo_runs frames"; fail=1; }
expect "$OUT/solo.log" "RESULT: ok" "session persistence restrictions remained active"

echo
echo "== a failed shared-screen session can explicitly retry"
PORT17=$((PORT + 16))
printf 'role=host\nport=%s\nmode=netplay\n' "$PORT17" > "$OUT/retry.session"
# B is the direct Wait/retry action even though Continue solo is highlighted by
# default. Keeping it held also verifies that edge detection prevents a retry
# loop once the second wait times out.
HARNESS_BUTTONS=1 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/retry.session" NETPLAY_PEER_TIMEOUT_MS=100 \
	"$OUT/harness" "$SHIM" 80 10 > "$OUT/retry.log" 2>&1 || true
expect "$OUT/retry.log" "user chose to wait and retry" "B restarted peer discovery"
retry_runs=$(grep -c "core:run" "$OUT/retry.log" || true)
[ "$retry_runs" -eq 0 ] \
	&& echo "  ok   retry did not advance the game without a peer" \
	|| { echo "  MISS retry advanced $retry_runs game frames without a peer"; fail=1; }

echo
# Two differently configured peers must still end on one number, because each
# side primes its own first input_delay frames with neutral input and a mismatch
# diverges during priming and never recovers. Refusing the pair enforced that by
# refusing to play; adopting the higher of the two proposals enforces it while
# still playing, and is order-independent so it needs no extra round trip.
echo "== differently configured peers converge on one input delay"
PORT15=$((PORT + 14))
printf 'role=host\nport=%s\nmode=netplay\ninput_delay=3\n' "$PORT15" > "$OUT/h15.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=netplay\ninput_delay=10\n' "$PORT15" > "$OUT/c15.session"
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h15.session" \
	"$OUT/harness" "$SHIM" 400 5 > "$OUT/h15.log" 2>&1 &
H15=$!
sleep 0.2
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c15.session" \
	"$OUT/harness" "$SHIM" 400 5 > "$OUT/c15.log" 2>&1 &
C15=$!
wait $H15 2>/dev/null || true
wait $C15 2>/dev/null || true
expect "$OUT/h15.log" "adopting it over our 3" "host raised its delay to meet the peer"
h15_delay=$(sed -n 's/.*agreed input delay \([0-9]*\).*/\1/p' "$OUT/h15.log" | tail -1)
c15_delay=$(sed -n 's/.*agreed input delay \([0-9]*\).*/\1/p' "$OUT/c15.log" | tail -1)
[ -n "$h15_delay" ] && [ "$h15_delay" = "$c15_delay" ] \
	&& echo "  ok   both settled on the same delay ($h15_delay)" \
	|| { echo "  MISS delays disagree (host=$h15_delay client=$c15_delay)"; fail=1; }
[ "$h15_delay" = "10" ] \
	&& echo "  ok   the higher proposal won" \
	|| { echo "  MISS expected the higher proposal (got $h15_delay)"; fail=1; }
expect "$OUT/h15.log" "inputsum" "host advanced on the agreed timeline"
h15_sum=$(sed -n 's/.*core:inputsum frames=\([0-9]*\) sum=\(.*\)/\1 \2/p' "$OUT/h15.log" | tail -1)
c15_sum=$(sed -n 's/.*core:inputsum frames=\([0-9]*\) sum=\(.*\)/\1 \2/p' "$OUT/c15.log" | tail -1)
[ -n "$h15_sum" ] && [ "$h15_sum" = "$c15_sum" ] \
	&& echo "  ok   converged peers ran identical inputs ($h15_sum)" \
	|| { echo "  MISS input streams differ (host='$h15_sum' client='$c15_sum')"; fail=1; }

echo
echo "== automatic RTT negotiation preserves the transport delay floor"
# Loopback proposes the minimum of two frames. Ordinary Wi-Fi sessions are
# intentionally written with a ten-frame floor because later jitter is much
# worse than a couple of quiet startup probes can establish.
PORT18=$((PORT + 17))
printf 'role=host\nport=%s\nmode=netplay\ninput_delay=10\ninput_delay_auto=1\n' "$PORT18" > "$OUT/h18.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=netplay\ninput_delay=10\ninput_delay_auto=1\n' "$PORT18" > "$OUT/c18.session"
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h18.session" \
	"$OUT/harness" "$SHIM" 400 5 > "$OUT/h18.log" 2>&1 &
H18=$!
sleep 0.2
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c18.session" \
	"$OUT/harness" "$SHIM" 400 5 > "$OUT/c18.log" 2>&1 &
C18=$!
wait $H18 2>/dev/null || true
wait $C18 2>/dev/null || true
for side in h18 c18; do
	expect "$OUT/$side.log" "keeping transport floor 10" "$side rejected an optimistic startup sample"
	delay=$(sed -n 's/.*agreed input delay \([0-9]*\).*/\1/p' "$OUT/$side.log" | tail -1)
	[ "$delay" = "10" ] \
		&& echo "  ok   $side retained ten frames" \
		|| { echo "  MISS $side negotiated delay $delay instead of 10"; fail=1; }
done

echo
echo "== mode comes from the core, not the session"
# A session is armed once and covers every system, so it cannot name a mode -
# the core it ends up wrapping decides. No peer is needed to assert this: the
# shim logs the decision as it loads.
PORT9=$((PORT + 8))
mode_case() { # <tag> <core name> <mode line, or empty> <expected mode> <expected source>
	printf 'role=host\nport=%s\n' "$PORT9" > "$OUT/$1.session"
	[ -n "$3" ] && echo "$3" >> "$OUT/$1.session"
		FAKE_CORE_NAME="$2" NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
			NETPLAY_NEGOTIATE_TIMEOUT_MS=100 \
			NETPLAY_SESSION="$OUT/$1.session" \
		"$OUT/harness" "$SHIM" 5 15 > "$OUT/$1.log" 2>&1 || true
	expect "$OUT/$1.log" "mode=$4 for .* ($5)" "$2${3:+ + $3} -> $4 ($5)"
}

# Name matching is case-insensitive and on a substring, so the real cores'
# reported names ("Gambatte", "gpSP") land without pinning their exact casing.
mode_case m1 FakeCore ''            shared-screen "from core"
mode_case m2 Gambatte ''            link-cable    "from core"
mode_case m3 gpSP     ''            link-cable    "from core"
mode_case m4 FakeCore 'mode=link'   link-cable    "from session"
mode_case m5 Gambatte 'mode=netplay' shared-screen "from session"
mode_case m6 'mGBA Dual' ''         link-cable    "from core"

# A serial exchange that fits inside one retro_run only works if a packet that
# arrived mid-frame is delivered mid-frame. Before deliver_packets existed the
# core's poll_receive was an empty function and the byte waited out the frame.
echo
echo "== a core reading mid-frame gets its packet without a frame boundary"
MFPORT=$((PORT + 3))
printf 'role=host\nport=%s\nmode=link\n' "$MFPORT" > "$OUT/mfh.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=link\n' "$MFPORT" > "$OUT/mfc.session"

FAKE_CORE_NAME=Gambatte FAKE_CORE_MIDFRAME_POLL=1 FAKE_CORE_POLL_IN_RECEIVE=1 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/mfh.session" \
	"$OUT/harness" "$SHIM" 200 15 > "$OUT/mfh.log" 2>&1 &
MFH_PID=$!
sleep 0.5
FAKE_CORE_NAME=Gambatte FAKE_CORE_MIDFRAME_POLL=1 FAKE_CORE_POLL_IN_RECEIVE=1 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/mfc.session" \
	"$OUT/harness" "$SHIM" 200 15 > "$OUT/mfc.log" 2>&1 &
MFC_PID=$!
wait $MFH_PID 2>/dev/null || true
wait $MFC_PID 2>/dev/null || true

expect "$OUT/mfh.log" "core:np_recv" "host core received link packets"
expect "$OUT/mfh.log" "core:np_reentrant_poll_returned" "a poll from inside receive returned without recursing"

# The assertion that matters: a receive landing between a midframe_poll and the
# next run is one the frontend frame boundary did not gate.
if awk '/core:midframe_poll/{p=1;next} /^core:run$/{p=0} /core:np_recv/{if(p){print;exit}}' \
     "$OUT/mfh.log" | grep -q np_recv; then
	echo "  ok   packet delivered inside retro_run, before the next frame"
else
	echo "  MISS no packet was delivered mid-frame"
	fail=1
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS"
else
	echo "FAIL"
	echo "--- host ---";   tail -25 "$OUT/host.log"
	echo "--- client ---"; tail -25 "$OUT/client.log"
fi
exit $fail
