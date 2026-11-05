#!/bin/sh
#
# spruce's own game menu in front instead of DevBridge's app, for the
# invitation tests (an invitation over an app's menu is dropped).
#
#   spruce-menu.sh
#
# DevBridge's launcher keeps its pid in /tmp/DevBridge-spruce-ui. SIGTERM
# runs its cleanup without ending it, and B in its list did not close it on
# the RG35XXH, so the launcher and its list are killed outright: spruce's loop
# then goes on to its menu. Waits for that menu and 5 s more for it to draw.

ui=/tmp/DevBridge-spruce-ui
if [ -f $ui ] && kill -0 "$(cat $ui)" 2>/dev/null; then
	kill -9 "$(cat $ui)"
	for m in $(pidof MainUI); do
		tr '\0' ' ' < /proc/$m/cmdline | grep -q -- -optionListFile && kill -9 $m
	done
	rm -f $ui
fi
i=0
while [ $i -lt 40 ]; do
	for m in $(pidof MainUI); do
		tr '\0' ' ' < /proc/$m/cmdline | grep -q -- -optionListFile || { sleep 5; exit 0; }
	done
	sleep 0.5
	i=$((i + 1))
done
echo "spruce's menu did not come back" >&2
exit 1
