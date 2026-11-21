# zombievilleusa2_nx: notes

## The game, from its libraries (APK 1.6.1, supplied for analysis only)

- Unity **5.4.0b21**, **Mono** (not IL2CPP), armeabi-v7a (an x86 set is also in the APK, unused).
  Managed code: `Assembly-CSharp`, `-firstpass`, `-UnityScript`, `UnityEngine(.UI/.Networking)`,
  plus `P31RestKit` and `Photon3Unity3D`. Data: `assets/bin/Data/` (about 950 files). 913 of
  the APK's asset entries are deflated (104 MB), 41 stored: hence the stored rewrite.
- `UnityPlayerActivity` is a plain Activity (not a NativeActivity). `libmain.so` (19 KB) is the
  loader stub: JNI_OnLoad, a `/proc/cpuinfo` CPU check, `dlopen`/`dlsym`. `libunity.so` (16 MB)
  imports 481 symbols (407 after removing what libmain/libmono export), among them EGL,
  `ANativeWindow_*`, `ASensor*`, `ALooper_*`; no OpenSL symbols directly: FMOD (inside libunity)
  `dlopen`s `libOpenSLES.so` and has an AudioTrack output too (`disableOpenSL`).
- `libgpg.so` (Google Play Games native SDK) is **not loaded**. Play Games, Firebase, Unity Ads,
  Photon and P31RestKit reach Java through Mono's JNI and get neutral answers.
