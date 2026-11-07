# Varnish

What Varnish is, what Netplay uses it for, and where its limits are.

## What it is

[Varnish.pak](https://github.com/Helaas/nextui-varnish-pak) (Kevin Vranken,
"Helaas") is an overlay service for NextUI. A preloaded library,
`varnish_overlay.so`, hooks SDL's render pipeline in every process the NextUI
boot chain starts. A daemon, `varnish`, renders overlays into shared memory
slots, and the hook composites those slots into each frame before it is
presented. Other paks talk to the daemon by writing one line to
`/tmp/varnish.fifo`.

Because it draws inside NextUI's own frames, it needs no framebuffer of its own
and does not flicker. That is what lets Netplay put something on screen while
NextUI, not Netplay, is in front.

## What Netplay uses it for

| Feature | Varnish command | Netplay side |
|---|---|---|
| Netplay icon in NextUI | `ICON` from an asset sheet, recoloured with the theme | `launcher/nextui/status-icon.sh`, `res/status@<scale>x.png` |
| Quick menu | `MENU` (title, up to six items, button hints), `HIDE` | `app/nextui/quickmenu.c` → `netplay-quickmenu.elf`, started by `launcher/nextui/quick-menu.sh` |
| Quick menu input | the input grab and release in `varnish.h` | the quick menu reads the buttons from evdev while holding the grab, so NextUI does not act on the same presses |
| Notices | `PILL` | short results ("Session ended.") after a quick menu action |

Both features are optional by design: every send fails quietly when the daemon
is not running, and Netplay behaves as before.

## The fork Netplay builds

Upstream Varnish (v0.1.0, `861d1ab`) has `PILL`, `HIDE`, `CLEAR` and hotkeys for
screenshots and video recording. Netplay builds a fork,
[bmpriest/nextui-varnish-pak](https://github.com/bmpriest/nextui-varnish-pak)
(checked out at `community-paks/nextui-varnish-pak`, v0.2.0), which adds:

- the `ICON` and `MENU` commands, and their CLI and shell helpers;
- the input grab and release (`varnish_grab_input()`, `varnish_release_input()`);
- the H700 port (`make h700`; it preloads through
  `.system/h700/paks/MinUI.pak/launch.sh`, because BaseOS starts NextUI
  directly);
- theme following and centred messages.

`make varnish` builds that checkout for tg5040, tg5050, my355 and h700 into
`dist/varnish/<platform>/`, which the release bundles as `varnish/<platform>/`.
`make check-varnish` fails the build if `app/nextui/varnish.h` differs from the bundled
header, because the quick menu would then speak a protocol the daemon does not.

As of 2026-11-02 these changes are still uncommitted in that checkout (26
files); until they are pushed to the fork, a lost checkout loses the icon and the
quick menu. They are worth offering upstream
([../upcoming.md](../upcoming.md#upstreaming)).

## Installing it

Turning on the icon or the quick menu offers to install the bundled Varnish
(`launcher/nextui/varnish-install.sh`): files are copied beside any existing install
(so a full Varnish.pak keeps its ffmpeg), renamed into place, then
`varnish --install` enables it. The bundle is the overlay alone, without ffmpeg,
so Varnish's own screen recording still needs the full Varnish.pak.

Installing means patching the boot chain: Varnish edits `.tmp_update/<platform>.sh`
(on H700, MinUI.pak's `launch.sh`) to export `LD_PRELOAD`, and adds a boot hook
that repairs that patch and starts the daemon. The hook only reaches processes
started after it, so **the first install needs a reboot**, and the app says so.

## What it can do

- Draw over NextUI's menus, and over MinArch games (its recorder captures
  MinArch's OpenGL path through the same hook).
- Up to eight overlay slots at once, one per client; sending again to the same
  client replaces its overlay. A menu takes three (the panel and two hint
  corners).
- Seven fixed positions (corners, top and bottom centre, centre). Pills and
  icons are as tall as a line of text; text is truncated with "...".
- Menus of one to six items with a title and NextUI-style button hints, in the
  theme's colours.

## What it cannot do

- **Deliver input to a client.** Varnish draws and arbitrates the input grab;
  it does not pass button presses on. The quick menu reads them itself from
  `/dev/input` while it holds the grab, so NextUI does not scroll or launch
  underneath it, and releasing waits until NextUI has drained what was pressed
  during the menu. Any other Varnish-drawn interaction has to do the same.
- **Run on the A30.** There is no my282 build, so the icon, quick menu and
  Varnish-based notices are absent there.
- **Reach programs that do not render through SDL in a hooked process.** Paks
  are covered only if they inherit the boot chain's `LD_PRELOAD`. Whether the
  overlay appears over DSperate (fbdev) or DraStic has not been checked.
- **Keep state across a restart.** Overlays sent during the daemon's five-second
  start-up warm-up are dropped, and a restarted daemon forgets every overlay.
  `status-icon.sh` resends the icon every 30 seconds for this reason.
- **Be guaranteed to deliver.** The FIFO is best effort: the helpers open it
  non-blocking and give up if no daemon is reading. A plain shell redirect to the
  FIFO can block forever after a daemon crash; use the helpers or check
  `/tmp/varnish.pid` first.
- **Survive an update that rewrites `.tmp_update`** without a reboot: the boot
  hook re-applies its patch and forces one more reboot.
- **Draw arbitrary graphics.** Pills, icons from a pre-scaled sheet (one
  pixel-exact sheet per device scale; it never resamples) and the fixed menu
  layout are all there is.

## Where it fits next

Varnish is the natural surface for anything Netplay has to say while NextUI is
in front: an incoming game invitation, a guest connecting, a cable plugged in.
In-game controls beside MinArch's own menu are a different problem, since
MinArch stops calling the core while that menu is open; see
[../upcoming.md](../upcoming.md).
