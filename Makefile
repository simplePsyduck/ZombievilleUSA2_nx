#---------------------------------------------------------------------------------
# ZombievilleUSA2_nx -- Nintendo Switch wrapper (32-bit / AArch32)
#
# Ships NO game code and NO game data: the player's own APK (any name) is read
# at run time; libmain/libunity/libmono are unpacked from it on the first
# launch and the game's data is read from the APK itself.
#
# The build is the android32 runtime's (runtime/runtime.mk: devkitARM +
# libnx32 + mesa32 from portlibs32/); ./build.sh runs it in the toolchain
# container. Output: zombievilleusa2_nx.nsp, which the launcher NRO carries.
#---------------------------------------------------------------------------------
TARGET               := zombievilleusa2_nx
# One id per port: change it if it clashes with another port's
# (AS3 ...10A3, AS4 ...10A4, SoulCalibur ...10C5).
PORT_NPDM_PROGRAM_ID := 0x01000000000010C6
include runtime/runtime.mk

.PHONY: check
check:
	@echo "run on the host: python3 runtime/tools/gen_imports.py --check"
