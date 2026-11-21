/* zv2_setup_plan.c -- Zombieville USA 2's part of the first launch: its plan
 * for the runtime's dcr_setup.c, which does the work.
 *
 * The bar, in permille of the whole first launch:
 *     0- 800  the APK rewritten with its assets stored (by bytes written)
 *   800- 820  the rewritten copy checked
 *   820- 940  libmain / libunity / libmono unpacked (by bytes)
 *   940- 990  the Java class list (classes.dex)
 *        1000 the game starts
 *
 * .setup keys, which must not change (or every player unpacks again once):
 * libmain.so, libunity.so, libmono.so, classes.txt. MIT.
 */
#include "dcr_setup.h"

static const char *const k_libs[] = {"libmain.so", "libunity.so", "libmono.so"};

const RtSetupPlan port_setup_plan = {
    .libs = k_libs,
    .nlibs = 3,
    .libs_what = "Unpacking the game",
    .apk_requirement = "This port needs the Android APK of Zombieville USA 2\n"
                       "(com.MikaMobile.Zombieville2, armeabi-v7a): use your own copy.",
    .store_apk_prefix = "assets/",
    .store_p0 = 0,
    .store_p1 = 800,
    .libs_p0 = 820,
    .libs_p1 = 940,
    .classes_p0 = 940,
    .classes_p1 = 990,
};
