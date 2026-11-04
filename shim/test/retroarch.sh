#!/bin/sh
# The shim's RetroArch protocol (raproto.c) against a model of RetroArch
# 1.22.2's netplay peer (testing/retroarch/ra_model.py), over loopback, in
# both roles and at protocols 5, 6 and 7.
#
#   ./retroarch.sh
#
# The model enforces the rules that make RetroArch NAK or hang up, simulates
# the fake core over the frames whose inputs are known, and exchanges CRCs of
# that state with the shim. Agreement therefore means both sides ran every
# frame with the same two inputs on the same ports, starting from the same
# loaded state. The desync cases corrupt the shim's core and expect one
# mismatch, one savestate, and agreement afterwards.
#
# Host-only; needs python3. Not a substitute for a real RetroArch: see
# docs/analysis/retroarch-protocol.md for what the model does not cover.

set -e
cd "$(dirname "$0")"

CC="${CC:-cc}"
OUT=$(mktemp -d)
MODEL=../../testing/retroarch/ra_model.py
BASE=${PORT:-$((42000 + ($$ % 15000)))}
cleanup() {
	kill $SHIM_PID 2>/dev/null || true
	if [ -n "$KEEP_TEST_OUTPUT" ]; then echo "test logs kept at $OUT"; else rm -rf "$OUT"; fi
}
trap cleanup EXIT

echo "== building"
(cd .. && make native >/dev/null)
$CC fake_core.c -o "$OUT/fake_libretro.so" -shared -fPIC -I../include -O0 -std=gnu99
$CC harness.c   -o "$OUT/harness"          -I../include -O0 -std=gnu99 -ldl
$CC raproto_test.c ../raproto.c -I../include -o "$OUT/raproto_test" -lz -lpthread
SHIM=../../bin/native/netplay_shim.so

fail=0
say() { # <condition-result> <description>
	if [ "$1" -eq 0 ]; then echo "  ok   $2"; else echo "  MISS $2"; fail=1; fi
}
count() { grep -c "$1" "$2" 2>/dev/null || true; }

# shim_client <name> <port> <frames> [model args...]: RetroArch hosts.
shim_client() {
	name=$1 port=$2 frames=$3; shift 3
	printf 'role=client\npeer=127.0.0.1\nprotocol=retroarch\nra_port=%s\ninput_delay=4\nnick=Brick\n' \
		"$port" > "$OUT/$name.session"
	python3 -I "$MODEL" host "$port" --frames "$frames" "$@" > "$OUT/$name.model" 2>&1 &
	MODEL_PID=$!
	sleep 0.3
	env FAKE_STATE_INPUTS=1 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
		NETPLAY_SESSION="$OUT/$name.session" HARNESS_EXPECT_NO_PERSISTENCE=1 $SHIM_ENV \
		"$OUT/harness" "$SHIM" $((frames + 300)) 16 > "$OUT/$name.shim" 2>&1 &
	SHIM_PID=$!
	wait $MODEL_PID || true
	wait $SHIM_PID || true
	SHIM_PID=
}

# shim_host <name> <port> <frames> [model args...]: the shim hosts.
shim_host() {
	name=$1 port=$2 frames=$3; shift 3
	printf 'role=host\nprotocol=retroarch\nra_port=%s\ninput_delay=4\nnick=H700\n' \
		"$port" > "$OUT/$name.session"
	env FAKE_STATE_INPUTS=1 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
		NETPLAY_SESSION="$OUT/$name.session" $SHIM_ENV \
		"$OUT/harness" "$SHIM" $((frames + 300)) 16 > "$OUT/$name.shim" 2>&1 &
	SHIM_PID=$!
	sleep 0.3
	python3 -I "$MODEL" client 127.0.0.1 "$port" --frames "$frames" "$@" > "$OUT/$name.model" 2>&1 || true
	wait $SHIM_PID || true
	SHIM_PID=
}

model_ok() { grep -q "^model: ok" "$OUT/$1.model"; }
model_field() { sed -n "s/.* $2=\([0-9]*\).*/\1/p" "$OUT/$1.model"; }

echo
echo "== wire helpers against RetroArch 1.22.2's values"
# The capture lives in the git-ignored results tree; without it the
# byte-for-byte container comparison is skipped.
CAP=../../testing/results/retroarch-spruce-20261102/h700-host.pcap
DEC=../../testing/results/tools/radecode.py
if [ -f "$CAP" ] && [ -f "$DEC" ] &&
	python3 -I ../../testing/retroarch/extract_state.py "$(realpath "$DEC")" "$(realpath "$CAP")" "$OUT/spruce.state"; then
	r=0; "$OUT/raproto_test" "$OUT/spruce.state" 5572 > "$OUT/raproto_test.log" 2>&1 || r=$?
