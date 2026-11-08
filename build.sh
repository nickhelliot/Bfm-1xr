#!/bin/sh
# Build BFMXR: app/build/bfmxr.fwsc (needs JieLi's Linux toolchain and AC79 SDK: see base/README.md).
#   ./build.sh                  the firmware
#   ./build.sh --host           the simulator on this computer: app/build/host/bfmxr_host app/tests/basic.txt app/build/host
#   ./build.sh --release X.Y    a release package
set -e
cd "$(dirname "$0")"
exec python3 base/tools/build.py app "$@"
