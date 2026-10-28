/* launcher_core: the launcher's model, independent of any window.
 *
 * The launcher is the front door of the engine (think Unity Hub or Godot's
 * project manager): it keeps a registry of known projects, shows what state
 * each is in (module built, scenes, last edit), creates new projects from
 * a starter template, and opens them in the editor or runs them in the
 * player. Everything the window does is a function here so it can be
 * tested headlessly.
 *
 * The registry is a text file (default <engine_root>/projects.cwlist, or
 * CW_PROJECTS in the environment): a header line and one absolute .cwproj
 * path per line, most recently opened first. Projects found under
 * <engine_root>/games are registered automatically.
 */
#ifndef LAUNCHER_CORE_H
#define LAUNCHER_CORE_H

#include "cw_game.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LC_MAX_PROJECTS 64
#define LC_MAX_SCENES 32
#define LC_STATUS_CAPACITY 240

typedef struct {
    char path[CW_GAME_PATH_CAPACITY];        /* the .cwproj */
    cw_project project;                      /* valid when `loadable` */
    bool loadable;                           /* the project file parsed */
    bool module_built;                       /* its module library exists */
    char error[160];                         /* why it is not loadable */
    char scenes[LC_MAX_SCENES][128];         /* scene file names */
    size_t scene_count;
    time_t modified;                         /* newest of the project file and its scenes (0 when unknown) */
} lc_entry;

typedef struct {
    char engine_root[CW_GAME_PATH_CAPACITY];
    char registry[CW_GAME_PATH_CAPACITY];    /* the projects.cwlist file */
    char editor[CW_GAME_PATH_CAPACITY];      /* executables the launcher starts (CW_EDITOR / CW_PLAYER override) */
    char player[CW_GAME_PATH_CAPACITY];
    lc_entry entries[LC_MAX_PROJECTS];
    size_t count;
    char status[LC_STATUS_CAPACITY];
    bool status_error;
    char log[8192];                          /* output of the last build */
} lc_launcher;

void lc_init(lc_launcher *l, const char *engine_root, const char *tool_dir);
/* Reads the registry (missing is fine), scans <engine_root>/games, refreshes every entry. */
void lc_load(lc_launcher *l);
bool lc_save(const lc_launcher *l);
void lc_refresh(lc_launcher *l);               /* re-reads every project from disk */
void lc_refresh_entry(lc_entry *e);

/* --- registry --- */
int lc_add(lc_launcher *l, const char *cwproj_path);       /* registers (refreshes when already known); returns its index or -1 */
bool lc_remove(lc_launcher *l, size_t index);              /* forgets; files stay on disk */
void lc_touch(lc_launcher *l, size_t index);               /* moves to the top (most recent) */
int lc_find(const lc_launcher *l, const char *cwproj_path);

/* --- creating projects ---
 * Creates <parent>/<name>/ with <name>.cwproj, <name>_game.c (a complete
 * starter game: a few components with reflection, systems, prefabs,
 * drawing) and scenes/main.cwscene, then registers it. Names are
 * [A-Za-z][A-Za-z0-9_]* since they become identifiers and file names. */
bool lc_name_valid(const char *name);
bool lc_create(lc_launcher *l, const char *parent_dir, const char *name, size_t *index);
const char *lc_template_source(void);          /* the starter module, with @NAME@ placeholders */

/* --- actions --- */
/* Command lines the launcher runs; exposed so tests can check them without starting anything. */
void lc_editor_command(const lc_launcher *l, size_t index, const char *scene, char *out, size_t capacity);
void lc_player_command(const lc_launcher *l, size_t index, const char *scene, char *out, size_t capacity);
bool lc_open_in_editor(lc_launcher *l, size_t index, const char *scene);   /* scene may be NULL */
bool lc_run(lc_launcher *l, size_t index, const char *scene);
bool lc_build(lc_launcher *l, size_t index);                 /* the module, through tools/build-game.sh */

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void lc_status(lc_launcher *l, bool error, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif
