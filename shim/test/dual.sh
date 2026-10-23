#!/bin/sh
# Two Gambatte-shaped cores per process. This verifies the instanced control
# plane without requiring a copyrighted ROM: each device bootstraps its hidden
# instance from the peer, then both local cores consume delayed network input.

set -e
cd "$(dirname "$0")"

CC="${CC:-cc}"
OUT=$(mktemp -d)
PORT=${PORT:-$((42000 + ($$ % 18000)))}
cleanup() {
	kill "$HOST_PID" "$CLIENT_PID" "$HOST2_PID" "$CLIENT2_PID" "$HOST3_PID" "$CLIENT3_PID" 2>/dev/null || true
	if [ -n "$KEEP_TEST_OUTPUT" ]; then echo "test logs kept at $OUT"; else rm -rf "$OUT"; fi
}
trap cleanup EXIT

(cd .. && make native >/dev/null)
$CC fake_core.c -o "$OUT/gambatte_libretro.so" -shared -fPIC -I../include -O0 -std=gnu99 -DFAKE_DUAL
$CC harness.c -o "$OUT/harness" -I../include -O0 -std=gnu99 -ldl
# Give the client a clock nine seconds off the host's, which is what the real
# pair measured. Without it both roles read the same machine clock and the
# cartridge-clock comparison below could not fail.
$CC skewclock.c -o "$OUT/skewclock.so" -shared -fPIC -O0 -std=gnu99 -ldl
CLIENT_CLOCK_SKEW=9
SHIM=../../bin/native/netplay_shim.so

printf 'role=host\nport=%s\nmode=link\ninput_delay=3\ninstanced_gambatte=1\n' "$PORT" > "$OUT/host.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=link\ninput_delay=3\ninstanced_gambatte=1\n' "$PORT" > "$OUT/client.session"

FAKE_CORE_NAME=Gambatte FAKE_CORE_NO_NETPACKET=1 HARNESS_EXPECT_CORE_NAME=Gambatte \
	FAKE_SRAM_BYTE=17 HARNESS_BUTTONS=11 NETPLAY_REAL_CORE="$OUT/gambatte_libretro.so" \
	HARNESS_RESET_AT=120 \
	NETPLAY_SESSION="$OUT/host.session" "$OUT/harness" "$SHIM" 180 5 > "$OUT/host.log" 2>&1 &
HOST_PID=$!
sleep 0.3
FAKE_CORE_NAME=Gambatte FAKE_CORE_NO_NETPACKET=1 HARNESS_EXPECT_CORE_NAME=Gambatte \
	FAKE_SRAM_BYTE=34 HARNESS_BUTTONS=22 NETPLAY_REAL_CORE="$OUT/gambatte_libretro.so" \
	LD_PRELOAD="$OUT/skewclock.so" SKEW_CLOCK_SECONDS="$CLIENT_CLOCK_SKEW" \
	NETPLAY_SESSION="$OUT/client.session" "$OUT/harness" "$SHIM" 180 5 > "$OUT/client.log" 2>&1 &
CLIENT_PID=$!

HRC=0; wait "$HOST_PID" || HRC=$?
CRC=0; wait "$CLIENT_PID" || CRC=$?
fail=0
expect() {
	if grep -q "$2" "$1"; then echo "  ok   $3"; else echo "  MISS $3"; fail=1; fi
}

