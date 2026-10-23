#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

echo "== instanced-core UI capability map"

grep -q 'NS_INST_CORE\[NS_INST_CORES\].*{ "gambatte", "mgba" }' app/netsetup.c
if grep -q 'NS_INST_IMPLEMENTED\|inst_gpsp\|"gpsp".*"mgba"' app/netsetup.c app/netsetup.h app/main.c; then
	echo "  MISS unsupported gpSP/planned-core UI remains"
	exit 1
fi

echo "  ok   only implemented paired cores (Gambatte and mGBA) are selectable"
