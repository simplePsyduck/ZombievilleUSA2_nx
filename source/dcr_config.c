/* dcr_config.c -- Zombieville USA 2's config.ini, on the runtime's INI engine
 * (runtime/source/rt_cfg.c). MIT.
 *
 * [gamepad] names the game's own controller inputs and gives each the Switch
 * buttons that press it, as a comma separated list ("Y, ZL"): the same Switch
 * button may appear under several in-game buttons, and one in-game button may
 * have several Switch buttons. [mouse] is the right stick as a pointer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

#include "dcr_config.h"
#include "rt_cfg.h"
#include "util.h"

static DcrConfig g_cfg = {.touch = 1, .res_w = 1280, .res_h = 720, .boost = 0, .left_stick_moves = 1,
                          .mouse = 1, .mouse_speed = 1.0f, .mouse_deadzone = 0.15f, .mouse_cursor = 1,
                          .mouse_size = 1.0f, .mouse_hide_s = 3.0f};

const DcrConfig *dcr_config(void) { return &g_cfg; }

/* the text of each list, kept until apply() turns it into bits */
static char t_tap[64], t_capture[24], t_stick[8];

#define SWITCH_BUTTONS \
  "A, B, X, Y, L, R, ZL, ZR, Plus, Minus, Up, Down, Left, Right, LStick, RStick; Up/Down/Left/Right are the D-pad"
#define GP_ROW(key, def, help) \
  {"gamepad", key, def, help, CFG_TEXT, NULL, NULL, 0, 0, 0, 0}

static const CfgOpt k_opts[] = {
    {"game", "language", "auto",
     "auto (the console's language) or a code such as en, fr, de, es, it, pt, ja, ko, zh.",
     CFG_TEXT, NULL, NULL, 0, 0, 0, 0},
    {"controls", "touchscreen", "true",
     "The touchscreen plays the game (handheld), as on a phone. The game hides its\n"
     "# on-screen controls while a controller is used and shows them again when the\n"
     "# screen is touched.",
     CFG_BOOL, NULL, &g_cfg.touch, 0, 0, 0, 0},

    /* ---- [gamepad] ---- */
    GP_ROW("red", "Y",
           "The game's controller buttons, and the Switch buttons that press each one.\n"
           "# Switch buttons: " SWITCH_BUTTONS ".\n"
           "# Give one in-game button several Switch buttons with a comma (red = Y, ZL), and\n"
           "# the same Switch button to several in-game buttons if you like. none = unassigned.\n"
           "#\n"
           "# red, green, yellow: the game's three coloured buttons, left to right."),
    GP_ROW("green", "B", NULL),
    GP_ROW("yellow", "A", NULL),
    GP_ROW("pause", "Plus",
           "pause: pauses the game, and picks the highlighted item in the menus."),
    GP_ROW("cancel", "Minus", "cancel: goes back in the menus."),
    GP_ROW("move_up", "Up", "The D-pad moves the character like the left stick (the stick itself:\n"
                            "# left_stick_moves below)."),
    GP_ROW("move_down", "Down", NULL),
    GP_ROW("move_left", "Left", NULL),
    GP_ROW("move_right", "Right", NULL),
    {"gamepad", "left_stick_moves", "true",
     "The left stick moves the character and the menu cursor.",
     CFG_BOOL, NULL, &g_cfg.left_stick_moves, 0, 0, 0, 0},

    /* ---- [mouse] ---- */
    {"mouse", "enabled", "true",
     "The right stick moves a cursor over the screen, and the tap buttons below touch\n"
     "# it, as a finger would (hold one to drag). Touching the screen with the cursor\n"
     "# makes the game show its on-screen controls, as a real touch does.",
     CFG_BOOL, NULL, &g_cfg.mouse, 0, 0, 0, 0},
    {"mouse", "stick", "right", "Which stick moves the cursor: right or left.", CFG_TEXT, NULL, t_stick, 0, 0,
     sizeof t_stick, 0},
    {"mouse", "tap", "ZR",
     "Switch buttons that tap at the cursor (a comma separated list, as above).",
     CFG_TEXT, NULL, t_tap, 0, 0, sizeof t_tap, 0},
    {"mouse", "speed", "1.0", "Cursor speed: 1.0 crosses the screen in about 1.5 s at full tilt.", CFG_FLOAT,
     NULL, &g_cfg.mouse_speed, 0.2f, 5.0f, 0, 0},
    {"mouse", "deadzone", "0.15", "Stick travel ignored around the centre (0 to 0.5).", CFG_FLOAT, NULL,
     &g_cfg.mouse_deadzone, 0.0f, 0.5f, 0, 0},
    {"mouse", "show_cursor", "true", "Draw the cursor.", CFG_BOOL, NULL, &g_cfg.mouse_cursor, 0, 0, 0, 0},
    {"mouse", "cursor_size", "1.0", "Size of the cursor.", CFG_FLOAT, NULL, &g_cfg.mouse_size, 0.5f, 3.0f, 0, 0},
    {"mouse", "hide_after", "3",
     "Seconds without moving or tapping before the cursor hides (0: never).",
     CFG_FLOAT, NULL, &g_cfg.mouse_hide_s, 0.0f, 60.0f, 0, 0},

    {"display", "resolution", "720", CFG_HELP_RESOLUTION, CFG_CHOICE, "720,1080,auto", NULL, 0, 0, 0, 0},
    {"performance", "boost_cpu_when_loading", "false",
     "CPU at 1785 MHz while the game starts (until its first picture) and\n"
     "# inside loading frames (those over 50 ms), normal otherwise. Off by default:\n"
     "# it uses more battery.",
     CFG_BOOL, NULL, &g_cfg.boost, 0, 0, 0, 0},
    CFG_ROW_GL_SELFTEST(&g_cfg.gl_selftest),
    CFG_ROW_BOOT_LOG(CFG_HELP_BOOT_LOG, &g_cfg.boot_log),
    CFG_ROW_LOG_JNI(CFG_HELP_LOG_JNI, &g_cfg.log_jni),
    {"debug", "log_touches", "false",
     "Write every touch and cursor tap (with its position) to debug.log: how to read the\n"
     "# positions of the game's on-screen controls.",
     CFG_BOOL, NULL, &g_cfg.log_touch, 0, 0, 0, 0},
    {"debug", "screenshot_button", "none",
     "A Switch button that saves the next frame as capture-NNN.bmp in the game folder (none: off).",
     CFG_TEXT, NULL, t_capture, 0, 0, sizeof t_capture, 0},
};

