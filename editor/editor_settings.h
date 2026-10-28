/* editor_settings: the editor's own preferences (not the project's), kept
 * per user in editor.cfg: view defaults, grid and snapping, nudge step,
 * behaviour. They are described by a tunables table (render2d/r2d_tunables.h)
 * like the rendering settings, so the settings panel renders them
 * generically and the file is hand-editable.
 *
 * Location: CW_EDITOR_CONFIG, else <user config dir>/clockwork/editor.cfg
 * (XDG_CONFIG_HOME or ~/.config on POSIX, APPDATA on Windows). */
#ifndef EDITOR_SETTINGS_H
#define EDITOR_SETTINGS_H

#include "r2d_tunables.h"

typedef struct {
    float text_scale;          /* UI glyph size multiplier */
    bool grid;                 /* show the grid when a project opens */
    float grid_spacing;        /* world units between grid lines (snap uses it too) */
    bool snap;                 /* snap placement, nudges and drags to the grid */
    float nudge;               /* arrow-key step in world units (shift x10) */
    bool preview_rendering;    /* start with the game's rendering stack on */
    bool confirm_discard;      /* ask twice before discarding unsaved scene edits */
    float zoom_step;           /* wheel zoom factor per notch */
    bool open_last_scene;      /* reopen the project's main scene on start */
    int autosave_seconds;      /* 0 disables; otherwise writes <scene>.autosave.cwscene while dirty */
} ed_settings;

void ed_settings_defaults(ed_settings *s);
r2d_tunable_set ed_settings_set(ed_settings *s);            /* the table bound to `s` */
const r2d_tunable *ed_settings_defs(size_t *count);
/* Resolves the config path (see above); `out` gets "" when no location is known. */
void ed_settings_path(char *out, size_t capacity);
/* Loads from `path` (NULL: the default location); a missing file leaves the defaults. */
bool ed_settings_load(ed_settings *s, const char *path, char *error, size_t error_capacity);
bool ed_settings_save(const ed_settings *s, const char *path);
/* Snaps a coordinate to the grid when snapping is on. */
float ed_settings_snap(const ed_settings *s, float v);

#endif
