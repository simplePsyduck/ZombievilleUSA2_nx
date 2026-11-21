/* config.h -- Zombieville USA 2's own constants. The runtime's settings are in
 * port_config.h (runtime/source/rt_settings.h). MIT.
 *
 * The game: com.MikaMobile.Zombieville2 1.6.1, Unity 5.4.0b21 with Mono (not
 * IL2CPP), armeabi-v7a. UnityPlayerActivity (a plain Activity, not a
 * NativeActivity) loads libmain.so and libunity.so; libunity registers its
 * natives on com/unity3d/player/UnityPlayer with RegisterNatives in JNI_OnLoad
 * and reads its data from the APK itself. libgpg.so (Google Play Games) is not
 * loaded. */
#ifndef ZV2_CONFIG_H
#define ZV2_CONFIG_H
#include "rt_settings.h"

/* the names the files taken from the Crossy Road port use (DCR_*) */
#define DCR_LIB_MAIN    "libmain.so"
#define DCR_LIB_UNITY   "libunity.so"
#define DCR_LIB_MONO    "libmono.so"
#define DCR_DATA_DIR    "assets/bin/Data"
#define DCR_MANAGED_DIR "assets/bin/Data/Managed"
#define DCR_PKG_NAME    PORT_PACKAGE

#define ZV2_ACTIVITY "com/unity3d/player/UnityPlayerActivity"
#define ZV2_PLAYER   "com/unity3d/player/UnityPlayer"
#define ZV2_LIB_MAIN  DCR_LIB_MAIN
#define ZV2_LIB_UNITY DCR_LIB_UNITY
#define ZV2_LIB_MONO  DCR_LIB_MONO
#endif
