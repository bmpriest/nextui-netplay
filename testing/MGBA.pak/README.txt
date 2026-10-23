mGBA supplied by Netplay.pak
============================

This is the ordinary, single-player mGBA frontend installed by Netplay when
instanced mGBA is enabled. Builds are supplied for tg5040, h700 and my282.

The pak intentionally contains only mgba_libretro.so, its launcher and config.
The paired core and Netplay shim remain inside Netplay.pak at:

  cores/override/<platform>/mgba_dual_libretro.so
  bin/<platform>/netplay_shim.so

During an armed paired session, this launcher enters Netplay's shared pre-launch
path. Netplay's minarch wrapper selects the paired core and applies the normal
verbose-log setting. Ordinary single-player launches retain the ordinary core.

Installation and removal are managed from Netplay > Settings > Use instanced
cores. Existing MGBA.paks receive collision-free .bak names; saves and states
are copied beneath .userdata/shared/Netplay/mgba-save-backup. Disabling the
feature offers to restore each category independently. Files that would be
overwritten during restore are preserved beneath shared/Netplay.

The paired beta currently targets GBA cartridges. It has completed Mario Kart:
Super Circuit races on two TrimUI Bricks. The A30 does not have enough sustained
CPU headroom for the current same-thread dual implementation, and h700 remains
compile-tested without a hardware tester.
