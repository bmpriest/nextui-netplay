#!/bin/sh
set -eu

if [ "$#" -ne 1 ] || [ ! -d "$1" ]; then
	echo "usage: $0 SESSION_DIRECTORY" >&2
	exit 2
fi

session=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
classifier="$script_dir/classify-screen.py"

classify() {
	role=$1
	image=$2
	if [ ! -f "$session/$image" ]; then
		echo "$image: missing"
		return
	fi
	printf '%s: ' "$image"
	python3 "$classifier" "$role" "$session/$image" || true
}

found=0
for path in "$session"/*-host.png "$session"/*-client.png; do
	[ -f "$path" ] || continue
	found=1
	image=${path##*/}
	case "$image" in
		*-host.png) classify host "$image" ;;
		*-client.png) classify client "$image" ;;
	esac
done
if [ "$found" -eq 0 ]; then
	echo "no role-labelled screenshots found in $session" >&2
	exit 1
fi
