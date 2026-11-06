#!/bin/sh
# One checked broker fixture stage for the spruce two-device comparison.
set -eu
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
stage=${1:?stage required}
tag=${BROKER_TAG:?BROKER_TAG required}
protocol=${BROKER_PROTOCOL:-linkplay}
export RIG=${RIG:-$here/rig.conf}
export OUT=${DEVBRIDGE_ATTEMPT_DIR:?DEVBRIDGE_ATTEMPT_DIR required}/measurement
export CAPTURE_MODE=logs
mkdir -p "$OUT"
case $stage in
	preflight)
		"$here/adhoc.sh" up
		;;
	setup)
		"$here/arm.sh" "$protocol"
		;;
	test)
		"$here/launch.sh" "$tag" GBC 'Super Mario Bros. Deluxe (USA, Europe) (Rev 2).zip'
		"$here/play.sh" mariodx "$tag"
		;;
	collect)
		"$here/collect.sh" "$tag"
		;;
	judge)
		python3 "$here/judge.py" "$OUT/$tag"
		python3 "$here/table.py" "$OUT/$tag" > "$OUT/$tag/comparison.md"
		;;
	teardown)
		"$here/adhoc.sh" down
		;;
	*) echo "unknown broker fixture stage: $stage" >&2; exit 2 ;;
esac
