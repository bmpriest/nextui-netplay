#!/bin/bash
# Move the pinned DSperate release to <tag>.
#
# Usage: bump-dsperate.sh <tag> [root]
#
# Rewrites DSPERATE_TAG in the Makefile and the version the README names, in
# the repo at [root] (default: the one this script lives in). update-dsperate.yaml
# runs it when upstream publishes a release. Pinning is the only part that can
# be automated: the patches, the CMake options and the ABI table still need the
# review "Bumping the upstream version" in TECHNICAL.md describes.
#
# A tag that is not vX.Y.Z is refused rather than written: a branch name would
# clone fine and make every build non-reproducible, and tests/makefile.bats
# would only catch it after the fact.
set -euo pipefail

TAG="${1:-}"
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if ! [[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: '$TAG' is not a release tag of the form vX.Y.Z" >&2
    exit 1
fi
VERSION="${TAG#v}"

# Not sed -i: its syntax differs between GNU and BSD, and this runs on both.
rewrite() {
    local file="$1" expr="$2"
    sed -E "$expr" "$file" >"$file.tmp"
    mv "$file.tmp" "$file"
}

rewrite "$ROOT/Makefile" "s/^(DSPERATE_TAG[[:space:]]*:=[[:space:]]*).*/\1$TAG/"
rewrite "$ROOT/README.md" "s/\(version [0-9]+\.[0-9]+\.[0-9]+\)/(version $VERSION)/"

# A pattern that stopped matching would leave the old version in place and
# exit 0, so check the result rather than trusting the substitution.
if ! grep -qE "^DSPERATE_TAG[[:space:]]*:=[[:space:]]*$TAG\$" "$ROOT/Makefile"; then
    echo "error: DSPERATE_TAG in $ROOT/Makefile was not updated" >&2
    exit 1
fi
if ! grep -qF "(version $VERSION)" "$ROOT/README.md"; then
    echo "error: the version in $ROOT/README.md was not updated" >&2
    exit 1
fi

echo "DSperate pinned at $TAG"
