#!/bin/bash
# Assert the ABI floor of a shipped binary.
#
# Usage: check-abi.sh <max-glibc> <binary>...
#
# Two failure modes, both silent at build time and fatal on device:
#
#  1. A symbol versioned above what the device's glibc provides. Upstream's own
#     release binaries need GLIBC_2.38; the devices are 2.33 (TrimUI SDK) and
#     2.35 (H700 stock). The NextUI sysroot is older still, so anything high
#     means the build reached outside it.
#  2. A surviving GLIBCXX_/CXXABI_ requirement, meaning -static-libstdc++ did
#     not take and the binary now wants a libstdc++.so.6 no MinUI device ships.
#
# Run this AFTER stripping: .gnu.version_r survives --strip-unneeded, so the
# check then covers the exact bytes that ship.
set -euo pipefail

READELF="${READELF:-aarch64-nextui-linux-gnu-readelf}"
MAX="$1"
shift
rc=0

for bin in "$@"; do
    echo "== $bin"

    if ! "$READELF" -h "$bin" | grep -q 'Machine:.*AArch64'; then
        echo "  FAIL: not an AArch64 object"
        rc=1
        continue
    fi

    reqs="$("$READELF" -V --wide "$bin" | grep -oE '\b(GLIBC|GLIBCXX|CXXABI)_[0-9][0-9.]*' | sort -u || true)"

    cxx="$(printf '%s\n' "$reqs" | grep -E '^(GLIBCXX|CXXABI)_' || true)"
    if [ -n "$cxx" ]; then
        echo "  FAIL: libstdc++ is dynamic -- -static-libstdc++ did not take:"
        # Deliberately unquoted: split the list so each version prints on its own line.
        # shellcheck disable=SC2086
        printf '    %s\n' $cxx
        rc=1
    fi

    if "$READELF" -d --wide "$bin" | grep -q 'NEEDED.*libstdc++'; then
        echo "  FAIL: NEEDED libstdc++.so.6"
        rc=1
    fi

    if "$READELF" -V --wide "$bin" | grep -q 'GLIBC_PRIVATE'; then
        echo "  FAIL: references GLIBC_PRIVATE"
        rc=1
    fi

    # sort -V, never a lexical sort: 2.9 must not rank above 2.28.
    # || true matters: with no GLIBC_ lines the grep fails, and under set -e a
    # failing command substitution in an assignment aborts the script before it
    # can report why.
    highest="$(printf '%s\n' "$reqs" | grep -E '^GLIBC_[0-9]' | sed 's/^GLIBC_//' | sort -V | tail -n1 || true)"
    if [ -z "$highest" ]; then
        echo "  FAIL: no GLIBC_ version requirements at all -- is this statically linked?"
        rc=1
    elif [ "$(printf '%s\n%s\n' "$highest" "$MAX" | sort -V | tail -n1)" != "$MAX" ]; then
        echo "  FAIL: needs GLIBC_$highest, ceiling is GLIBC_$MAX"
        rc=1
    else
        echo "  ok: max GLIBC_$highest (ceiling GLIBC_$MAX)"
    fi

    # Not an assertion. This is how launch.sh's LD_LIBRARY_PATH gets validated
    # and how the h700 SDL2 SONAME question stays answered in every CI log.
    echo "  NEEDED: $("$READELF" -d --wide "$bin" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"
done

exit $rc
