DraStic Nintendo DS for my282 (Miyoo A30)
==========================================

This is an initial NextUI-compatible A30 package built around DraStic 2.5.0.4
and steward-fu's A30-specific SDL2, audio, input, and detour libraries. It is
intended to answer the first feasibility question: how well does DraStic run
ordinary DS software on the A30?

Install
-------
Extract the archive into the root of the SD card. It installs:

  Emus/my282/NDS.pak/
  Roms/Nintendo DS (NDS)/

Put legally obtained .nds ROMs in the ROM directory. The pak includes the
32-bit ARM DraStic binary from the local steward-fu/nds checkout used to build
the archive. DraStic is proprietary software; do not redistribute it unless
you have permission to do so.

Controls
--------
The A30 compatibility layer provides these important controls:

  R2             swap screens
  L2             toggle gamepad/stylus mode
  D-pad + A      move/use the stylus while in stylus mode
  MENU + Left/Right  change display layout
  MENU + L1      exit DraStic
  MENU + L2      quick-load state
  MENU + R1      fast-forward
  MENU + R2      quick-save state
  MENU + Select  open the original DraStic menu
  MENU + Start   open the custom display/settings menu

Notes
-----
This first package intentionally does not add multiplayer or Nintendo DS Wi-Fi
support. It establishes a known-working A30 baseline before any hooks change.

The launcher restores both the A30 framebuffer and backlight during NextUI's
process handoff. It also translates NextUI's shoulder-button ordering to the
ordering expected by the upstream A30 DraStic layer.

DraStic normally runs without a time limit. For display diagnostics, launch it
with NDS_WATCHDOG_SECONDS set to a positive number to stop it automatically.
Timestamped logs are written to NextUI's log directory.

If DraStic's own menu cannot exit, run this command over SSH:

  /mnt/SDCARD/Emus/my282/NDS.pak/hard-quit.sh

DraStic stores configuration, battery saves, and save states beneath the
drastic directory inside this pak, matching the original A30 port. Back up the
pak before replacing or removing it if you want to retain those files.

The newest log is also copied to NextUI's normal NDS log path (or /tmp as a
fallback).
