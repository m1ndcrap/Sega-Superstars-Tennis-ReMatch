#!/bin/sh
# Starts SST Rematch from its own folder (so it finds assets/, shaders/ and its libraries wherever it is launched from)
here=$(dirname "$(readlink -f "$0")")
cd "$here" || exit 1
exec ./sst_rematch "$@"