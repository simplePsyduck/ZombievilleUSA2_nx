#!/bin/sh
# Build zombievilleusa2_nx.nsp in the AArch32 toolchain container (the runtime's
# docker_build.sh: arguments go to make, e.g. ./build.sh clean, DCR_GL_MESA=0).
# libnx32 is found next to this folder (../libnx32/prefix), or where DCR_LIBNX32
# says. Mesa goes in ./portlibs32/ (lib/, include/).
exec "$(dirname "$0")/runtime/tools/docker_build.sh" "$@"