- `libunity`'s JNI_OnLoad registers these natives (read from its RegisterNatives table):

  | UnityPlayer native | signature |
  | --- | --- |
  | initJni | (Landroid/content/Context;)V |
  | nativeFile | (Ljava/lang/String;)V |
  | nativeRecreateGfxState | (ILandroid/view/Surface;)V |
  | nativeRender | ()Z |
  | nativeResume / nativeDone | ()V |
  | nativePause | ()Z |
  | nativeFocusChanged | (Z)V |
  | nativeInjectEvent | (Landroid/view/InputEvent;)Z |
  | nativeSetInputString / nativeSoftInputClosed / nativeSetInputCanceled | (String)V / ()V / (Z)V |
  | nativeInitWWW / nativeInitWebRequest | (Ljava/lang/Class;)V |
  | nativeVideoFrameCallback | (I[BII)V |
  | nativeActivityIndicatorStyle | ()I |
  | nativeUnitySendMessage | (String;String;String)V |
  | nativeProxyInvoke / nativeProxyFinalize | (ILjava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object; / (I)V |

- The Java classes libunity looks up (names only): Activity, Context, SharedPreferences(+Editor),
  PackageManager/Info, Display/DisplayMetrics/WindowManager, InputDevice(+MotionRange),
  MotionEvent/KeyEvent/InputEvent, AudioManager, ConnectivityManager/NetworkInfo, Location*,
  Vibrator, Handler/Looper/HandlerThread, Choreographer, AlertDialog$Builder, Settings$Secure/System,
  `java/lang/reflect/Method`, `com/unity3d/player/{UnityPlayer,ReflectionHelper}`.

## What differs from the three example ports

1. **Unity, not NativeActivity/cocos2d-x.** The activity part is `UnityPlayer`'s (`zv2_boot.c`):
   `initJni`, `nativeFile(apk)`, `nativeRecreateGfxState`, `nativeResume`, then the `nativeRender`
   loop on the main thread ("UnityMain"). libunity makes its own EGL context; there is no `*_egl.c`.
2. **Three libraries**, loaded libmono, libmain, libunity...: here in the APK's order main, unity, mono,
   all bound together (libunity imports libmono's exports), constructors in the same order.
3. **The APK is the data.** The runtime's APK role renames it to `game.apk`; the setup plan rewrites
   it with assets stored.
4. **Mono's JIT** needs what the runtime only has the hooks for. It comes from the Crossy Road port
   (`dcr_sea_nx`), hardware path only:
   - `jit_arena.c`: `jitCreate` code memory, two views (rx / rw), a page allocator, the runtime's
     `cs_mmap/cs_munmap/cs_mprotect/cs_write/cs_rw_alias` and `port_code_flush`, and a boot-time
     self-test (store emulated, code runs, rewritten code runs, SIGSEGV delivered).
   - `dcr_exc_mono.c` + `arm_store_emu.c`: the data-abort hook that completes the JIT's stores on rw,
     and Linux-style SIGSEGV delivery to Mono's handler (`dcr_sigreturn.S`).
   - `mono_rt.c`: `mono_arch_flush_icache` -> `jit_flush`; the Boehm GC stop-the-world bridge
     (`port_gc_signal`); names for managed frames in logs and for `NullReferenceException`.
5. **libmono offsets (`dcr_offsets.h`)** were found by hand in this APK's libmono (no capstone
   here): `mono_arch_flush_icache` +0x10e0f0 (the `svc #0`, r7 = 0xF0002, is at +0x10e134);
   `GC_suspend_handler` +0x2bc5d4 (calls `sem_post`, `sigsuspend`; loads `GC_stop_count` through GOT
   slot +0x392be4 and `GC_suspend_ack_sem` through +0x392bec; its `stop_info` stores at +0xc, +0x10,
   +0x14; signals 30 and 24); `GC_lookup_thread` +0x2bb3dc (exported). All three guards are identical to
   Crossy Road's, so the code is the same Mono. They are checked at run time and skipped with a log
   line on mismatch.
6. **Frame pacing**: Unity 5.4 prints "Choreographer available: Enabling VSYNC timing" and waits on a
   counter that a Java `Choreographer.FrameCallback` raises. libunity builds that callback as an
   interface proxy itself, so `zv2_choreo.c` keeps it from `postFrameCallback` and runs its
   `doFrame(long)` once per frame before `nativeRender`. This replaces the Crossy Road port's
   offsets-based vsync pump and Choreographer patches (Unity 5.6/2017 RVAs, not applicable here).
7. **Java side**: the Crossy Road port's `jni_android.c` (about 330 answers: Context, Activity, files,
   SharedPreferences, display, input devices, MotionEvent/KeyEvent, Handler/Looper, reflection,
   JNIBridge/ReflectionHelper proxies), with its Fabric application class and FMOD Java device
   removed. `jni_www.c` answers the Java WWW downloader.
8. Settings follow the runtime's notes for that port: `RT_EXTRA_ENV "MONO_DEBUG=explicit-null-checks"`,
   `RT_GL_SWAP_ENDS_FRAME 0`, `RT_MAIN_THREAD_NAME "UnityMain"`, `RT_JNI_UNHANDLED_*`,
   `RT_LOOPER_MAIN_IMPLICIT 0`, `RT_DIRCACHE 0`, `RT_EMU_FIXUPS 0`. FMOD takes its Java output
   (`org.fmod.FMODAudioDevice`), played by `dcr_audio.c` through audout, with `RT_DL_HAS_OPENSLES 0`.

## What is NOT done, most important first

1. **Never compiled for the Switch, never run.** Checked on a host: `gen_imports.py --check` (407
   imports, 0 missing; the runtime lacked `isnan`, `pthread_attr_setschedpolicy`,
   `ANativeWindow_lock`, `ANativeWindow_unlockAndPost`: `zv2_shims.c`); a syntax pass of the new
   files against stub headers; a cross-reference of every `dcr_/jit_/zv2_/jni_/rt_/port_/cs_/b_`
   function the sources call against what the project and runtime define. Not checked: the
   devkitARM/libnx32/Mesa build, the link, any run. The files taken from Crossy Road were built
   there; the changes to them (FMOD out, Choreographer in, the arena without its emulator code, the
   config and names) are the new risk.
2. **The JIT arena needs a console to confirm**: Atmosphere with user exception handlers. Its
   self-test logs four `[jit] self-test N: OK/FAILED` lines before Mono runs.
3. **Choreographer pump is a theory.** If `[choreo] first postFrameCallback` says "NOT a proxy" the
   callback is an ordinary Java object and the pump does nothing; the first frame then waits for
   ever. The fallbacks: find `WaitVSync` and its counter in libunity and raise it as the Crossy
   Road port's `dcr_vsync.c` does, or patch the Choreographer enable function to `bx lr`.
4. **Unity time freeze across HOME/sleep**: Crossy Road hooks 15 Time sites (Unity 5.6.4 RVAs) so
   the engine clock stops while suspended; not done here. The runtime stops the process clocks, so
   after a long HOME the game may see a large `deltaTime` once.
5. **Audio**: first console log showed FMOD choosing its Java output (no `libOpenSLES.so` dlopen), so
   `dcr_audio.c` (Crossy Road's pump) is in and OpenSL ES is off. Its rate/voice patches (`dcr_fmod.c`)
   are Unity 5.6 offsets and are not carried over; this FMOD mixes at its own default rate and
   `dcr_audio.c` resamples to 48 kHz.
6. **Controllers** are exposed as one gamepad (the game is touch-only); nothing uses them.
7. Not done: soft keyboard (`nativeSetInputString`), video, Play Games, ads, store (Unity Ads,
   Photon, P31RestKit, Firebase fail quietly offline). `config.ini [debug] log_java_calls = true`
   lists each `[jni] unhandled ...` call.
8. `PORT_NPDM_PROGRAM_ID 0x01000000000010C6` is a guess at a free one.

## First console log (2026-11-20): what it showed

Setup, the three libraries (0 unresolved imports), the JIT self-tests 1-4, the GC bridge, JNI natives,
Mono start-up, the managed assemblies, EGL/GLES on Mesa and three presented frames all worked. Then
every thread parked: libunity creates a Java `HandlerThread`, a `Handler(looper, Handler.Callback)`
and sends `Message`s (`obtainMessage(what)`, `sendToTarget()`), whose `handleMessage` is a proxy into
native code, and waits for the result. Nothing ran that loop. Fixed in `jni_proxy.c`: a real
HandlerThread (own thread and queue), `Message` (fields `what`, `arg1`, `arg2`, `target`),
`Handler.sendMessage*`, `Looper.quit`. Also added: `Settings.Secure.ANDROID_ID`.

## Second console log: the logo scene

The HandlerThread fix worked: first picture after ~3 s, 60 fps (frames 600/1200/1800 at 58-60 fps),
the Choreographer pump ran (one doFrame per frame, as a proxy), FMOD's Java output ran through
`dcr_audio.c` (0 underruns), PlayerPrefs loaded. The game stayed on its logo scene because
`Startup.Start()` calls Play Games' `Authenticate`, whose native library `gpg` (the APK's libgpg.so)
is not loaded: `DllNotFoundException: gpg` aborted `Startup.Start`, so nothing after it ran.

Fixed with `zv2_gpg.c`: a built-in stand-in for that library, reached through new weak hooks in the
runtime's `bionic_dl.c` (`port_dl_builtin`, `port_dl_owns`, `port_dl_builtin_sym`). The 507 P/Invoke
names and signatures, the callback delegate signatures and the enum values were read from the game's
own assembly metadata (the parser was a throwaway script; none of the game's code or data is used).
Sign-in finishes at once with `ERROR_NOT_AUTHORIZED`; every other function returns 0. What to look
for in the next log: `[gpg] dlopen`, `[gpg] StartAuthorizationUI`, then the game moving on; and the
`[gpg] dlsym(...)` lines, which are the plugin functions the game actually reaches.
The real libgpg.so would need Google Play services (Java), which does not exist here.

## Controllers (third log round)

The game reads Unity's InputManager, read from its `globalgamemanagers` (this is where the
bindings come from; nothing else of that file is used): `Fire1`/`Fire2`/`Fire3` = keys 1/2/3 or
joystick buttons 15/14/13, `Horizontal` = keys A/D or joystick buttons 7/5 or the stick (axis 0,
dead zone 0.19), `Vertical` = keys S/W or buttons 6/4 or axis 1, `Pause` = joystick button 0,
`Submit` = Return/Enter/Space or joystick button 0, `Cancel` = Escape or joystick button 1. The game's
`MotherBrain.handleJoysticks` reads them. The old mapping sent Switch A as Android BUTTON_A =
joystick button 0 = Pause, which is why A paused, and sent Y/B/X to buttons the game does not read.

New mapping (`dcr_input.c`, `[gamepad]` in config.ini): Fire1/2/3 are sent as the keyboard keys 1/2/3
(KEYCODE_1..3, source KEYBOARD, device -1), because Unity's Android joystick-button numbering
(15/14/13) could not be read out of libunity reliably and the keys are unambiguous; the D-pad is
sent as keys W/A/S/D; Pause/Cancel as the gamepad's BUTTON_A/BUTTON_B (joystick 0/1); the left stick
as the joystick axes as before. Defaults: Y = red (Fire1), B = green (Fire2), A = yellow (Fire3),
Plus = pause, Minus = cancel. **The order of the colours is an assumption** (left to right = Fire1,
2, 3); if a button does the wrong thing, swap the names in `[gamepad]`.

Mouse: the right stick moves a cursor in view pixels; the `[mouse] tap` buttons put a synthetic
finger (pointer id 100) down at it, move it while held, and lift it. A real finger on the screen
hides the cursor. `zv2_cursor.c` draws the arrow in `port_gl_before_swap` (the runtime calls it
inside eglSwapBuffers on the render thread) and restores the GL state it touched (program, buffer,
vertex attributes 0/1, texture 0, blend/depth/stencil/scissor/cull, colour mask, viewport,
framebuffer) -- the approach of abspace_nx's overlay, reduced to triangles. Needs confirming on a
console that Unity's next frame is unaffected. `[debug] log_touches = true` logs every touch and
cursor tap with its position, which also gives the positions of the on-screen controls.

CPU boost is off by default now (`[performance] boost_cpu_when_loading`); config version 2 moves a
file that still holds the old default (true) to false.

## Crashes (third log round)

`debug_prev.log` (a 9.7 minute session, frame 34800) and `debug_2ce1f6.log` (frame 3000) both end
with no crash text: the process ended between two log flushes. Everything the runtime knows at a
fault goes to `crash.log` (and the tail of the log ring with it), so that file is what is needed.
What the logs do show: steady 58-60 fps, GC suspend counts growing normally (0 non-GC threads),
the JIT arena in use well under its 32 MB, Photon retrying its connection every ~10 s (DNS failures
raised and caught as managed exceptions) for the whole session. Nothing in them points at a cause.
The periodic `[boot] frame ...` line now also reports live Java objects and JIT arena KB used, so a
leak or an exhausted arena shows as a trend in the next long log.

## First-run checklist (debug.log)

- `[setup]` lines: the APK rewritten stored, three libraries unpacked, classes listed.
- `[boot] libmain.so / libunity.so / libmono.so ... (0 unresolved imports ...)`, `[boot] modules mapped`.
- `[mono] GC bridge armed`, no `GUARD MISMATCH`; `[jit] arena ... rx ... rw ...` and four self-tests OK.
- `[boot] module constructors done`, `[jni] RegisterNatives ...`, no `MISSING required native`.
- `[boot] nativeRecreateGfxState`, GL lines, `[choreo] first postFrameCallback`, `first frame presented`.
- `[exc] ... thrown in:` lines name the C# method behind a managed exception.
- If it stops early, keep `debug.log` and `crash.log`.

## Legal

No game file is in this project, and `.gitignore` keeps them out. `tools/imports_needed.txt`
holds symbol names only; `dcr_offsets.h` holds addresses and instruction guard words of libmono (an interface, not game content) (an interface, not game content). The native-method table above is
interface names and signatures. The launcher's icon is a placeholder made for this project.
