/* dcr_config.h -- the user's settings, from <game folder>/config.ini (dcr_config.c). */
#ifndef DCR_USER_CONFIG_H
#define DCR_USER_CONFIG_H
#include <stdint.h>

/* The game's own controller inputs (what its scripts read: the InputManager's
 * Fire1/2/3, Horizontal/Vertical, Pause and Cancel). Each one is pressed by any
 * of the Switch buttons assigned to it in [gamepad]. */
enum {
  GP_RED,    /* Fire1: the first coloured button  (the game binds key "1") */
  GP_GREEN,  /* Fire2: the second                 (key "2") */
  GP_YELLOW, /* Fire3: the third                  (key "3") */
  GP_PAUSE,  /* Pause, and Submit in the menus    (joystick button 0) */
  GP_CANCEL, /* Cancel in the menus               (joystick button 1) */
  GP_UP,     /* Vertical +  (keys W / joystick button 4) */
  GP_DOWN,   /* Vertical -  (S) */
  GP_LEFT,   /* Horizontal - (A) */
  GP_RIGHT,  /* Horizontal + (D) */
  GP_COUNT
};

typedef struct {
  char locale[8];   /* [game] language: "en", "fr"... ("" = the console's language) */
  int touch;        /* [controls] touchscreen */
  int res_w, res_h; /* [display] resolution */
  int boost;        /* [performance] boost_cpu_when_loading */
  int gl_selftest;  /* [debug] gl_selftest */
  int boot_log;     /* [debug] boot_log_on_screen */
  int log_jni;      /* [debug] log_java_calls */
  int log_touch;    /* [debug] log_touches */

  /* [gamepad]: for each in-game input, the set of Switch buttons (HidNpadButton
   * bits) that press it; 0 = unassigned */
  uint64_t gp_map[GP_COUNT];
  int left_stick_moves;  /* [gamepad] left_stick_moves */
  uint64_t capture_btn;  /* [debug] screenshot_button (0 = none) */

  /* [mouse]: the right stick as a pointer for the touchscreen */
  int mouse;             /* enabled */
  int mouse_stick;       /* 0 right, 1 left */
  float mouse_speed;     /* multiplier */
  float mouse_deadzone;
  uint64_t mouse_tap;    /* Switch buttons that tap at the cursor (hold: drag) */
  int mouse_cursor;      /* draw the cursor */
  float mouse_size;      /* cursor size multiplier */
  float mouse_hide_s;    /* seconds idle before the cursor hides (0: never) */
} DcrConfig;

/* Read config.ini (writing it with the defaults first). Early in main(). */
void dcr_config_load(void);
const DcrConfig *dcr_config(void);

#endif
