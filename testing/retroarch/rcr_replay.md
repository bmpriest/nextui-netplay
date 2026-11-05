# Replaying a RetroArch netplay capture offline

How the 2026-11-04 River City Ransom resyncs were traced to core options
(docs/analysis/retroarch-protocol.md, Core options). Host-only; needs
qemu-aarch64 and an AArch64 root with libc and libz for `-L`.

```sh
python3 -I replay_extract.py testing/results/tools/radecode.py h700.pcap out/
unzip -p 'River City Ransom (USA).zip' '*.nes' > out/rcr.nes
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w \
    ghcr.io/loveretro/tg5040-toolchain:latest \
    aarch64-nextui-linux-gnu-gcc -O1 -I../../shim/include replay.c -o out/replay -ldl -lz
cd out
# the host's options and its own history: CRCs match the host's
PRE=0 HIST=state0.bin CMP=state312.bin CMP_AT=312 \
STATE_VARS='fceumm_sndquality=High;fceumm_sndvolume=10;fceumm_turbo_enable=Player 1' \
    qemu-aarch64 -L $ARM64_ROOT ./replay fceumm_libretro.so rcr.nes state19.bin 19 inputs.txt crcs.txt 312
```

`replay <core> <rom> <state> <start> <inputs> <crcs> [end]` loads `<state>` as
the state before `<start>`, runs the captured inputs (port 1 host, port 2
client) and prints its CRC beside the host's at every CRC frame. `STATE_VARS`
sets core options; `RR_SPECIAL=1` answers the rollback-netplay savestate
context; `PRE=N` runs N power-on frames before the load (default 2);
`HIST=<state>` loads that at frame 0 and runs the inputs up to `<start>`
first; `CMP`/`CMP_AT` diff the whole state at a frame against a sent one.
