#ifndef _WIN32
#define _DEFAULT_SOURCE 1
#endif
#define _CRT_SECURE_NO_WARNINGS 1
#include "editor_settings.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0755)
#endif

#define F(member) offsetof(ed_settings, member)
static const r2d_tunable defs[] = {
    {"view", "text_scale", R2D_TUNE_FLOAT, F(text_scale), 1.0f, 0.75f, 2.0f, 0.25f, NULL, 0, "UI glyph size multiplier (8 px glyphs at 1)"},
    {"view", "grid", R2D_TUNE_BOOL, F(grid), 1, 0, 1, 1, NULL, 0, "show the grid when a project opens"},
    {"view", "grid_spacing", R2D_TUNE_FLOAT, F(grid_spacing), 50, 1, 1000, 5, NULL, 0, "world units between grid lines; snapping uses it"},
    {"view", "preview_rendering", R2D_TUNE_BOOL, F(preview_rendering), 0, 0, 1, 1, NULL, 0, "start with the game's rendering stack (lights, post-processing) on"},
    {"view", "zoom_step", R2D_TUNE_FLOAT, F(zoom_step), 1.15f, 1.01f, 2.0f, 0.05f, NULL, 0, "wheel zoom factor per notch"},
    {"editing", "snap", R2D_TUNE_BOOL, F(snap), 0, 0, 1, 1, NULL, 0, "snap placement, nudges and drags to the grid"},
    {"editing", "nudge", R2D_TUNE_FLOAT, F(nudge), 1, 0.1f, 100, 1, NULL, 0, "arrow-key step in world units (shift multiplies by 10)"},
    {"editing", "confirm_discard", R2D_TUNE_BOOL, F(confirm_discard), 1, 0, 1, 1, NULL, 0, "ask twice before discarding unsaved scene edits"},
    {"editing", "open_last_scene", R2D_TUNE_BOOL, F(open_last_scene), 1, 0, 1, 1, NULL, 0, "open the project's main scene when the project opens"},
    {"editing", "autosave_seconds", R2D_TUNE_INT, F(autosave_seconds), 0, 0, 3600, 30, NULL, 0, "write <scene>.autosave.cwscene this often while the scene is dirty; 0 disables"},
};
#undef F
const r2d_tunable *ed_settings_defs(size_t *count) {
    if (count) *count = sizeof(defs) / sizeof(defs[0]);
    return defs;
}
r2d_tunable_set ed_settings_set(ed_settings *s) {
    r2d_tunable_set set = {defs, sizeof(defs) / sizeof(defs[0]), s};
    return set;
}
void ed_settings_defaults(ed_settings *s) {
    memset(s, 0, sizeof(*s));
    r2d_tunable_set set = ed_settings_set(s);
    r2d_tunables_reset(&set);
}
void ed_settings_path(char *out, size_t capacity) {
    const char *env = getenv("CW_EDITOR_CONFIG");
    if (env && *env) { snprintf(out, capacity, "%s", env); return; }
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (!base || !*base) { out[0] = 0; return; }
    snprintf(out, capacity, "%s\\clockwork\\editor.cfg", base);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, capacity, "%s/clockwork/editor.cfg", xdg);
    else if (home && *home) snprintf(out, capacity, "%s/.config/clockwork/editor.cfg", home);
    else out[0] = 0;
#endif
}
bool ed_settings_load(ed_settings *s, const char *path, char *error, size_t error_capacity) {
    char buffer[512];
    if (!path) { ed_settings_path(buffer, sizeof(buffer)); path = buffer; }
    if (error && error_capacity) error[0] = 0;
    if (!*path) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;   /* nothing saved yet: defaults stay */
    r2d_tunable_set set = ed_settings_set(s);
    return r2d_tunables_load(&set, path, error, error_capacity);
}
bool ed_settings_save(const ed_settings *s, const char *path) {
    char buffer[512];
    if (!path) { ed_settings_path(buffer, sizeof(buffer)); path = buffer; }
    if (!*path) return false;
    /* create the directory chain */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *p = dir + 1; *p; ++p) {
        if (*p != '/' && *p != '\\') continue;
        char c = *p;
        *p = 0;
        make_dir(dir);
        *p = c;
    }
    r2d_tunable_set set = ed_settings_set((ed_settings *)s);
    return r2d_tunables_save(&set, path, "Clockwork editor preferences (edited under F10 > editor, or here)");
}
float ed_settings_snap(const ed_settings *s, float v) {
    if (!s->snap || s->grid_spacing <= 0) return v;
    return roundf(v / s->grid_spacing) * s->grid_spacing;
}
