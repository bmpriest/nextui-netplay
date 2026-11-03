# A DSperate peer on a PC

DraStic's radio speaks melonDS's LAN protocol, so any DSperate (or melonDS) can
be the other console. This directory turns DSperate's headless frontend into a
scriptable, always-available peer for testing a single handheld, which is how
both roles were first proven on 2026-10-23 while the second handheld was
asleep.

## Build

```sh
git -C ~/dev/sbcs/cores/DSperate worktree add /tmp/dsp-peer ddf2910
cd /tmp/dsp-peer && git apply .../dsperate-headless-control.patch
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DDSPERATE_SDL=OFF \
    -DDSPERATE_TESTS=OFF -DDSPERATE_CHEEVOS=OFF -DDSPERATE_VULKAN=OFF -DDSPERATE_NET=ON
ninja -C build dsperate-headless
```

(`pip install --user cmake` if the host has none.) The patch adds `DS_CTL`, a
FIFO the run reads every frame:

| line | effect |
|---|---|
| `keys a+down 12` | hold buttons for 12 frames |
| `touch 128 96 10` | hold the pen at (128,96) for 10 frames |
| `shot /path.ppm` | write both screens (256x384) after this frame |

and `DS_LAN_TIMEOUT_MS` (DSP.pak's patch 0005 reads it in the SDL frontend
only).

## Run

```sh
mkfifo ctl
DS_CTL=$PWD/ctl DS_LAN_LOG=1 DS_LAN_TIMEOUT_MS=100 build/.../dsperate-headless \
    --direct --bios9 nds_bios_arm9.bin --bios7 nds_bios_arm7.bin --save mk.sav \
    --lan-join 192.168.0.144 --lan-name WSL --frames 400000 mariokart.nds
PEER_DIR=$PWD ./ctl.sh 'keys down 12' 'sleep 1.5' 'keys a 10' 'sleep 8' 'shot rooms'
```

Mario Kart ignores the first D-pad press after its main menu settles; check a
shot before pressing A.

## The handheld as guest from WSL2

WSL2's NAT lets the PC join a handheld's session (outbound UDP) but not host
one. For that direction, carry the ENet datagrams over an SSH reverse tunnel:

```sh
# on the handheld (udptcp.c, built with the platform toolchain)
start-stop-daemon -S -b -m -p /tmp/udptcp.pid -x /tmp/udptcp -- 7064 7065
# on the PC
python3 tcpudp.py 17065 7064 &
ssh -N -R 7065:127.0.0.1:17065 root@<handheld> &
# DSperate hosts with --lan-host; DraStic joins 127.0.0.1 (drastic/lan: join:127.0.0.1)
```

TCP over the house Wi-Fi adds enough latency (22 ms average, 90 ms spikes to
the Brick) that 25 ms MP waits end in "Communication error"; use 100 ms on both
ends (`drastic/lan_timeout_ms`, `DS_LAN_TIMEOUT_MS`). Speed figures from this
setup measure the tunnel, not the emulators.
