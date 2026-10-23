#!/bin/sh
# Install and restore the mGBA pak paired with Netplay's instanced core.
# The implementation is shared with DSP.pak; see pak-manage.sh.
exec sh "$(dirname "$0")/pak-manage.sh" mgba "$@"
