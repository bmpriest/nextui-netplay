#!/usr/bin/env bats

load test_helper

# A scratch copy of the files the bump rewrites, so the repo is never touched.
setup() {
    WORK="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$WORK"
    cp "$REPO_ROOT/Makefile" "$REPO_ROOT/README.md" "$REPO_ROOT/pak.json" "$WORK/"
    BUMP="$REPO_ROOT/scripts/bump-dsperate.sh"
}

pinned_tag() {
    run make -C "$WORK" --no-print-directory print-DSPERATE_TAG
    output="${output#DSPERATE_TAG=}"
}

@test "a new release is pinned in the Makefile and named in the README" {
    run "$BUMP" v9.8.7 "$WORK"
    [ "$status" -eq 0 ]
    pinned_tag
    [ "$output" = "v9.8.7" ]
    grep -qF "(version 9.8.7)" "$WORK/README.md"
}

@test "only the pin and the README version change" {
    run "$BUMP" v9.8.7 "$WORK"
    [ "$status" -eq 0 ]
    run diff "$REPO_ROOT/Makefile" "$WORK/Makefile"
    [ "$(printf '%s\n' "$output" | grep -c '^[<>]')" -eq 2 ]
    run diff "$REPO_ROOT/README.md" "$WORK/README.md"
    [ "$(printf '%s\n' "$output" | grep -c '^[<>]')" -eq 2 ]
}

@test "bumping to the current pin changes nothing" {
    mk DSPERATE_TAG
    run "$BUMP" "$output" "$WORK"
    [ "$status" -eq 0 ]
    cmp "$REPO_ROOT/Makefile" "$WORK/Makefile"
    cmp "$REPO_ROOT/README.md" "$WORK/README.md"
}

@test "anything but a release tag is refused and nothing is written" {
    for bad in "" main v3.0 3.0.5 v3.0.5-rc1; do
        run "$BUMP" "$bad" "$WORK"
        [ "$status" -ne 0 ] || return 1
        [[ "$output" == *"is not a release tag"* ]] || return 1
    done
    cmp "$REPO_ROOT/Makefile" "$WORK/Makefile"
    cmp "$REPO_ROOT/README.md" "$WORK/README.md"
}

@test "a README that no longer names a version fails the bump" {
    # Otherwise the substitution silently matches nothing and the README goes
    # on advertising the old release.
    sed 's/(version [0-9.]*)//' "$REPO_ROOT/README.md" >"$WORK/README.md"
    run "$BUMP" v9.8.7 "$WORK"
    [ "$status" -ne 0 ]
    [[ "$output" == *"README.md was not updated"* ]]
}

@test "the workflow pins through the bump script" {
    wf="$REPO_ROOT/.github/workflows/update-dsperate.yaml"
    [ -f "$wf" ]
    grep -q "scripts/bump-dsperate.sh" "$wf"
    # make clone applies every patch and fails on the first that drifts; the
    # PR body reports it rather than the workflow guessing.
    grep -q "make clone" "$wf"
}
