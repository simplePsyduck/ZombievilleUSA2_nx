#!/bin/sh
# Build zombievilleusa2_nx.nro (the launcher) with the runtime's launcher build
# (devkitPro's 64-bit toolchain container). Build the wrapper first
# (../build.sh): the NRO carries ../zombievilleusa2_nx.nsp and its .build file.
HERE="$(cd "$(dirname "$0")" && pwd)"
LAUNCHER_DIR="$HERE" PAYLOAD=zombievilleusa2_nx exec "$HERE/../runtime/launcher/build.sh" "$@"
