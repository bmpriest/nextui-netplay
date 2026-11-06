#!/bin/sh
# Run the requested spruce comparison on the already-armed pair.
set -u
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
protocol=$1
shift
for spec in "$@"; do
	IFS='|' read -r test system rom <<EOF
$spec
EOF
	tag=$protocol-$test
	echo "=== $tag: launch ==="
	if ! "$here/launch.sh" "$tag" "$system" "$rom"; then
		echo "FAILED launch $tag"
		continue
	fi
	echo "=== $tag: play ==="
	"$here/play.sh" "$test" "$tag"
	echo "=== $tag: collect ==="
	"$here/collect.sh" "$tag"
done