echo "== dual-instance Gambatte"
# The version is deliberately not pinned here: the shim only accepts a core whose
# ABI equals the header it was built against, so a mismatch fails at the
# capability check with its own message rather than silently here.
expect "$OUT/host.log"   "dual ABI v. enabled (console A visible)" "host selected ABI console A"
expect "$OUT/client.log" "dual ABI v. enabled (console B visible)" "client selected ABI console B"
expect "$OUT/host.log"   "sent console A save memory to peer" "host exported its local save memory"
expect "$OUT/client.log" "sent console B save memory to peer" "client exported its local save memory"
expect "$OUT/host.log"   "console B save memory adopted" "host adopted client save memory"
expect "$OUT/client.log" "console A save memory adopted" "client adopted host save memory"
expect "$OUT/host.log"   "sent authoritative paired checkpoint" "host established authoritative paired state"
expect "$OUT/client.log" "adopted authoritative paired checkpoint" "client adopted authoritative paired state"
expect "$OUT/host.log"   "applied synchronized reset to console A" "host applied its logical-console reset"
expect "$OUT/client.log" "applied synchronized peer reset to console A" "client mirrored the host console reset"
expect "$OUT/host.log"   "instanced link agreed by both devices" "host recorded the pairing agreement"
expect "$OUT/client.log" "instanced link agreed by both devices" "client recorded the pairing agreement"
expect "$OUT/host.log"   "both consoles run the same cartridge" "host recognised the shared cartridge"
expect "$OUT/host.log"   "cable is local and Wi-Fi is input-only" "host completed bootstrap"
expect "$OUT/client.log" "cable is local and Wi-Fi is input-only" "client completed bootstrap"
expect "$OUT/host.log"   "paired consoles agree at frame" "host confirmed paired-state agreement"
expect "$OUT/client.log" "paired consoles agree at frame" "client confirmed paired-state agreement"
expect "$OUT/host.log"   "core:ports p0=11 p1=22" "host mapped A/B inputs into the paired core"
expect "$OUT/client.log" "core:ports p0=11 p1=22" "client mapped the same A/B inputs into its replica"
# Both devices must install the *same* pair of cartridge-clock epochs, or an RTC
# game diverges the first time it latches. The client's clock is skewed above,
# so the two sides can only agree by using the readings they exchanged; each
# taking its own would leave the pairs nine seconds apart. A comparison rather
# than a fixed expectation, because the values are two real clocks.
host_clocks=$(sed -n 's/.*cartridge clocks set: \(A=[0-9]* B=[0-9]*\).*/\1/p' "$OUT/host.log" | head -1)
client_clocks=$(sed -n 's/.*cartridge clocks set: \(A=[0-9]* B=[0-9]*\).*/\1/p' "$OUT/client.log" | head -1)
if [ -n "$host_clocks" ] && [ "$host_clocks" = "$client_clocks" ]; then
	echo "  ok   both devices installed the same cartridge clocks ($host_clocks)"
else
	echo "  MISS cartridge clocks differ: host '$host_clocks' client '$client_clocks'"
	fail=1
