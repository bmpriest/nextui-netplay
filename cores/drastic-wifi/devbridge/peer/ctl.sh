#!/bin/sh
# ctl.sh 'keys a 10' | 'shot name' ...
for c in "$@"; do
  case "$c" in
    shot\ *) echo "shot ${PEER_DIR:-.}/${c#shot }.ppm" > ${PEER_DIR:-.}/ctl ;;
    sleep\ *) sleep ${c#sleep } ;;
    *) echo "$c" > ${PEER_DIR:-.}/ctl ;;
  esac
done
