#!/bin/sh
# Run the requested spruce comparison on the already-armed pair.
set -u
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
protocol=$1
shift
status=0
for spec in "$@"; do
	IFS='|' read -r test system rom <<EOF
$spec
EOF
	tag=$protocol-$test
	echo "=== $tag: launch ==="
	if ! "$here/launch.sh" "$tag" "$system" "$rom"; then
		echo "FAILED launch $tag"
		status=1
		continue
	fi
	echo "=== $tag: play ==="
	if ! "$here/play.sh" "$test" "$tag"; then
		echo "FAILED play $tag; collecting diagnostic artifacts"
		status=1
		"$here/collect.sh" "$tag"
		continue
	fi
	echo "=== $tag: collect ==="
	if ! "$here/collect.sh" "$tag"; then
		echo "FAILED multiplayer evidence $tag"
		status=1
	fi
done
exit "$status"
