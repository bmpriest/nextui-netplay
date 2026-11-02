# Plan: USB-C cable link

A third way to connect two devices, beside house WiFi and ad hoc: a USB-C data
cable carrying plain IP. No radio is involved, so nothing about either device's
WiFi changes, and round trips drop to about a millisecond (0.3-0.7 ms measured
on two Bricks, against 4 ms ad hoc and 26-43 ms through an access point). On a
device with no WiFi at all, such as the RG28XX, it would be the only way to
play. Work happens on branch `feat/cable` (worktree `.worktrees/netplay-cable`).

Written 2026-11-02 from two existing implementations and checks on two Bricks.

## Sources

| Source | License | Use |
|---|---|---|
| sMainero's fork, `community-paks/nextui-netplay-cable` | MIT (Netplay's own) | Port its packet bridge and protocol framing; credit sMainero in the release |
| nx-handheld's `workspace/all/usblink` (`nx-redux/nx-handheld`) | **GPL-3.0** | Ideas and hardware findings only; **no code** may be copied into Netplay |

The fork split from `main` at `4f5607d` (2026-08-16). Since then `main` has 104
commits and the fork 31 (about 15,300 added lines across 79 files, including its
own tests and mGBA changes). The cable work is ported onto `main`, not merged:
`app/usbnet.c` (3,100 lines), `app/cableproto.c`, the `link=` session key, and
its tests (`testing/cableproto-unit.c`, `testing/test-cable.sh`).

## How USB works here

A USB link has one **host**, which enumerates, starts every transfer and
supplies 5 V, and one **device**, which answers. Linux runs the device side as a
*gadget*, assembled in configfs from functions; the firmware's own gadget
(`g1`) carries `adb`.

**The Brick (tg5040, kernel 4.9.191), read on two units on 2026-11-02:**

| Socket | Controllers | Roles |
|---|---|---|
| Bottom (USB0) | the only device controller (`5100000.udc-controller`), plus `ehci0`/`ohci0` | device or host |
| Top (USB1) | `5200000.ehci1`/`ohci1` only | host only |

The top socket has no device controller in the chip, so no driver can change
that: top-to-top can never work. Top-to-bottom always can. The kernel has
`CONFIG_USB_CONFIGFS_F_FS` and TUN but no ECM, NCM or RNDIS function, so there is
no `usb0` network interface to be had. Both implementations therefore do the
same thing:

```
game ↔ TCP ↔ tun ↔ daemon ↔ USB bulk endpoints ═cable═ USB ↔ daemon ↔ tun ↔ TCP ↔ game
```

On the device side the daemon serves a vendor interface through **FunctionFS**;
on the host side it claims that interface through **usbfs**; each side copies
packets between USB and a **TUN** interface. Everything above the TUN is
ordinary TCP/UDP, so the shim, broker and DS radios run on it unchanged.

Other platforms differ and must be probed, not assumed:

- **tg5050** (Smart Pro S): no TUN in the stock kernel; nx-handheld builds and
  ships `tun.ko` for it. Its main port is a PMIC-managed dual-role Type-C port.
- **H700** is about a dozen devices. The RG35XXH has two USB ports, the RG28XX
  one; which controller sits behind which port is unknown for every model.

## Design

### 1. Add a function beside `adb`, never take the gadget over

The fork takes the firmware's gadget over for the session: `adb` dies, and
creating and destroying configfs/functionfs objects on the H700's vendor kernel
corrupts its heap (a slab fault in an unrelated subsystem after a couple of
cycles, freezing the device and sometimes leaving the SD card read-only).

nx-handheld instead adds its function (`functions/ffs.net`) to the existing
`g1` configuration next to `ffs.adb`: unbind the UDC, link the function, rebind.
`adb` drops for 1-3 s and survives. Netplay adopts that shape, and it is the
first thing to test against the H700 freeze. Invariants:

- **Unlink the function before its `ep0` closes.** A linked FunctionFS function
  with no `ep0` stops the whole gadget binding: `adb` is dead until repaired.
- **A repair path that needs no live daemon.** Record the UDC name before
  unbinding; `stop`, and every daemon start, unlink a leftover function, rebind
  the recorded UDC and unmount the FunctionFS instance. SIGKILL cannot be
  caught, so this is the only cover for it.
- **Read controller attributes back** rather than trusting a write (the fork's
  v3.2.5 lesson on the vendor kernel).

### 2. USB role independent of Host and Join

