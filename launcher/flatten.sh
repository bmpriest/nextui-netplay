#!/bin/sh
#
# Lays out one firmware's launcher the way it is installed:
#
#   flatten.sh <nextui|spruce> <dest>
#
# The source tree keeps the scripts every firmware runs in common/ and each
# firmware's own in nextui/ or spruce/, but on a device they all sit together in
# <pak>/launcher/ and find each other there. A firmware's file wins over a common
# one of the same name. The release build and the tests both stage through this,
# so neither can drift from the other.

set -e
firmware=$1
dest=$2
here="$(cd "$(dirname "$0")" && pwd)"

case "$firmware" in
	nextui|spruce) ;;
	*) echo "usage: flatten.sh <nextui|spruce> <dest>" >&2; exit 2 ;;
esac
[ -n "$dest" ] || { echo "usage: flatten.sh <nextui|spruce> <dest>" >&2; exit 2; }

mkdir -p "$dest"
for f in "$here/common"/* "$here/$firmware"/*; do
	[ -f "$f" ] || continue
	cp "$f" "$dest/"
done
