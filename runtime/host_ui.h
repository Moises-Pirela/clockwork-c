/* host_ui: small widgets shared by the desktop tools on top of render2d's
 * immediate-mode UI: a single-line text field with keyboard focus, and
 * layout helpers (rows, columns, headings). One field can be focused at a
 * time; while one is, the host should leave the keyboard alone
 * (hui_focused). */
#ifndef HOST_UI_H
#define HOST_UI_H

#include "render2d.h"

typedef struct { float x, y, w; } hui_layout;
r2d_rect hui_row(hui_layout *l, float height);                 /* a full-width row, advancing the layout */
r2d_rect hui_split(r2d_rect r, int index, int count);          /* column `index` of `count` inside a row */
void hui_heading(hui_layout *l, const char *text);
void hui_line(hui_layout *l, r2d_color color, const char *text);
bool hui_button(r2d_rect r, const char *label, bool highlighted);
void hui_label(r2d_vec2 pos, const char *text);                /* r2d_ui_label that honours the clip */

/* A vertical clip for scrolling columns: while active, rows (headings,
 * lines, buttons, text fields, labels) outside [top, bottom) are neither
 * drawn nor clickable, but layouts still advance past them. */
void hui_clip(float top, float bottom);
void hui_unclip(void);
bool hui_visible(r2d_rect r);

/* A text field editing `buffer` in place. `key` identifies the field across
 * frames (any stable pointer). Returns true when an edit was committed
 * (Enter, Tab, or a click elsewhere); Escape cancels. */
bool hui_textfield(r2d_rect r, const void *key, char *buffer, size_t capacity);
bool hui_focused(void);
const void *hui_focus_key(void);
void hui_blur(void);
void hui_frame(float dt);                                      /* once per frame, for the caret blink */

extern const r2d_color hui_yellow, hui_white, hui_red, hui_green, hui_accent, hui_plain, hui_dim;

#endif