nx-handheld runs the same daemon on both devices: each offers its gadget and
scans its host port at once, and whichever hears the other first brings the
link up. Netplay's Host and Join then mean only what they mean on WiFi. That
removes the fork's hardest rule for players ("Host takes the cable in its
bottom socket, Join in its top"): the only rule left is one device's top socket
to the other's bottom, and either way round works.

### 3. Never read the port-role nodes

Reading `/sys/devices/platform/soc*/usbc0/usb_host` (or `usb_device`,
`usb_null`, `usb_otg`) *switches* the port, and on a Smart Pro S the host role
stuck across reboots: no `adb`, no charging from a PC. Only `otg_role` is safe
to read. The fork switches the port by reading one of these nodes; that does not
come across. With role independence nothing needs switching: the top socket is
host from boot, and the bottom socket stays a device.

### 4. Addressing and discovery

A point-to-point /30 on its own subnet, never the ad hoc `10.0.0.0/24` (both
earlier transport additions collided with an existing range within days). The
USB-host side and the USB-device side each get a fixed address, and the daemon
reports the peer's. A point-to-point link has no broadcast, so the guest is
armed with the host's address directly, as on the ad hoc path, and anything
that announces goes unicast to the peer.

### 5. Health and status through what exists

- **During a game**: `netlink.c`'s heartbeat already detects a dead peer; the
  cable needs nothing of its own.
- **Link up or down**: the daemon has to keep a link-level keepalive anyway (a
  pulled cable shows as `EPROTO` on the host side, or silence). It publishes the
  state where the broker reads it, so the app's session screen, the icon and the
  quick menu show "cable connected / no device on the cable" from one source.
- **Host to guest**: the host's status crosses the link as a message, like any
  other session data.
- **Recovery**: `wifi-watchdog.sh` generalises to the transport: a
  transport-neutral target address (it hardcodes `10.0.0.1`), and a USB repair
  record beside the WiFi one. Its 60-second grace suits recovery, not a live
  status line.
- An unplugged cable does not end the session; re-plugging relinks with the
  same addresses (about a second in nx-handheld).

The fork's separate status file and its UI polling loop are not ported.

### 6. Power

The USB host supplies 5 V, so one device powers the other. The fork measured two
Bricks both charging (the host end through its free bottom socket). nx-handheld
found the opposite on a Brick with a Smart Pro S: the device end's charging sagged
the host's VBUS and dropped the link, so it turns the device end's charger off
through the PMIC while linked. Measure before choosing; either way, warn when
the powering device's battery is low.

### 7. Capability, probed per device

Offer the cable only where it can work, decided at run time: a device
controller in `/sys/class/udc` (for the device role), a host controller, TUN
(or a module to load), writable configfs, and FunctionFS. Log the first check
that fails. Socket hints are generic unless the exact model is known ("use a
data cable; if nothing connects, try the other port").

### 8. The app

Two selectable entries, built in the current row-and-hint menu style with text
in `pak_hints.json`:

- **Host ▸ Cable (USB)**
- **Join ▸ USB cable peer** (a fixed last row; it is not a discovered host)

Plus status text: the cable line on the session screen, "Hosting over cable" /
"Joining over cable" / "Ending the cable link" progress, and a startup notice
when an interrupted session's gadget was repaired. Input delay floor: 1 frame.

## Phases

1. **Inventory.** Read the fork's `usbnet.c`, `cableproto.c` and tests; list
   what is ported, what is replaced by the design above, and what is dropped.
2. **Daemon on `main`.** The bridge and framing as a separate binary with a
   host build, and the fork's protocol unit tests in `make test`.
3. **Gadget beside `adb` on a Brick.** Pass criteria: 50 arm/stop cycles with
   `adb` alive after each; `kill -9` of the daemon followed by `stop` restores
   `adb`; a pulled and re-plugged cable relinks.
4. **Role independence.** Either cable direction with either device hosting.
5. **App integration.** Menu entries, `link=` session key, broker status, the
   watchdog's repair record, hints.
6. **Matrix cells** over the cable: the systems tests and DS races, Brick to
   Brick.
7. **Other hardware.** RG35XXH: map its ports to controllers with read-only
   queries, then the 50-cycle freeze test. Ship H700 cable only for models that
   pass it, listed by model. tg5050: `tun.ko`, and the role-node caution.

## Open questions

- Does the H700 vendor kernel still corrupt its heap when a function is added
  beside `adb` rather than the gadget swapped? Phase 7 answers it.
- Can a single-port device (RG28XX) be the USB device to a Brick's top socket?
- The fork's DS figures predate the cable; DS over cable is untested anywhere.
