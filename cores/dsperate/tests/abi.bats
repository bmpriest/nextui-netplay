#!/usr/bin/env bats

# check-abi.sh is pure text processing over readelf output, so a stubbed readelf
# exercises every branch on a machine with no cross toolchain.

load test_helper

setup() {
    STUB="$BATS_TEST_TMPDIR/stub"
    mkdir -p "$STUB"
    BIN="$BATS_TEST_TMPDIR/fake-binary"
    : >"$BIN"
    export READELF="$STUB/readelf"
}

# fake_readelf <machine> <version-list> <needed-list>
fake_readelf() {
    cat >"$STUB/readelf" <<STUB_EOF
#!/bin/bash
case "\$1" in
-h) echo "  Machine:                           $1" ;;
-V) printf '%s\n' $2 ;;
-d) for n in $3; do echo "  0x0001 (NEEDED)  Shared library: [\$n]"; done ;;
esac
STUB_EOF
    # shellcheck disable=SC2016
    sed -i.bak "s|Machine:                           \$1|Machine:                           $1|" "$STUB/readelf"
    chmod +x "$STUB/readelf"
}

run_check() {
    run "$REPO_ROOT/scripts/check-abi.sh" "$1" "$BIN"
}

@test "passes a binary at the ceiling" {
    fake_readelf "AArch64" "GLIBC_2.17 GLIBC_2.18 GLIBC_2.28" "libc.so.6"
    run_check 2.28
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok: max GLIBC_2.28"* ]]
}

@test "passes a binary below the ceiling" {
    fake_readelf "AArch64" "GLIBC_2.17 GLIBC_2.18" "libc.so.6 libm.so.6"
    run_check 2.28
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok: max GLIBC_2.18"* ]]
}

@test "fails the real upstream failure: GLIBC_2.38" {
    fake_readelf "AArch64" "GLIBC_2.17 GLIBC_2.29 GLIBC_2.38" "libc.so.6"
    run_check 2.28
    [ "$status" -ne 0 ]
    [[ "$output" == *"needs GLIBC_2.38"* ]]
}

@test "fails when a GLIBCXX_ requirement survives" {
    fake_readelf "AArch64" "GLIBC_2.17 GLIBCXX_3.4.30" "libc.so.6"
    run_check 2.28
    [ "$status" -ne 0 ]
    [[ "$output" == *"-static-libstdc++ did not take"* ]]
}

@test "fails when a CXXABI_ requirement survives" {
    fake_readelf "AArch64" "GLIBC_2.17 CXXABI_1.3.9" "libc.so.6"
    run_check 2.28
    [ "$status" -ne 0 ]
    [[ "$output" == *"-static-libstdc++ did not take"* ]]
}

@test "fails on NEEDED libstdc++ even with no version requirement" {
    fake_readelf "AArch64" "GLIBC_2.17" "libc.so.6 libstdc++.so.6"
    run_check 2.28
    [ "$status" -ne 0 ]
    [[ "$output" == *"NEEDED libstdc++.so.6"* ]]
}

@test "fails a non-AArch64 object" {
    fake_readelf "Advanced Micro Devices X86-64" "GLIBC_2.17" "libc.so.6"
    run_check 2.28
    [ "$status" -ne 0 ]
    [[ "$output" == *"not an AArch64 object"* ]]
}

@test "fails when there are no GLIBC_ requirements at all" {
    # Would otherwise pass the ceiling check vacuously.
    fake_readelf "AArch64" "" ""
    run_check 2.28
    [ "$status" -ne 0 ]
    [[ "$output" == *"no GLIBC_ version requirements"* ]]
}

@test "sorts versions numerically, not lexically" {
    # The bug this script is most likely to have: a lexical sort ranks 2.9
    # above 2.28 and rejects a perfectly good binary.
    fake_readelf "AArch64" "GLIBC_2.9 GLIBC_2.17" "libc.so.6"
    run_check 2.28
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok: max GLIBC_2.17"* ]]
}

@test "reports the NEEDED list for the launcher's library path" {
    fake_readelf "AArch64" "GLIBC_2.17" "libSDL2-2.0.so.0 libc.so.6"
    run_check 2.28
    [ "$status" -eq 0 ]
    [[ "$output" == *"NEEDED: libSDL2-2.0.so.0 libc.so.6"* ]]
}