#define NROWS CFG_COUNT(k_opts)

static const struct { const char *name; uint64_t bit; } k_names[] = {
    {"a", HidNpadButton_A},         {"b", HidNpadButton_B},          {"x", HidNpadButton_X},
    {"y", HidNpadButton_Y},         {"l", HidNpadButton_L},          {"r", HidNpadButton_R},
    {"zl", HidNpadButton_ZL},       {"zr", HidNpadButton_ZR},        {"plus", HidNpadButton_Plus},
    {"+", HidNpadButton_Plus},      {"minus", HidNpadButton_Minus},  {"-", HidNpadButton_Minus},
    {"up", HidNpadButton_Up},       {"down", HidNpadButton_Down},    {"left", HidNpadButton_Left},
    {"right", HidNpadButton_Right}, {"dup", HidNpadButton_Up},       {"ddown", HidNpadButton_Down},
    {"dleft", HidNpadButton_Left},  {"dright", HidNpadButton_Right}, {"lstick", HidNpadButton_StickL},
    {"stickl", HidNpadButton_StickL}, {"l3", HidNpadButton_StickL},  {"rstick", HidNpadButton_StickR},
    {"stickr", HidNpadButton_StickR}, {"r3", HidNpadButton_StickR},
};

/* "Y, ZL" -> bits. An unknown word is logged and skipped. */
static uint64_t parse_buttons(const char *what, const char *text) {
  uint64_t bits = 0;
  char buf[96];
  snprintf(buf, sizeof buf, "%s", text ? text : "");
  for (char *save = NULL, *tok = strtok_r(buf, ",; \t", &save); tok; tok = strtok_r(NULL, ",; \t", &save)) {
    if (!strcasecmp(tok, "none") || !strcasecmp(tok, "off"))
      continue;
    int ok = 0;
    for (unsigned i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
      if (!strcasecmp(tok, k_names[i].name)) {
        bits |= k_names[i].bit;
        ok = 1;
        break;
      }
    if (!ok)
      debugPrintf("[config] %s: \"%s\" is not a Switch button (A B X Y L R ZL ZR Plus Minus Up Down Left Right LStick RStick): ignored\n",
                  what, tok);
  }
  return bits;
}

static void apply(void) {
  static const char *const gp_keys[GP_COUNT] = {"red",     "green",     "yellow",    "pause",    "cancel",
                                                 "move_up", "move_down", "move_left", "move_right"};
  const RtConfig *rt = rt_config();
  g_cfg.res_w = rt->res_w;
  g_cfg.res_h = rt->res_h;
  const char *lang = rt_config_get("game", "language");
  g_cfg.locale[0] = 0;
  if (lang && strcasecmp(lang, "auto"))
    snprintf(g_cfg.locale, sizeof g_cfg.locale, "%s", lang);
  for (int i = 0; i < GP_COUNT; i++) {
    char what[40];
    snprintf(what, sizeof what, "[gamepad] %s", gp_keys[i]);
    g_cfg.gp_map[i] = parse_buttons(what, rt_config_get("gamepad", gp_keys[i]));
  }
  g_cfg.mouse_tap = parse_buttons("[mouse] tap", rt_config_get("mouse", "tap"));
  g_cfg.capture_btn = parse_buttons("[debug] screenshot_button", rt_config_get("debug", "screenshot_button"));
  const char *st = rt_config_get("mouse", "stick");
  g_cfg.mouse_stick = st && !strcasecmp(st, "left") ? 1 : 0;
  debugPrintf("[config] language %s; %dx%d; touch %s; CPU boost %s; mouse %s (%s stick)\n",
              g_cfg.locale[0] ? g_cfg.locale : "auto", g_cfg.res_w, g_cfg.res_h, g_cfg.touch ? "on" : "off",
              g_cfg.boost ? "on" : "off", g_cfg.mouse ? "on" : "off", g_cfg.mouse_stick ? "left" : "right");
  debugPrintf("[config] gamepad: red %llx green %llx yellow %llx pause %llx cancel %llx (Switch button bits)\n",
              (unsigned long long)g_cfg.gp_map[GP_RED], (unsigned long long)g_cfg.gp_map[GP_GREEN],
              (unsigned long long)g_cfg.gp_map[GP_YELLOW], (unsigned long long)g_cfg.gp_map[GP_PAUSE],
              (unsigned long long)g_cfg.gp_map[GP_CANCEL]);
}

/* Version 2: CPU boost is off by default now. A file written by version 1 with
 * the old default (true) is moved to the new one; a player who set it on
 * themselves in version 1 can set it again. */
static const CfgMigrate k_migrate[] = {
    {"performance", "boost_cpu_when_loading", "true", "false", 2},
};

static const CfgTable k_table = {
    .opts = k_opts,
    .nopts = NROWS,
    .migrate = k_migrate,
    .nmigrate = CFG_COUNT(k_migrate),
    .version = 2,
    .apply = apply,
};

void dcr_config_load(void) { rt_config_load(&k_table); }
