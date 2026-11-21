# ZombievilleUSA2_nx

An unofficial Nintendo Switch wrapper for the Android version of **Zombieville
USA 2** (`com.MikaMobile.Zombieville2`, 1.6.1). It loads the game's own 32-bit
ARM Unity libraries (`libmain.so`, `libunity.so`, `libmono.so`: Unity 5.4,
Mono) and supplies what they expect from Android: libc, JNI and the game's Java
side, EGL/GLES, sound, saved data and input. It runs in AArch32 mode on the
shared **android32** runtime (`runtime/`), like SoulCalibur_nx and
AmateurSurgeon3_nx / AmateurSurgeon4_nx.

**It ships no game code and no game data.** You supply the APK of a copy you
own.

## Status: complete draft, NOT yet built or run on a console

See `NOTES.md`. The Mono JIT arena, the store-fault emulation, Mono's
cache-flush hook and Boehm GC bridge, the Java layer (about 330 answers), saved
data, touch/pad input and the Unity start-up are in, taken from the Crossy Road
port (`dcr_sea_nx`: also Unity on Mono) and adapted: the libmono offsets were
found again for this APK's Unity 5.4 build, and the guard words match that
port's byte for byte. Nothing has been compiled for the Switch (no devkitARM
here) or run: expect build errors and a first-run log to work through. Not
carried over: the Time/vsync patches of that port (Unity 5.6/2017 offsets), its
FMOD rate/voice patches and its mod/DuckTales parts. FMOD's Java output pump (`dcr_audio.c`) is in.

## Using it (players)

1. Install the NRO launcher (`zombievilleusa2_nx.nro`) to
   `sdmc:/switch/zombievilleusa2_nx/`.
2. Copy your own **APK** (any name ending in `.apk`) into that same folder, next
   to the `.nro`.
3. Create a forwarder using sphaira then launch the game using the icon on the home screen.
4. The first launch **renames the APK to `game.apk` and rewrites it
   with its assets stored** (Unity reads its data from the APK in place and would
   otherwise inflate every file at each read), unpacks the three libraries from
   it, and lists its Java classes. It needs free space for a second copy of the
   APK while it does that (about 200 MB). Later launches start the game directly.

Saved data: `data/shared_prefs/` (Android's own XML format: PlayerPrefs) and
`data/files/`. Logs: `debug/`. Settings: `config.ini`.

Controls: the touchscreen (handheld) and controllers. The game has its own gamepad
support (it hides its on-screen controls while a controller is used and shows them
again when the screen is touched); `config.ini` says which Switch buttons press
which of the game's buttons:

```ini
[gamepad]
red = Y            ; the game's three coloured buttons, left to right
green = B
yellow = A
pause = Plus       ; pause, and select in the menus
cancel = Minus     ; back in the menus
move_up = Up       ; the D-pad (the left stick moves the character too)
...
[mouse]
enabled = true     ; the right stick moves a cursor over the screen
tap = ZR           ; Switch buttons that tap at the cursor (hold to drag)
```

Several Switch buttons for one in-game button: `red = Y, ZL`. Buttons: A B X Y L R ZL ZR
Plus Minus Up Down Left Right LStick RStick. `none` leaves an input unassigned. The
file is written with every option and its explanation on the first launch; an older
file gets the new options added.

## Building (developers)

Needs Docker with the AArch32 toolchain image, libnx32 4.12.0+ (next to this
folder, or `DCR_LIBNX32`), Mesa for AArch32 in `./portlibs32/` (see
`runtime/README.md`), and `devkitpro/devkita64` for the launcher.

```sh
./build.sh              # zombievilleusa2_nx.nsp  (the 32-bit program)
launcher/build.sh       # zombievilleusa2_nx.nro  (carries the .nsp)
./build.sh DCR_GL_MESA=0   # null renderer, to check the build without Mesa
python3 runtime/tools/gen_imports.py --check   # verify source/imports.c
```

`PORT_NPDM_PROGRAM_ID` in the `Makefile` (`0x01000000000010C6`) must be unique
among your ports (AS3 `...10A3`, AS4 `...10A4`, SoulCalibur `...10C5`).

## Layout

| Path | What |
| --- | --- |
| `runtime/` | the android32 runtime (MIT), as in SoulCalibur_nx |
| `source/port_config.h` | name, package, APK rule, the Unity/Mono settings |
| `source/zv2_main.c` | `port_load` / `port_run`, in the Crossy Road port's order |
| `source/zv2_setup_plan.c` | first launch: APK stored, libraries unpacked, class list |
| `source/zv2_loader.c` | maps libmain, libunity, libmono; binds imports; constructors |
| `source/zv2_boot.c` | UnityPlayer's part: natives, surface, resume/pause, frame loop |
| `source/zv2_choreo.c` | Unity 5.4's Choreographer pacing: runs the FrameCallback per frame |
| `source/jit_arena.c`, `dcr_exc_mono.c`, `arm_store_emu.c`, `dcr_sigreturn.S` | the Mono JIT's two-view executable memory, store emulation, SIGSEGV delivery |
| `source/mono_rt.c`, `dcr_offsets.h`, `dcr_patches.c` | Mono hooks: cache flush, GC stop-the-world bridge, exception tracing |
| `source/jni_android.c`, `jni_proxy.c`, `jni_www.c` | the Java side in C (Crossy Road port's, adapted) |
| `source/dcr_prefs.c` | SharedPreferences (PlayerPrefs) kept in Android's XML |
| `source/dcr_audio.c` | FMOD's Java output device, played through audout |
| `source/zv2_gpg.c` | stand-in for Play Games' native library (sign-in reports not authorized) |
| `source/dcr_input.c` | touch, controller buttons (mapped by `[gamepad]`), the right-stick mouse |
| `source/zv2_cursor.c` | the mouse cursor, drawn in Unity's GL context before each present |
| `source/zv2_shims.c` | the four imports the runtime lacked |
| `source/imports.c` | generated import table (`runtime/tools/gen_imports.py`) |
| `tools/` | `imports.cfg`, `imports_needed.txt` (symbol names only) |
| `launcher/` | the NRO that installs the 32-bit program (placeholder icon) |

## License

MIT for this wrapper and the runtime. The game belongs to its owners; this
project contains none of it.
