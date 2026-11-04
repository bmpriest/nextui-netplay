Core options a stock RetroArch peer runs with, per firmware, as RetroArch's
`<core name>.opt` files (`key = "value"`). A RetroArch-protocol session points
the shim here (`ra_options=` in the session) and the shim runs the core with
the file named after its `library_name`, because lockstep needs both sides to
run the same options, and some (fceumm's sound quality and volume) are part of
the serialized state. See docs/analysis/retroarch-protocol.md, Core options.

spruce/: copied from spruceOS `RetroArch/.retroarch/config/<core>/<core>.opt`
at 0c410ba (2026-10-25). They are spruce's shipped values; a spruce user who
changes an option there has to change it here too.