else
	echo "  (no spruce capture here; container comparison skipped)"
	r=0; "$OUT/raproto_test" > "$OUT/raproto_test.log" 2>&1 || r=$?
fi
sed 's/^/  /' "$OUT/raproto_test.log" | grep -v RESULT
say $r "raproto helpers"

echo
echo "== RetroArch hosts, the shim joins (protocol 7)"
SHIM_ENV= shim_client c7 $BASE 900
model_ok c7; say $? "the model saw a well-formed peer for 900 frames ($(sed -n 's/^model: //p' "$OUT/c7.model"))"
grep -q "host header: protocol 7, compression zlib" "$OUT/c7.shim"; say $? "negotiated protocol 7 with zlib"
grep -q "SYNC: frame 1000, we are client 1" "$OUT/c7.shim"; say $? "adopted the host's frame counter from SYNC"
grep -q "playing as RetroArch port 2 from frame" "$OUT/c7.shim"; say $? "MODE made us player 2"
grep -q "loaded the host's state for frame 1000" "$OUT/c7.shim"; say $? "loaded the host's container state"
[ "$(count 'RetroArch CRC agrees' "$OUT/c7.shim")" -ge 3 ]; say $? "the host's CRCs agree with our state"
! grep -q "CRC mismatch" "$OUT/c7.shim"; say $? "no CRC mismatches"
grep -q "RESULT: ok" "$OUT/c7.shim"; say $? "harness assertions (guest hides SRAM from the frontend)"

echo
echo "== the shim hosts, RetroArch joins (protocol 7)"
SHIM_ENV= shim_host h7 $((BASE + 1)) 900 --request-at 400
model_ok h7; say $? "the model saw a well-formed host ($(sed -n 's/^model: //p' "$OUT/h7.model"))"
[ "$(model_field h7 crc_ok)" -ge 2 ] && [ "$(model_field h7 crc_bad)" -eq 0 ]
say $? "our CRCs agree with the model's simulation"
[ "$(model_field h7 loads)" -ge 2 ]; say $? "a mid-session savestate request was served at the stream point"
grep -q "RetroArch client plays port 2 from frame" "$OUT/h7.shim"; say $? "answered PLAY with MODE"
grep -q "RESULT: ok" "$OUT/h7.shim"; say $? "harness assertions"

echo
echo "== older RetroArch protocols"
SHIM_ENV= shim_client c5 $((BASE + 2)) 600 --max-proto 5
model_ok c5 && grep -q "host header: protocol 5" "$OUT/c5.shim"
say $? "joins a protocol 5 host (raw core state, no settings, no ping)"
SHIM_ENV= shim_host h6 $((BASE + 3)) 600 --max-proto 6
model_ok h6 && [ "$(model_field h6 crc_bad)" -eq 0 ] && grep -q "client header: protocol 6" "$OUT/h6.shim"
say $? "hosts a protocol 6 client (raw core state)"

echo
echo "== a desync on the joining side is detected and repaired"
SHIM_ENV="FAKE_CORE_CORRUPT_AT=150" shim_client cd $((BASE + 4)) 900
model_ok cd; say $? "session survives ($(sed -n 's/^model: //p' "$OUT/cd.model"))"
[ "$(count 'CRC mismatch' "$OUT/cd.shim")" -eq 1 ]; say $? "exactly one mismatch, then agreement"
grep -q "requested the host's savestate" "$OUT/cd.shim"; say $? "asked the host for its state"
[ "$(model_field cd requests)" -ge 1 ] && [ "$(model_field cd loads)" -ge 2 ]
say $? "the host answered with a state"

echo
echo "== a desync on the hosting side is detected and repaired"
SHIM_ENV="FAKE_CORE_CORRUPT_AT=150" shim_host hd $((BASE + 5)) 900
model_ok hd; say $? "session survives ($(sed -n 's/^model: //p' "$OUT/hd.model"))"
[ "$(model_field hd crc_bad)" -eq 1 ] && [ "$(model_field hd crc_ok)" -ge 1 ]
say $? "one mismatch, agreement after the state"
grep -q "RetroArch client asked for a savestate" "$OUT/hd.shim"; say $? "served the client's request"

if [ "$fail" -eq 0 ]; then echo "PASS"; else
	echo "FAIL"
	for f in "$OUT"/*.model; do echo "--- $f"; cat "$f"; done
fi
exit $fail