fi
# ...and the pair has to reflect both players, not one clock used twice: console
# A is the host's and console B the client's, which the skew makes visible.
host_a=${host_clocks%% *}; host_a=${host_a#A=}
host_b=${host_clocks##* }; host_b=${host_b#B=}
if [ -n "$host_a" ] && [ "$((host_b - host_a))" = "$CLIENT_CLOCK_SKEW" ]; then
	echo "  ok   console B carries the client's own clock (+${CLIENT_CLOCK_SKEW}s)"
else
	echo "  MISS console B clock is $((host_b - host_a))s from console A, expected $CLIENT_CLOCK_SKEW"
	fail=1
fi

host_runs=$(grep -c '^core:run$' "$OUT/host.log" || true)
client_runs=$(grep -c '^core:run$' "$OUT/client.log" || true)
[ "$host_runs" -ge 90 ] && [ "$host_runs" -lt 180 ] && echo "  ok   host advanced one paired core ($host_runs calls)" \
	|| { echo "  MISS host made $host_runs paired-core calls"; fail=1; }
[ "$client_runs" -ge 90 ] && [ "$client_runs" -lt 180 ] && echo "  ok   client advanced one paired core ($client_runs calls)" \
	|| { echo "  MISS client made $client_runs paired-core calls"; fail=1; }
[ "$HRC" -eq 0 ] || { echo "  MISS host exited $HRC"; fail=1; }
[ "$CRC" -eq 0 ] || { echo "  MISS client exited $CRC"; fail=1; }

# Different cartridges are legal - Red links to Blue - but only when each device
# owns both of them. Neither of these has the other's, so both must decline and
# ask the launcher for the network-serial core rather than pairing anyway.
echo
echo "== declining a pairing neither device can host"
printf 'role=host\nport=%s\nmode=link\ninput_delay=3\ninstanced_gambatte=1\n' "$((PORT + 1))" \
	> "$OUT/host2.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=link\ninput_delay=3\ninstanced_gambatte=1\n' \
	"$((PORT + 1))" > "$OUT/client2.session"

FAKE_CORE_NAME=Gambatte FAKE_CORE_NO_NETPACKET=1 HARNESS_EXPECT_CORE_NAME=Gambatte \
	HARNESS_ROM_ID=cartridge-red NETPLAY_REAL_CORE="$OUT/gambatte_libretro.so" \
	NETPLAY_SERIAL_FALLBACK="$OUT/host.fallback" \
	NETPLAY_SESSION="$OUT/host2.session" "$OUT/harness" "$SHIM" 180 5 > "$OUT/host2.log" 2>&1 &
HOST2_PID=$!
sleep 0.3
FAKE_CORE_NAME=Gambatte FAKE_CORE_NO_NETPACKET=1 HARNESS_EXPECT_CORE_NAME=Gambatte \
	HARNESS_ROM_ID=cartridge-blue NETPLAY_REAL_CORE="$OUT/gambatte_libretro.so" \
	NETPLAY_SERIAL_FALLBACK="$OUT/client.fallback" \
	NETPLAY_SESSION="$OUT/client2.session" "$OUT/harness" "$SHIM" 180 5 > "$OUT/client2.log" 2>&1 &
CLIENT2_PID=$!
wait "$HOST2_PID" 2>/dev/null || true
wait "$CLIENT2_PID" 2>/dev/null || true

# Also pins that the search ran the CRC32 filter rather than the old size-only
# one: the message carries the crc32 the peer declared.
expect "$OUT/host2.log"   "crc32 .* is not installed" "host searched for the peer cartridge"
expect "$OUT/host2.log"   "instanced link declined" "host declined the pairing"
expect "$OUT/client2.log" "instanced link declined" "client declined the pairing"
[ -f "$OUT/host.fallback" ] && echo "  ok   host asked the launcher for network serial" \
	|| { echo "  MISS host wrote no fallback marker"; fail=1; }
[ -f "$OUT/client.fallback" ] && echo "  ok   client asked the launcher for network serial" \
	|| { echo "  MISS client wrote no fallback marker"; fail=1; }
! grep -q "serial is local and Wi-Fi is input-only" "$OUT/host2.log" \
	&& ok_msg="  ok   declined pairing never reached the input barrier" \
	|| { ok_msg="  MISS declined pairing bootstrapped anyway"; fail=1; }
echo "$ok_msg"

# A replica that stops matching its peer used to be silent - each player simply
# watched the other's console do something it had never done. It must now be
# named, and it must land in the failure overlay rather than the dead end that
# a terminal dual_failed would be.
echo
echo "== a diverged replica is detected and reported"
printf 'role=host\nport=%s\nmode=link\ninput_delay=3\ninstanced_gambatte=1\n' "$((PORT + 2))" \
	> "$OUT/host3.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=link\ninput_delay=3\ninstanced_gambatte=1\n' \
	"$((PORT + 2))" > "$OUT/client3.session"

FAKE_CORE_NAME=Gambatte FAKE_CORE_NO_NETPACKET=1 HARNESS_EXPECT_CORE_NAME=Gambatte \
	FAKE_DUAL_DIVERGE_AT=40 NETPLAY_REAL_CORE="$OUT/gambatte_libretro.so" \
	NETPLAY_SESSION="$OUT/host3.session" "$OUT/harness" "$SHIM" 900 3 > "$OUT/host3.log" 2>&1 &
HOST3_PID=$!
sleep 0.3
FAKE_CORE_NAME=Gambatte FAKE_CORE_NO_NETPACKET=1 HARNESS_EXPECT_CORE_NAME=Gambatte \
	NETPLAY_REAL_CORE="$OUT/gambatte_libretro.so" \
	NETPLAY_SESSION="$OUT/client3.session" "$OUT/harness" "$SHIM" 900 3 > "$OUT/client3.log" 2>&1 &
CLIENT3_PID=$!
wait "$HOST3_PID" 2>/dev/null || true
wait "$CLIENT3_PID" 2>/dev/null || true

expect "$OUT/host3.log"   "core:dual_diverged" "host replica was made to diverge"
expect "$OUT/host3.log"   "PAIRED DESYNC at frame" "host detected the divergence"
expect "$OUT/client3.log" "PAIRED DESYNC at frame" "client detected the divergence"
expect "$OUT/host3.log"   "Consoles have diverged" "host reported it to the player"

if [ "$fail" -ne 0 ]; then
	echo "--- host3 ---"; tail -n 40 "$OUT/host3.log"
	echo "--- client3 ---"; tail -n 40 "$OUT/client3.log"
	echo "--- host ---"; tail -n 100 "$OUT/host.log"
	echo "--- client ---"; tail -n 100 "$OUT/client.log"
	echo "--- host2 ---"; tail -n 60 "$OUT/host2.log"
	echo "--- client2 ---"; tail -n 60 "$OUT/client2.log"
fi
exit "$fail"
