#!/bin/sh
# Test fixture: top DS screen above bottom, with original settings restored.
set -eu
saved=/tmp/netplay-suite-ds-layout
case ${1:-} in
set)
    [ ! -e "$saved" ] || { echo 'pending DS layout restore' >&2; exit 1; }
    mkdir -p "$saved"
    : > "$saved/files"
    for file in /mnt/SDCARD/Saves/dsperate/games/mariokart.ini \
        /mnt/SDCARD/Emu/NDS/config/drastic-Brick.cfg \
        /mnt/SDCARD/Emu/NDS/config/drastic-AnbernicXX640480.cfg \
        /mnt/SDCARD/Emu/NDS/config/drastic.cfg; do
        backup=$saved/$(basename "$file")
        if [ -f "$file" ]; then
            cp "$file" "$backup"
            printf 'present|%s|%s\n' "$file" "$backup" >> "$saved/files"
        else
            printf 'absent|%s|%s\n' "$file" "$backup" >> "$saved/files"
            [ "$file" = /mnt/SDCARD/Saves/dsperate/games/mariokart.ini ] || continue
            mkdir -p "$(dirname "$file")"
            : > "$file"
        fi
        case $file in
        *.ini)
            awk '
                /^\[/ { video=($0 == "[video]"); print; if (video) { found=1; print "layout = vertical\nscreen = top" }; next }
                video && /^[[:space:]]*(layout|screen)[[:space:]]*=/ { next }
                { print }
                END { if (!found) print "[video]\nlayout = vertical\nscreen = top" }
            ' "$file" > "$file.suite-tmp"
            ;;
        *.cfg)
            sed -E 's/^screen_orientation[[:space:]]*=.*/screen_orientation = 0/; s/^screen_swap[[:space:]]*=.*/screen_swap = 0/' "$file" > "$file.suite-tmp"
            ;;
        esac
        mv "$file.suite-tmp" "$file"
    done
    echo 'DS test layout: vertical, top screen above bottom screen'
    ;;
restore)
    [ -f "$saved/files" ] || exit 0
    while IFS='|' read -r state file backup; do
        if [ "$state" = present ]; then cp "$backup" "$file"; else rm -f "$file"; fi
    done < "$saved/files"
    rm -rf "$saved"
    echo 'DS layouts restored'
    ;;
*) exit 2 ;;
esac
