#ifndef _WIN32
#define _DEFAULT_SOURCE 1
#endif
#define _CRT_SECURE_NO_WARNINGS 1
#include "editor_core.h"
#include <math.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define popen _popen
#define pclose _pclose
#else
#include <dirent.h>
#include <sys/stat.h>
#endif
#include <sys/stat.h>

static void copy_str(char *dst, size_t capacity, const char *src) {
    if (!capacity) return;
    size_t n = strlen(src ? src : "");
    if (n >= capacity) n = capacity - 1;
    memcpy(dst, src ? src : "", n);
    dst[n] = 0;
}
static void append_str(char *dst, size_t capacity, const char *src) {
    size_t n = strlen(dst);
    if (n < capacity) copy_str(dst + n, capacity - n, src);
}
static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
static void *grow(void *ptr, size_t *capacity, size_t needed, size_t elem) {
    if (needed <= *capacity) return ptr;
    size_t cap = *capacity ? *capacity * 2 : 32;
    while (cap < needed) cap *= 2;
    ptr = realloc(ptr, cap * elem);
    if (!ptr) { fputs("editor: out of memory\n", stderr); abort(); }
    *capacity = cap;
    return ptr;
}

void ed_status(ed_document *d, bool error, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(d->status, sizeof(d->status), fmt, args);
    va_end(args);
    d->status_error = error;
    printf("%s%s\n", error ? "ERROR: " : "", d->status);
    fflush(stdout);
}

/* --- names --- */
static ed_name *find_name(const ed_document *d, cw_entity_id id) {
    for (size_t i = 0; i < d->name_count; ++i)
        if (d->names[i].id == id) return &d->names[i];
    return NULL;
}
const char *ed_name_of(const ed_document *d, cw_entity_id id) {
    const ed_name *n = find_name(d, id);
    return n ? n->name : "";
}
static void set_name(ed_document *d, cw_entity_id id, const char *name) {
    ed_name *n = find_name(d, id);
    if (!name || !*name) {
        if (n) { *n = d->names[--d->name_count]; }
        return;
    }
    if (!n) {
        d->names = grow(d->names, &d->name_capacity, d->name_count + 1, sizeof(*d->names));
        n = &d->names[d->name_count++];
        n->id = id;
    }
    copy_str(n->name, sizeof(n->name), name);
}
static bool name_of_cb(void *user, cw_entity_id id, char *out, size_t capacity) {
    const char *name = ed_name_of(user, id);
    if (!*name) return false;
    copy_str(out, capacity, name);
    return true;
}
static void on_name_cb(void *user, cw_entity_id id, const char *name) { set_name(user, id, name); }
void ed_label(const ed_document *d, cw_entity_id id, char *out, size_t capacity) {
    const char *name = ed_name_of(d, id);
    if (*name) { copy_str(out, capacity, name); return; }
    const cw_world *w = &d->gw.world;
    if (d->world_ready && d->gw.module->describe) {
        out[0] = 0;
        d->gw.module->describe((cw_world *)w, id, out, capacity);
        if (out[0]) return;
    }
    for (unsigned c = 0; c < CW_MAX_COMPONENTS && d->world_ready; ++c) {
        const cw_type_info *t = cw_comp_type(w, (cw_component_id)c);
        if (!t || (t->flags & (CW_TYPE_TRANSIENT | CW_TYPE_INTERNAL)) || !cw_comp_has(w, id, (cw_component_id)c)) continue;
        bool positional = false;
        for (size_t f = 0; f < t->field_count; ++f)
            if (t->fields[f].semantic == CW_SEM_POSITION_X) positional = true;
        if (positional) continue;   /* "Position" says nothing about what it is */
        copy_str(out, capacity, t->name);
        return;
    }
    copy_str(out, capacity, "entity");
}

/* --- selection --- */
static size_t selection_find(const ed_document *d, cw_entity_id id, bool *found) {
    size_t lo = 0, hi = d->selection_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (d->selection[mid] < id) lo = mid + 1; else hi = mid;
    }
    *found = lo < d->selection_count && d->selection[lo] == id;
    return lo;
}
bool ed_is_selected(const ed_document *d, cw_entity_id id) {
    bool found;
    selection_find(d, id, &found);
    return found;
}
bool ed_select_add(ed_document *d, cw_entity_id id) {
    if (!d->world_ready || !cw_world_is_valid(&d->gw.world, id)) return false;
    bool found;
    size_t at = selection_find(d, id, &found);
    if (found) return false;
    d->selection = grow(d->selection, &d->selection_capacity, d->selection_count + 1, sizeof(*d->selection));
    memmove(d->selection + at + 1, d->selection + at, (d->selection_count - at) * sizeof(*d->selection));
    d->selection[at] = id;
    ++d->selection_count;
    return true;
}
bool ed_select_remove(ed_document *d, cw_entity_id id) {
    bool found;
    size_t at = selection_find(d, id, &found);
    if (!found) return false;
    memmove(d->selection + at, d->selection + at + 1, (d->selection_count - at - 1) * sizeof(*d->selection));
    --d->selection_count;
    return true;
}
void ed_select_toggle(ed_document *d, cw_entity_id id) {
    if (!ed_select_remove(d, id)) ed_select_add(d, id);
}
void ed_select_clear(ed_document *d) { d->selection_count = 0; }
void ed_select_set(ed_document *d, cw_entity_id id) {
    ed_select_clear(d);
    ed_select_add(d, id);
}
void ed_select_all(ed_document *d) {
    ed_select_clear(d);
    if (!d->world_ready) return;
    const cw_world *w = &d->gw.world;
    for (size_t i = 0; i < w->capacity; ++i)
        if (cw_query_visible(w->entities[i])) ed_select_add(d, (cw_entity_id)i);
}
cw_entity_id ed_primary(const ed_document *d) { return d->selection_count ? d->selection[0] : CW_INVALID_ENTITY; }
static void selection_prune(ed_document *d) {
    size_t kept = 0;
    for (size_t i = 0; i < d->selection_count; ++i) {
        cw_entity_id id = d->selection[i];
        if (cw_world_is_valid(&d->gw.world, id) && !(d->gw.world.entities[id].flags & CW_TO_DESTROY)) d->selection[kept++] = id;
    }
    d->selection_count = kept;
}

/* --- images (scene text in memory) --- */
static void image_free(ed_image *img) {
    free(img->text);
    img->text = NULL;
    img->size = 0;
}
static bool capture(ed_document *d, ed_image *img, const char *label) {
    cw_writer out;
    cw_writer_init(&out);
    cw_scene_save_config cfg = {d->gw.module->name, d->gw.module->scene_version, name_of_cb, d};
    cw_scene_result r;
    if (!cw_scene_write(&d->gw.world, &cfg, &out, &r)) {
        cw_writer_destroy(&out);
        ed_status(d, true, "cannot capture the scene: %s", r.message);
        return false;
    }
    image_free(img);
    img->text = (char *)cw_writer_release(&out, &img->size);
    copy_str(img->label, sizeof(img->label), label ? label : "");
    return true;
}
static bool restore(ed_document *d, const ed_image *img, bool lenient) {
    d->name_count = 0;
    cw_scene_load_config cfg = {d->gw.module->name, lenient, false, 0, 0, on_name_cb, d};
    cw_scene_result r;
    bool ok = cw_game_world_read_scene(&d->gw, img->text, img->size, &cfg, &r);
    if (!ok) ed_status(d, true, "cannot restore the scene (line %d): %s", r.line, r.message);
    else if (r.warnings) ed_status(d, false, "restored with %zu warning%s: unknown components or fields were dropped", r.warnings, r.warnings == 1 ? "" : "s");
    cw_game_world_settle(&d->gw);
    selection_prune(d);
    return ok;
}

/* --- history --- */
static void history_clear(ed_document *d) {
    for (size_t i = 0; i < d->history_count; ++i) image_free(&d->history[i]);
    d->history_count = d->current = 0;
}
void ed_commit(ed_document *d, const char *label) {
    if (!d->world_ready) return;
    if (d->playing || d->no_history) { cw_game_world_settle(&d->gw); return; }   /* play-mode and recorded edits are never kept */
    /* drop the redo branch */
    for (size_t i = d->current + 1; i < d->history_count; ++i) image_free(&d->history[i]);
    d->history_count = d->history_count ? d->current + 1 : 0;
    if (d->history_count == ED_UNDO_DEPTH) {
        image_free(&d->history[0]);
        memmove(d->history, d->history + 1, (ED_UNDO_DEPTH - 1) * sizeof(*d->history));
        --d->history_count;
    }
    ed_image img = {NULL, 0, ""};
    if (!capture(d, &img, label)) return;
    d->history[d->history_count] = img;
    d->current = d->history_count++;
    d->dirty = true;
}
bool ed_can_undo(const ed_document *d) { return d->world_ready && !d->playing && d->history_count > 1 && d->current > 0; }
bool ed_can_redo(const ed_document *d) { return d->world_ready && !d->playing && d->current + 1 < d->history_count; }
const char *ed_undo_label(const ed_document *d) { return ed_can_undo(d) ? d->history[d->current].label : ""; }
bool ed_undo(ed_document *d) {
    if (!ed_can_undo(d)) { ed_status(d, false, "nothing to undo"); return false; }
    const char *what = d->history[d->current].label;
    --d->current;
    bool ok = restore(d, &d->history[d->current], false);
    d->dirty = true;
    if (ok) ed_status(d, false, "undid %s", what);
    return ok;
}
bool ed_redo(ed_document *d) {
    if (!ed_can_redo(d)) { ed_status(d, false, "nothing to redo"); return false; }
    ++d->current;
    bool ok = restore(d, &d->history[d->current], false);
    d->dirty = true;
    if (ok) ed_status(d, false, "redid %s", d->history[d->current].label);
    return ok;
}

/* --- lifecycle --- */
void ed_init(ed_document *d, const char *engine_root, const char *player_path) {
    memset(d, 0, sizeof(*d));
    copy_str(d->engine_root, sizeof(d->engine_root), engine_root ? engine_root : "");
    const char *env = getenv("CW_PLAYER");
    copy_str(d->player_path, sizeof(d->player_path), env && *env ? env : (player_path ? player_path : ""));
    d->last_build_status = -1;
    d->texture_lock = true;
    ed_status(d, false, "open a project (.cwproj) to start");
}
static void world_down(ed_document *d) {
    if (d->world_ready) cw_game_world_destroy(&d->gw);
    d->world_ready = false;
    d->playing = d->paused = false;
    image_free(&d->play_backup);
    d->name_count = 0;
    d->selection_count = 0;
    history_clear(d);
}
void ed_shutdown(ed_document *d) {
    world_down(d);
    cw_game_close(&d->game);
    if (d->project_open) cw_assets_destroy(&d->assets);
    free(d->names);
    free(d->selection);
    memset(d, 0, sizeof(*d));
}

/* --- project and module --- */
bool ed_module_built(const ed_document *d) { return d->project_open && file_exists(d->project.module); }
bool ed_load_module(ed_document *d) {
    if (!d->project_open) { ed_status(d, true, "no project is open"); return false; }
    if (!file_exists(d->project.module)) {
        ed_status(d, true, "module not built yet: %s (build it with F7)", d->project.module);
        return false;
    }
    ed_image keep = {NULL, 0, ""};
    bool had_scene = d->world_ready && capture(d, &keep, "reload");
    if (d->playing) ed_stop(d);
    world_down(d);
    cw_game_close(&d->game);
    char error[256];
    if (!cw_game_open(&d->game, d->project.module, error, sizeof(error))) {
        image_free(&keep);
        ed_status(d, true, "%s", error);
        return false;
    }
    if (!cw_game_world_init(&d->gw, d->game.module, error, sizeof(error))) {
        cw_game_close(&d->game);
        image_free(&keep);
        ed_status(d, true, "%s", error);
        return false;
    }
    d->world_ready = true;
    cw_game_world_set_assets(&d->gw, &d->assets);
    cw_game_world_set_ui_canvas(&d->gw, &d->project.ui);
    cw_game_world_set_baking(&d->gw, false);   /* editing authors config; runtime components appear in play mode */
    if (had_scene) {
        restore(d, &keep, true);
        image_free(&keep);
        ed_commit(d, "reload");
        d->dirty = true;
        ed_status(d, false, "module %s reloaded: %zu entities kept", d->game.module->name, ed_entity_count(d));
    } else {
        ed_new_scene(d);
        ed_status(d, false, "module %s loaded: %zu prefabs, %d Hz", d->game.module->name, d->game.module->prefab_count,
                  d->gw.clock.hz);
        if (d->project.main_scene[0] && file_exists(d->project.main_scene)) ed_load_scene(d, d->project.main_scene);
    }
    return true;
}
bool ed_open_project(ed_document *d, const char *path) {
    cw_project project;
    char error[256];
    if (!cw_project_load(&project, path, error, sizeof(error))) { ed_status(d, true, "%s", error); return false; }
    if (d->playing) ed_stop(d);
    world_down(d);
    cw_game_close(&d->game);
    if (d->project_open) cw_assets_destroy(&d->assets);
    d->project = project;
    d->project_open = true;
    d->scene_path[0] = 0;
    d->dirty = false;
    cw_assets_init(&d->assets, d->project.assets);
    if (!cw_assets_load(&d->assets, error, sizeof(error))) ed_status(d, true, "asset manifest %s: %s", d->project.assets, error);
    cw_assets_set_current(&d->assets);
    ed_scan_assets(d);
    if (!file_exists(d->project.module)) {
        ed_status(d, true, "project %s opened; the module is not built yet (F7 builds it)", d->project.name);
        return true;
    }
    return ed_load_module(d);   /* opens the main scene when there is one */
}
static void quote(char *cmd, size_t capacity, const char *arg) {
    append_str(cmd, capacity, " \"");
    append_str(cmd, capacity, arg);
    append_str(cmd, capacity, "\"");
}
bool ed_build(ed_document *d, const char *mode) {
    if (!d->project_open) { ed_status(d, true, "no project is open"); return false; }
    if (!d->engine_root[0]) { ed_status(d, true, "engine root unknown: start the editor with --engine <path>"); return false; }
    if (!d->project.source_count) { ed_status(d, true, "the project lists no sources"); return false; }
    char cmd[4096] = "", script[CW_GAME_PATH_CAPACITY];
    copy_str(script, sizeof(script), d->engine_root);
#ifdef _WIN32
    append_str(cmd, sizeof(cmd), "powershell -NoProfile -ExecutionPolicy Bypass -File");
    append_str(script, sizeof(script), "\\tools\\build-game.ps1");
#else
    append_str(cmd, sizeof(cmd), "sh");
    append_str(script, sizeof(script), "/tools/build-game.sh");
#endif
    quote(cmd, sizeof(cmd), script);
    char output[CW_GAME_PATH_CAPACITY];
    if (strcmp(mode, "module") == 0) copy_str(output, sizeof(output), d->project.module);
    else {
        copy_str(output, sizeof(output), d->project.dir);
        append_str(output, sizeof(output), "build/");
        append_str(output, sizeof(output), d->project.name);
#ifdef _WIN32
        append_str(output, sizeof(output), ".exe");
#endif
    }
    quote(cmd, sizeof(cmd), d->engine_root);
    quote(cmd, sizeof(cmd), d->project.path);
    quote(cmd, sizeof(cmd), mode);
    quote(cmd, sizeof(cmd), output);
    for (size_t i = 0; i < d->project.source_count; ++i) quote(cmd, sizeof(cmd), d->project.sources[i]);
    append_str(cmd, sizeof(cmd), " 2>&1");
    ed_status(d, false, "building %s (%s)...", d->project.name, mode);
    d->log[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) { ed_status(d, true, "cannot run the build script"); return false; }
    size_t len = 0;
    char line[512];
    while (fgets(line, sizeof(line), p)) {
        fputs(line, stdout);
        size_t n = strlen(line);
        if (len + n + 1 < sizeof(d->log)) { memcpy(d->log + len, line, n); len += n; d->log[len] = 0; }
    }
    int status = pclose(p);
#ifndef _WIN32
    if (status != -1) status = (status >> 8) & 0xff;
#endif
    d->last_build_status = status;
    if (status != 0) { ed_status(d, true, "build failed (exit %d): see the build log", status); return false; }
    if (strcmp(mode, "module") == 0) {
        ed_status(d, false, "module built");
        return ed_load_module(d);
    }
    ed_status(d, false, "exported %s to %s", d->project.name, output);
    return true;
}
bool ed_run_player(ed_document *d) {
    if (!d->project_open || !d->world_ready) { ed_status(d, true, "nothing to run"); return false; }
    if (d->dirty || !d->scene_path[0]) {
        if (!d->scene_path[0]) { ed_status(d, true, "save the scene first (Ctrl+S)"); return false; }
        if (!ed_save_scene(d, NULL)) return false;
    }
    const char *player = d->player_path;
    if (!*player || !file_exists(player)) { ed_status(d, true, "player not found at '%s' (set CW_PLAYER)", player); return false; }
    char cmd[2048] = "";
#ifdef _WIN32
    append_str(cmd, sizeof(cmd), "start \"\"");
#endif
    quote(cmd, sizeof(cmd), player);
    append_str(cmd, sizeof(cmd), " --project");
    quote(cmd, sizeof(cmd), d->project.path);
    append_str(cmd, sizeof(cmd), " --scene");
    quote(cmd, sizeof(cmd), d->scene_path);
#ifndef _WIN32
    append_str(cmd, sizeof(cmd), " &");
#endif
    int status = system(cmd);
    ed_status(d, status != 0, "launched the player on %s", d->scene_path);
    return status == 0;
}

/* --- assets --- */
bool ed_save_assets(ed_document *d) {
    if (!d->project_open) return false;
    char error[256];
    if (!cw_assets_save(&d->assets, error, sizeof(error))) { ed_status(d, true, "%s", error); return false; }
    return true;
}
const cw_asset *ed_import_asset(ed_document *d, const char *source, bool replace) {
    if (!d->project_open) { ed_status(d, true, "open a project first"); return NULL; }
    char error[256];
    const cw_asset *a = cw_assets_import(&d->assets, source, NULL, replace, error, sizeof(error));
    if (!a) { ed_status(d, true, "cannot import %s: %s", source, error); return NULL; }
    ed_save_assets(d);
    ed_status(d, false, "imported %s as %s (%s, %llu bytes)", source, a->name, cw_asset_type_name(a->type), (unsigned long long)a->size);
    return a;
}
size_t ed_import_directory(ed_document *d, const char *directory, bool replace) {
    if (!d->project_open) { ed_status(d, true, "open a project first"); return 0; }
    size_t n = 0, failed = 0;
#ifdef _WIN32
    char pattern[CW_GAME_PATH_CAPACITY + 4];
    copy_str(pattern, sizeof(pattern), directory);
    append_str(pattern, sizeof(pattern), "\\*");
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) { ed_status(d, true, "cannot read %s", directory); return 0; }
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[CW_GAME_PATH_CAPACITY];
        copy_str(path, sizeof(path), directory); append_str(path, sizeof(path), "\\"); append_str(path, sizeof(path), data.cFileName);
        char error[128];
        if (cw_assets_import(&d->assets, path, NULL, replace, error, sizeof(error))) ++n; else ++failed;
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR *dir = opendir(directory);
    if (!dir) { ed_status(d, true, "cannot read %s", directory); return 0; }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[CW_GAME_PATH_CAPACITY];
        copy_str(path, sizeof(path), directory); append_str(path, sizeof(path), "/"); append_str(path, sizeof(path), e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || (st.st_mode & S_IFDIR)) continue;
        char error[128];
        if (cw_assets_import(&d->assets, path, NULL, replace, error, sizeof(error))) ++n; else ++failed;
    }
    closedir(dir);
#endif
    ed_save_assets(d);
    ed_status(d, failed > 0, "imported %zu file%s from %s%s", n, n == 1 ? "" : "s", directory, failed ? " (some were skipped: already imported)" : "");
    return n;
}
size_t ed_scan_assets(ed_document *d) {
    if (!d->project_open) return 0;
    size_t n = cw_assets_scan(&d->assets, "");
    if (d->assets.dirty) ed_save_assets(d);
    return n;
}
bool ed_remove_asset(ed_document *d, uint32_t id, bool delete_file) {
    const cw_asset *a = cw_assets_find(&d->assets, id);
    if (!a) return false;
    char name[CW_ASSET_NAME_CAPACITY];
    copy_str(name, sizeof(name), a->name);
    if (!cw_assets_remove(&d->assets, id, delete_file)) return false;
    ed_save_assets(d);
    ed_status(d, false, "%s %s", delete_file ? "deleted" : "unregistered", name);
    return true;
}
bool ed_rename_asset(ed_document *d, uint32_t id, const char *name) {
    if (!cw_assets_rename(&d->assets, id, name)) { ed_status(d, true, "cannot rename the asset to '%s'", name ? name : ""); return false; }
    ed_save_assets(d);
    ed_status(d, false, "asset renamed to %s (references by the old name no longer resolve)", name);
    return true;
}
const cw_asset *ed_write_asset(ed_document *d, const char *relative, const void *data, size_t size) {
    if (!d->project_open || !relative || !*relative) { ed_status(d, true, "open a project first"); return NULL; }
    char full[CW_GAME_PATH_CAPACITY], dir[CW_GAME_PATH_CAPACITY];
    cw_assets_resolve(&d->assets, relative, full, sizeof(full));
    copy_str(dir, sizeof(dir), full);
    for (char *p = dir + 1; *p; ++p) {   /* create the directory chain */
        if (*p != '/' && *p != '\\') continue;
        char c = *p;
        *p = 0;
#ifdef _WIN32
        _mkdir(dir);
#else
        mkdir(dir, 0755);
#endif
        *p = c;
    }
    cw_file_error ferr;
    if (!cw_file_write(full, data, size, &ferr)) { ed_status(d, true, "cannot write %s: %s", full, cw_file_status_string(ferr.status)); return NULL; }
    const cw_asset *a = cw_assets_find_path(&d->assets, relative);
    char error[128];
    if (a) cw_assets_refresh(&d->assets);
    else a = cw_assets_add(&d->assets, relative, NULL, error, sizeof(error));
    if (!a) { ed_status(d, true, "%s written but not registered: %s", relative, error); return NULL; }
    ed_save_assets(d);
    return a;
}
size_t ed_asset_references(ed_document *d, uint32_t id) {
    if (!d->world_ready || !id) return 0;
    size_t n = 0;
    cw_world *w = &d->gw.world;
    for (unsigned c = 0; c < CW_MAX_COMPONENTS; ++c) {
        const cw_type_info *t = cw_comp_type(w, (cw_component_id)c);
        if (!t) continue;
        for (size_t f = 0; f < t->field_count; ++f) {
            if (t->fields[f].semantic != CW_SEM_ASSET) continue;
            for (size_t i = 0; i < w->capacity; ++i) {
                if (!cw_query_visible(w->entities[i]) || !cw_comp_has(w, (cw_entity_id)i, (cw_component_id)c)) continue;
                if ((uint32_t)cw_field_get(&t->fields[f], cw_comp_ptr(w, (cw_entity_id)i, (cw_component_id)c)) == id) ++n;
            }
        }
    }
    return n;
}

/* --- world inspection --- */
size_t ed_component_entities(const ed_document *d, cw_component_id ct) {
    if (!d->world_ready) return 0;
    const cw_world *w = &d->gw.world;
    size_t n = 0;
    for (size_t i = 0; i < w->capacity; ++i)
        if (cw_query_visible(w->entities[i]) && cw_comp_has(w, (cw_entity_id)i, ct)) ++n;
    return n;
}
void ed_select_with_component(ed_document *d, cw_component_id ct) {
    ed_select_clear(d);
    if (!d->world_ready) return;
    const cw_world *w = &d->gw.world;
    for (size_t i = 0; i < w->capacity; ++i)
        if (cw_query_visible(w->entities[i]) && cw_comp_has(w, (cw_entity_id)i, ct)) ed_select_add(d, (cw_entity_id)i);
    ed_status(d, false, "selected %zu entit%s with %s", d->selection_count, d->selection_count == 1 ? "y" : "ies", cw_comp_name(w, ct));
}
bool ed_set_system_enabled(ed_document *d, const char *name, bool enabled) {
    if (!d->world_ready || !cw_sim_set_enabled(&d->gw.sim, name, enabled)) return false;
    ed_status(d, false, "system %s %s", name, enabled ? "enabled" : "disabled");
    return true;
}

/* --- autosave --- */
void ed_autosave_path(const ed_document *d, char *out, size_t capacity) {
    out[0] = 0;
    if (!d->scene_path[0]) return;
    copy_str(out, capacity, d->scene_path);
    size_t len = strlen(out);
    if (len > 8 && strcmp(out + len - 8, ".cwscene") == 0) out[len - 8] = 0;
    append_str(out, capacity, ".autosave.cwscene");
}
bool ed_autosave(ed_document *d) {
    char path[CW_GAME_PATH_CAPACITY];
    ed_autosave_path(d, path, sizeof(path));
    if (!d->world_ready || !path[0] || d->playing) return false;
    cw_scene_save_config cfg = {d->gw.module->name, d->gw.module->scene_version, name_of_cb, d};
    cw_scene_result r;
    return cw_game_world_save_scene(&d->gw, path, &cfg, &r);
}

/* --- project settings --- */
void ed_render_path(const ed_document *d, char *out, size_t capacity) {
    copy_str(out, capacity, d->project_open ? d->project.render : "");
}
bool ed_render_settings_exist(const ed_document *d) { return d->project_open && file_exists(d->project.render); }
bool ed_set_window(ed_document *d, int width, int height, const char *title) {
    if (!d->project_open) { ed_status(d, true, "no project is open"); return false; }
    if (width < 64 || height < 64 || width > 16384 || height > 16384) { ed_status(d, true, "window size must be between 64 and 16384"); return false; }
    d->project.window_width = width;
    d->project.window_height = height;
    copy_str(d->project.title, sizeof(d->project.title), title && *title ? title : d->project.name);
    char error[256];
    if (!cw_project_save(&d->project, error, sizeof(error))) { ed_status(d, true, "%s", error); return false; }
    ed_status(d, false, "project saved: window %dx%d \"%s\"", width, height, d->project.title);
    return true;
}

/* --- scenes --- */
void ed_new_scene(ed_document *d) {
    if (!d->world_ready) { ed_status(d, true, "load a module first"); return; }
    if (d->playing) ed_stop(d);
    cw_game_world_clear(&d->gw);
    d->name_count = 0;
    d->selection_count = 0;
    history_clear(d);
    d->scene_path[0] = 0;
    ed_commit(d, "new scene");
    d->dirty = false;
    ed_status(d, false, "new scene");
}
bool ed_load_scene(ed_document *d, const char *path) {
    if (!d->world_ready) { ed_status(d, true, "load a module first"); return false; }
    if (d->playing) ed_stop(d);
    cw_buffer text;
    cw_file_error ferr;
    if (!cw_file_read(path, CW_SCENE_DEFAULT_MAX_SIZE, &text, &ferr)) {
        ed_status(d, true, "cannot read %s: %s", path, cw_file_status_string(ferr.status));
        return false;
    }
    ed_image img = {(char *)text.data, text.size, "open"};
    d->selection_count = 0;
    bool ok = restore(d, &img, false);
    cw_buffer_free(&text);
    history_clear(d);
    if (!ok) {
        copy_str(d->scene_path, sizeof(d->scene_path), "");
        ed_commit(d, "empty");
        d->dirty = false;
        return false;
    }
    copy_str(d->scene_path, sizeof(d->scene_path), path);
    ed_commit(d, "open");
    d->dirty = false;
    ed_status(d, false, "opened %s: %zu entities", path, ed_entity_count(d));
    return true;
}
bool ed_save_scene(ed_document *d, const char *path) {
    if (!d->world_ready) { ed_status(d, true, "nothing to save"); return false; }
    if (d->playing) { ed_status(d, true, "stop play mode before saving"); return false; }
    if (!path || !*path) path = d->scene_path;
    if (!*path) { ed_status(d, true, "the scene has no file name yet: use save as"); return false; }
    cw_scene_save_config cfg = {d->gw.module->name, d->gw.module->scene_version, name_of_cb, d};
    cw_scene_result r;
    if (!cw_game_world_save_scene(&d->gw, path, &cfg, &r)) { ed_status(d, true, "%s", r.message); return false; }
    copy_str(d->scene_path, sizeof(d->scene_path), path);
    d->dirty = false;
    ed_status(d, false, "saved %zu entities to %s", r.entities, path);
    return true;
}
static int compare_names(const void *a, const void *b) { return strcmp(a, b); }
size_t ed_list_scenes(const ed_document *d, char (*names)[128], size_t capacity) {
    size_t n = 0;
    if (!d->project_open || !capacity) return 0;
#ifdef _WIN32
    char pattern[CW_GAME_PATH_CAPACITY + 16];
    copy_str(pattern, sizeof(pattern), d->project.scenes_dir);
    append_str(pattern, sizeof(pattern), "\\*.cwscene");
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (n < capacity) copy_str(names[n++], 128, data.cFileName);
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR *dir = opendir(d->project.scenes_dir);
    if (!dir) return 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 9 || strcmp(e->d_name + len - 8, ".cwscene") != 0) continue;
        if (n < capacity) copy_str(names[n++], 128, e->d_name);
    }
    closedir(dir);
#endif
    qsort(names, n, 128, compare_names);
    return n;
}
void ed_scene_file(const ed_document *d, const char *name, char *out, size_t capacity) {
    copy_str(out, capacity, d->project.scenes_dir);
    append_str(out, capacity, "/");
    append_str(out, capacity, name);
    size_t len = strlen(out);
    if (len < 8 || strcmp(out + len - 8, ".cwscene") != 0) append_str(out, capacity, ".cwscene");
}

/* --- entities --- */
size_t ed_entity_count(const ed_document *d) { return d->world_ready ? cw_world_entity_count(&d->gw.world) : 0; }
size_t ed_entities(const ed_document *d, cw_entity_id *out, size_t capacity) {
    size_t n = 0;
    if (!d->world_ready) return 0;
    const cw_world *w = &d->gw.world;
    for (size_t i = 0; i < w->capacity && n < capacity; ++i)
        if (cw_query_visible(w->entities[i])) out[n++] = (cw_entity_id)i;
    return n;
}
cw_entity_id ed_place(ed_document *d, size_t prefab, float x, float y) {
    if (!d->world_ready) return CW_INVALID_ENTITY;
    const cw_game_module *m = d->gw.module;
    if (prefab >= m->prefab_count) { ed_status(d, true, "no such prefab"); return CW_INVALID_ENTITY; }
    cw_entity_id id = cw_game_world_spawn(&d->gw, prefab, x, y);
    if (id == CW_INVALID_ENTITY) { ed_status(d, true, "the %s prefab refused to spawn there", m->prefabs[prefab].name); return id; }
    cw_game_world_settle(&d->gw);
    ed_select_set(d, id);
    char label[64];
    snprintf(label, sizeof(label), "place %s", m->prefabs[prefab].name);
    ed_commit(d, label);
    ed_status(d, false, "placed %s %s #%" PRId64 " at %.0f, %.0f", m->prefabs[prefab].category ? m->prefabs[prefab].category : "",
              m->prefabs[prefab].name, id, (double)x, (double)y);
    return id;
}
static cw_component_id position_component(const cw_world *w) {
    for (unsigned c = 0; c < CW_MAX_COMPONENTS; ++c) {
        const cw_type_info *t = cw_comp_type(w, (cw_component_id)c);
        for (size_t f = 0; t && f < t->field_count; ++f)
            if (t->fields[f].semantic == CW_SEM_POSITION_X) return (cw_component_id)c;
    }
    return CW_INVALID_COMPONENT;
}
cw_entity_id ed_create_entity3(ed_document *d, float x, float y, float z) {
    if (!d->world_ready) return CW_INVALID_ENTITY;
    cw_world *w = &d->gw.world;
    cw_entity_id id = cw_world_create_entity(w);
    cw_component_id pc = position_component(w);
    if (pc != CW_INVALID_COMPONENT) {
        const cw_type_info *t = cw_comp_type(w, pc);
        unsigned char value[1024];
        if (t->size <= sizeof(value)) {
            cw_type_default(t, value);
            cw_comp_set(w, id, pc, value, t->size);
            cw_reflect_set_position3(w, id, x, y, z);
        }
    }
    cw_game_world_settle(&d->gw);
    ed_select_set(d, id);
    ed_commit(d, "create entity");
    ed_status(d, false, "created entity #%" PRId64 "%s: add components in the inspector", id,
              pc == CW_INVALID_COMPONENT ? "" : " with a position");
    return id;
}
cw_entity_id ed_create_entity(ed_document *d, float x, float y) { return ed_create_entity3(d, x, y, 0); }
/* --- the interface --- */
static const char *const ui_kind_names[ED_UI_KINDS] = {"panel", "image", "text", "button", "bar"};
const char *ed_ui_kind_name(ed_ui_kind kind) { return kind >= 0 && kind < ED_UI_KINDS ? ui_kind_names[kind] : "?"; }
bool ed_is_ui(const ed_document *d, cw_entity_id id) {
    return d->world_ready && cw_ui_ready(&d->gw.ui) && cw_world_is_valid(&d->gw.world, id) && cw_comp_has(&d->gw.world, id, d->gw.ui.rect);
}
size_t ed_ui_layout(ed_document *d, cw_ui_screen screen) {
    if (!d->world_ready) return 0;
    cw_game_world_set_ui_screen(&d->gw, screen);
    return cw_game_world_ui_layout(&d->gw);
}
cw_entity_id ed_ui_hit(ed_document *d, float x, float y) {
    if (!d->world_ready || !cw_ui_ready(&d->gw.ui)) return CW_INVALID_ENTITY;
    return cw_ui_hit(&d->gw.world, &d->gw.ui, x, y, false);
}
static void ui_add(cw_world *w, cw_entity_id id, cw_component_id ct) {
    cw_comp_add(w, id, ct);
    cw_type_default(cw_comp_type(w, ct), cw_comp_ptr(w, id, ct));
}
cw_entity_id ed_create_ui(ed_document *d, ed_ui_kind kind, cw_entity_id parent) {
    if (!d->world_ready || !cw_ui_ready(&d->gw.ui)) { ed_status(d, true, "load a module first"); return CW_INVALID_ENTITY; }
    if (kind < 0 || kind >= ED_UI_KINDS) return CW_INVALID_ENTITY;
    cw_world *w = &d->gw.world;
    const cw_ui_components *c = &d->gw.ui;
    if (!ed_is_ui(d, parent)) parent = CW_INVALID_ENTITY;
    cw_game_world_ui_layout(&d->gw);   /* so the parent's size is known */
    cw_entity_id id = cw_world_create_entity(w);
    ui_add(w, id, c->rect);
    cw_ui_rect *r = cw_comp_ptr(w, id, c->rect);
    r->parent = parent;
    float pw = d->gw.ui_canvas.width, ph = d->gw.ui_canvas.height;
    if (parent != CW_INVALID_ENTITY && cw_comp_has(w, parent, c->layout)) { const cw_ui_layout *pl = cw_comp_ptr(w, parent, c->layout); pw = pl->rw; ph = pl->rh; }
    float hw = 100, hh = 25;   /* half sizes per kind */
    switch (kind) {
        case ED_UI_PANEL: hw = pw * 0.25f; hh = ph * 0.25f; if (hw < 40) hw = 40; if (hh < 30) hh = 30; break;
        case ED_UI_IMAGE: hw = hh = 64; break;
        case ED_UI_TEXT: hw = 120; hh = 16; break;
        case ED_UI_BUTTON: hw = 90; hh = 22; break;
        case ED_UI_BAR: hw = 100; hh = 10; break;
        default: break;
    }
    r->anchor_min_x = r->anchor_max_x = r->anchor_min_y = r->anchor_max_y = 0.5f;
    r->left = -hw; r->right = hw; r->top = -hh; r->bottom = hh;
    switch (kind) {
        case ED_UI_PANEL: {
            ui_add(w, id, c->image);
            cw_ui_image *im = cw_comp_ptr(w, id, c->image);
            im->r = 24; im->g = 28; im->b = 38; im->a = 200;
            break;
        }
        case ED_UI_IMAGE: ui_add(w, id, c->image); break;
        case ED_UI_TEXT: ui_add(w, id, c->text); break;
        case ED_UI_BUTTON: {
            ui_add(w, id, c->image);
            cw_ui_image *im = cw_comp_ptr(w, id, c->image);
            im->r = 70; im->g = 110; im->b = 170;
            ui_add(w, id, c->button);
            /* the label: a stretched child */
            cw_entity_id label = cw_world_create_entity(w);
            ui_add(w, label, c->rect);
            cw_ui_rect *lr = cw_comp_ptr(w, label, c->rect);
            lr->parent = id;
            lr->anchor_min_x = lr->anchor_min_y = 0; lr->anchor_max_x = lr->anchor_max_y = 1;
            lr->left = lr->top = lr->right = lr->bottom = 0;
            ui_add(w, label, c->text);
            cw_ui_text *t = cw_comp_ptr(w, label, c->text);
            snprintf(t->text, sizeof(t->text), "button");
            t->size = 1.5f;
            set_name(d, label, "label");
            break;
        }
        case ED_UI_BAR: {
            ui_add(w, id, c->image);
            cw_ui_image *im = cw_comp_ptr(w, id, c->image);
            im->r = 90; im->g = 200; im->b = 90;
            im->fill_mode = CW_UI_FILL_HORIZONTAL;
            im->fill = 0.75f;
            break;
        }
        default: break;
    }
    set_name(d, id, ui_kind_names[kind]);
    cw_game_world_settle(&d->gw);
    cw_game_world_ui_layout(&d->gw);
    ed_select_set(d, id);
    char label[32];
    snprintf(label, sizeof(label), "create %s", ui_kind_names[kind]);
    ed_commit(d, label);
    ed_status(d, false, "created a %s (#%" PRId64 ")%s", ui_kind_names[kind], id, parent == CW_INVALID_ENTITY ? " on the canvas" : " inside the selected element");
    return id;
}
static bool ui_has_selected_ancestor(ed_document *d, cw_entity_id id) {
    cw_world *w = &d->gw.world;
    const cw_ui_components *c = &d->gw.ui;
    for (int depth = 0; depth < CW_UI_MAX_DEPTH; ++depth) {
        const cw_ui_rect *r = cw_comp_ptr(w, id, c->rect);
        if (!r || r->parent == id || !ed_is_ui(d, r->parent)) return false;
        if (ed_is_selected(d, r->parent)) return true;
        id = r->parent;
    }
    return false;
}
size_t ed_ui_move_selection(ed_document *d, float dx, float dy) {
    if (!d->world_ready || !cw_ui_ready(&d->gw.ui)) return 0;
    size_t n = 0;
    for (size_t i = 0; i < d->selection_count; ++i) {
        cw_entity_id id = d->selection[i];
        if (!ed_is_ui(d, id) || ui_has_selected_ancestor(d, id)) continue;   /* children follow their parent */
        cw_ui_rect *r = cw_comp_ptr(&d->gw.world, id, d->gw.ui.rect);
        r->left += dx; r->right += dx; r->top += dy; r->bottom += dy;
        ++n;
    }
    if (n) cw_game_world_ui_layout(&d->gw);
    return n;
}
bool ed_ui_resize(ed_document *d, cw_entity_id id, bool left, bool top, bool right, bool bottom, float dx, float dy) {
    if (!ed_is_ui(d, id)) return false;
    cw_ui_rect *r = cw_comp_ptr(&d->gw.world, id, d->gw.ui.rect);
    if (left) r->left += dx;
    if (right) r->right += dx;
    if (top) r->top += dy;
    if (bottom) r->bottom += dy;
    cw_game_world_ui_layout(&d->gw);
    return true;
}
bool ed_ui_set_anchors(ed_document *d, cw_entity_id id, float min_x, float min_y, float max_x, float max_y) {
    if (!ed_is_ui(d, id)) return false;
    cw_game_world_ui_layout(&d->gw);
    float pw, ph;
    if (!cw_ui_parent_size(&d->gw.world, &d->gw.ui, id, &pw, &ph)) return false;
    cw_ui_rect *r = cw_comp_ptr(&d->gw.world, id, d->gw.ui.rect);
    cw_ui_rect_set_anchors(r, pw, ph, min_x, min_y, max_x, max_y);
    cw_game_world_ui_layout(&d->gw);
    ed_commit(d, "anchors");
    return true;
}
bool ed_ui_set_anchor_preset(ed_document *d, cw_entity_id id, int preset) {
    if (preset < 0 || preset > 11) return false;
    if (preset < 9) {
        float ax = (float)(preset % 3) * 0.5f, ay = (float)(preset / 3) * 0.5f;
        return ed_ui_set_anchors(d, id, ax, ay, ax, ay);
    }
    if (!ed_is_ui(d, id)) return false;
    const cw_ui_rect *r = cw_comp_ptr(&d->gw.world, id, d->gw.ui.rect);
    float cy = (r->anchor_min_y + r->anchor_max_y) * 0.5f, cx = (r->anchor_min_x + r->anchor_max_x) * 0.5f;
    if (preset == 9) return ed_ui_set_anchors(d, id, 0, cy, 1, cy);
    if (preset == 10) return ed_ui_set_anchors(d, id, cx, 0, cx, 1);
    return ed_ui_set_anchors(d, id, 0, 0, 1, 1);
}
bool ed_ui_set_parent(ed_document *d, cw_entity_id id, cw_entity_id parent) {
    if (!ed_is_ui(d, id)) return false;
    if (parent != CW_INVALID_ENTITY && !ed_is_ui(d, parent)) { ed_status(d, true, "the parent must be an interface element"); return false; }
    cw_world *w = &d->gw.world;
    const cw_ui_components *c = &d->gw.ui;
    /* refuse cycles: the new parent must not be the element or one of its descendants */
    for (cw_entity_id p = parent; p != CW_INVALID_ENTITY && ed_is_ui(d, p);) {
        if (p == id) { ed_status(d, true, "an element cannot be inside itself"); return false; }
        const cw_ui_rect *pr = cw_comp_ptr(w, p, c->rect);
        if (pr->parent == p) break;
        p = pr->parent;
    }
    cw_game_world_ui_layout(&d->gw);
    const cw_ui_layout *l = cw_comp_ptr(w, id, c->layout);
    float x0 = l->rx, y0 = l->ry, x1 = l->rx + l->rw, y1 = l->ry + l->rh;   /* screen-relative reference edges */
    float px = 0, py = 0, pw = l->pw, ph = l->ph;
    if (parent != CW_INVALID_ENTITY) { const cw_ui_layout *pl = cw_comp_ptr(w, parent, c->layout); px = pl->rx; py = pl->ry; pw = pl->rw; ph = pl->rh; }
    else { pw = d->gw.ui_screen.w / (l->scale > 0 ? l->scale : 1); ph = d->gw.ui_screen.h / (l->scale > 0 ? l->scale : 1); }
    cw_ui_rect *r = cw_comp_ptr(w, id, c->rect);
    r->parent = parent;
    cw_ui_rect_set_edges(r, pw, ph, x0 - px, y0 - py, x1 - px, y1 - py);
    cw_game_world_ui_layout(&d->gw);
    ed_commit(d, "parent");
    return true;
}
bool ed_set_ui_canvas(ed_document *d, float width, float height, cw_ui_scale_mode mode) {
    if (!d->project_open) { ed_status(d, true, "no project is open"); return false; }
    if (!(width >= 64) || !(height >= 64) || width > 16384 || height > 16384 || mode < 0 || mode >= CW_UI_SCALE_MODES) { ed_status(d, true, "the reference resolution must be between 64 and 16384"); return false; }
    d->project.ui.width = width;
    d->project.ui.height = height;
    d->project.ui.mode = mode;
    if (d->world_ready) cw_game_world_set_ui_canvas(&d->gw, &d->project.ui);
    char error[256];
    if (!cw_project_save(&d->project, error, sizeof(error))) { ed_status(d, true, "%s", error); return false; }
    ed_status(d, false, "project saved: interface designed for %gx%g, %s", (double)width, (double)height, cw_ui_scale_mode_name(mode));
    return true;
}
static int compare_asset_names(const void *a, const void *b) {
    return strcmp((*(const cw_asset *const *)a)->name, (*(const cw_asset *const *)b)->name);
}
size_t ed_list_prefabs(const ed_document *d, const cw_asset **out, size_t capacity) {
    size_t n = 0;
    if (!d->project_open) return 0;
    for (size_t i = 0; i < d->assets.count && n < capacity; ++i)
        if (d->assets.items[i].type == CW_ASSET_PREFAB && !d->assets.items[i].missing) out[n++] = &d->assets.items[i];
    qsort(out, n, sizeof(*out), compare_asset_names);
    return n;
}
const cw_asset *ed_save_prefab(ed_document *d, const char *name) {
    if (!d->world_ready || !d->project_open) { ed_status(d, true, "open a project first"); return NULL; }
    if (!d->selection_count) { ed_status(d, true, "select the entities to save as a prefab"); return NULL; }
    if (!name || !*name || strpbrk(name, "/\\\"\r\n")) { ed_status(d, true, "give the prefab a file name (no slashes or quotes)"); return NULL; }
    char file[CW_ASSET_NAME_CAPACITY + 16], rel[CW_GAME_PATH_CAPACITY], full[CW_GAME_PATH_CAPACITY];
    size_t len = strlen(name);
    bool has_ext = len > 9 && strcmp(name + len - 9, ".cwprefab") == 0;
    snprintf(file, sizeof(file), "%s%s", name, has_ext ? "" : ".cwprefab");
    copy_str(rel, sizeof(rel), "prefabs/");
    append_str(rel, sizeof(rel), file);
    cw_assets_resolve(&d->assets, rel, full, sizeof(full));
    /* the fragment: selected entities in id order */
    cw_writer out;
    cw_writer_init(&out);
    cw_scene_save_config cfg = {d->gw.module->name, d->gw.module->scene_version, name_of_cb, d};
    cw_scene_result r;
    if (!cw_scene_write_entities(&d->gw.world, d->selection, d->selection_count, &cfg, &out, &r)) {
        cw_writer_destroy(&out);
        ed_status(d, true, "cannot write the prefab: %s", r.message);
        return NULL;
    }
    char dir[CW_GAME_PATH_CAPACITY];
    cw_assets_resolve(&d->assets, "prefabs", dir, sizeof(dir));
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    cw_file_error ferr;
    bool ok = cw_file_write(full, out.data, out.size, &ferr);
    cw_writer_destroy(&out);
    if (!ok) { ed_status(d, true, "cannot write %s: %s", full, cw_file_status_string(ferr.status)); return NULL; }
    const cw_asset *a = cw_assets_find_path(&d->assets, rel);
    if (a) cw_assets_refresh(&d->assets);
    else {
        char error[128];
        a = cw_assets_add(&d->assets, rel, NULL, error, sizeof(error));
        if (!a) { ed_status(d, true, "prefab written but not registered: %s", error); return NULL; }
    }
    ed_save_assets(d);
    ed_status(d, false, "prefab %s saved with %zu entit%s", a->name, d->selection_count, d->selection_count == 1 ? "y" : "ies");
    return a;
}
size_t ed_place_prefab(ed_document *d, uint32_t asset_id, float x, float y) {
    if (!d->world_ready) return 0;
    const cw_asset *a = cw_assets_find(&d->assets, asset_id);
    if (!a || a->type != CW_ASSET_PREFAB) { ed_status(d, true, "no such prefab"); return 0; }
    char full[CW_GAME_PATH_CAPACITY];
    cw_assets_resolve(&d->assets, a->path, full, sizeof(full));
    cw_buffer text;
    cw_file_error ferr;
    if (!cw_file_read(full, CW_SCENE_DEFAULT_MAX_SIZE, &text, &ferr)) { ed_status(d, true, "cannot read %s", full); return 0; }
    cw_entity_id ids[256];
    size_t count = 0;
    cw_scene_load_config cfg = {d->gw.module->name, false, false, 0, 0, on_name_cb, d};
    cw_scene_result r;
    bool ok = cw_scene_instantiate(&d->gw.world, (const char *)text.data, text.size, &cfg, true, x, y, ids, 256, &count, &r);
    cw_buffer_free(&text);
    if (!ok) { ed_status(d, true, "prefab %s (line %d): %s", a->name, r.line, r.message); return 0; }
    cw_game_world_settle(&d->gw);
    ed_select_clear(d);
    for (size_t i = 0; i < count && i < 256; ++i) ed_select_add(d, ids[i]);
    char label[96];
    snprintf(label, sizeof(label), "place %.60s", a->name);
    ed_commit(d, label);
    ed_status(d, false, "placed prefab %s: %zu entit%s at %.0f, %.0f", a->name, count, count == 1 ? "y" : "ies", (double)x, (double)y);
    return count;
}
bool ed_delete_prefab(ed_document *d, uint32_t asset_id) {
    const cw_asset *a = cw_assets_find(&d->assets, asset_id);
    if (!a || a->type != CW_ASSET_PREFAB) return false;
    return ed_remove_asset(d, asset_id, true);
}
size_t ed_delete_selection(ed_document *d) {
    if (!d->world_ready || !d->selection_count) { ed_status(d, false, "nothing selected"); return 0; }
    size_t n = 0;
    for (size_t i = 0; i < d->selection_count; ++i) {
        if (!cw_world_is_valid(&d->gw.world, d->selection[i])) continue;
        cw_world_mark_destroy(&d->gw.world, d->selection[i]);
        set_name(d, d->selection[i], NULL);
        ++n;
    }
    cw_game_world_settle(&d->gw);
    d->selection_count = 0;
    ed_commit(d, n == 1 ? "delete entity" : "delete entities");
    ed_status(d, false, "deleted %zu entit%s", n, n == 1 ? "y" : "ies");
    return n;
}
typedef struct { const cw_entity_id *from, *to; size_t count; } id_map;
static cw_entity_id map_duplicate(cw_entity_id ref, void *user) {
    const id_map *m = user;
    for (size_t i = 0; i < m->count; ++i)
        if (m->from[i] == ref) return m->to[i];
    return ref;
}
size_t ed_duplicate_selection(ed_document *d, float dx, float dy) { return ed_duplicate_selection3(d, dx, dy, 0); }
size_t ed_duplicate_selection3(ed_document *d, float dx, float dy, float dz) {
    if (!d->world_ready || !d->selection_count) { ed_status(d, false, "nothing selected"); return 0; }
    cw_world *w = &d->gw.world;
    size_t n = d->selection_count;
    cw_entity_id *from = malloc(n * sizeof(*from)), *to = malloc(n * sizeof(*to));
    if (!from || !to) { free(from); free(to); return 0; }
    memcpy(from, d->selection, n * sizeof(*from));
    size_t made = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!cw_world_is_valid(w, from[i])) { to[i] = CW_INVALID_ENTITY; continue; }
        to[i] = cw_world_create_entity(w);
        cw_reflect_copy_entity(w, from[i], to[i]);
        float x, y, z;
        if (cw_reflect_position3(w, to[i], &x, &y, &z)) cw_reflect_set_position3(w, to[i], x + dx, y + dy, z + dz);
        const char *name = ed_name_of(d, from[i]);
        if (*name) {
            char copy[CW_SCENE_NAME_CAPACITY];
            snprintf(copy, sizeof(copy), "%.*s copy", (int)(sizeof(copy) - 6), name);
            set_name(d, to[i], copy);
        }
        ++made;
    }
    id_map map = {from, to, n};
    for (size_t i = 0; i < n; ++i)
        if (to[i] != CW_INVALID_ENTITY) cw_reflect_remap(w, to[i], map_duplicate, &map);
    cw_game_world_settle(&d->gw);
    ed_select_clear(d);
    for (size_t i = 0; i < n; ++i)
        if (to[i] != CW_INVALID_ENTITY) ed_select_add(d, to[i]);
    free(from);
    free(to);
    ed_ui_move_selection(d, 16, 16);   /* interface copies step aside too (the ones not inside another copy) */
    ed_commit(d, "duplicate");
    ed_status(d, false, "duplicated %zu entit%s", made, made == 1 ? "y" : "ies");
    return made;
}
size_t ed_move_selection(ed_document *d, float dx, float dy) {
    if (!d->world_ready) return 0;
    size_t n = 0;
    for (size_t i = 0; i < d->selection_count; ++i) {
        float x, y;
        if (!cw_reflect_position(&d->gw.world, d->selection[i], &x, &y)) continue;
        if (cw_reflect_set_position(&d->gw.world, d->selection[i], x + dx, y + dy)) ++n;
    }
    return n;
}
size_t ed_move_selection3(ed_document *d, float dx, float dy, float dz) {
    if (!d->world_ready) return 0;
    size_t n = 0;
    for (size_t i = 0; i < d->selection_count; ++i) {
        float x, y, z;
        if (!cw_reflect_position3(&d->gw.world, d->selection[i], &x, &y, &z)) continue;
        if (cw_reflect_set_position3(&d->gw.world, d->selection[i], x + dx, y + dy, z + dz)) ++n;
        if (d->texture_lock && d->gw.brush_ct != CW_INVALID_COMPONENT && cw_comp_has(&d->gw.world, d->selection[i], d->gw.brush_ct)) {
            float delta[3] = {dx, dy, dz};
            cw_brush_lock_textures(cw_comp_ptr(&d->gw.world, d->selection[i], d->gw.brush_ct), delta);
            cw_comp_touch(&d->gw.world, d->selection[i], d->gw.brush_ct);
        }
    }
    return n;
}
bool ed_is_3d(const ed_document *d) { return d && d->project_open && d->project.renderer == CW_RENDERER_3D; }
bool ed_set_renderer(ed_document *d, cw_renderer_kind kind) {
    if (!d->project_open) { ed_status(d, true, "no project is open"); return false; }
    if (d->project.renderer == kind) return true;
    d->project.renderer = kind;
    char error[256];
    if (!cw_project_save(&d->project, error, sizeof(error))) { ed_status(d, true, "%s", error); return false; }
    ed_status(d, false, "project saved: viewed in %s", cw_renderer_name(kind));
    return true;
}
bool ed_is_decal(const ed_document *d, cw_entity_id id) {
    return d && d->world_ready && d->gw.decal3d_ct != CW_INVALID_COMPONENT && cw_comp_has(&d->gw.world, id, d->gw.decal3d_ct);
}
cw_entity_id ed_create_decal(ed_document *d, const float point[3], const float normal[3], float size, float depth, uint32_t material) {
    if (!d->world_ready) return CW_INVALID_ENTITY;
    cw_world *w = &d->gw.world;
    cw_component_id pc = position_component(w);
    if (pc == CW_INVALID_COMPONENT || d->gw.decal3d_ct == CW_INVALID_COMPONENT) { ed_status(d, true, "decals need a position component with x, y and z"); return CW_INVALID_ENTITY; }
    cw_entity_id id = cw_world_create_entity(w);
    const cw_type_info *t = cw_comp_type(w, pc);
    unsigned char value[1024];
    if (t->size <= sizeof(value)) {
        cw_type_default(t, value);
        cw_comp_set(w, id, pc, value, t->size);
    }
    cw_decal3d decal;
    cw_type_default(&cw_decal3d_type, &decal);
    decal.width = decal.height = size > 0.01f ? size : 0.01f;
    decal.depth = depth > 0.01f ? depth : 0.01f;
    decal.material = material;
    if (normal) {
        /* the decal's local y must point along the normal: pitch it off the ground, then yaw it around */
        float nx = normal[0], ny = normal[1], nz = normal[2], len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len > 1e-6f) {
            nx /= len; ny /= len; nz /= len;
            float h = sqrtf(nx * nx + nz * nz);
            if (h > 1e-4f) {
                /* Euler roll -> pitch -> yaw: pitch rx turns +y towards +z (rx = 90 deg: +y -> +z), yaw ry turns +z towards +x */
                decal.rx = atan2f(h, ny);
                decal.ry = atan2f(nx, nz);
            } else if (ny < 0) decal.rx = 3.1415927f;   /* a ceiling */
        }
    }
    cw_comp_set(w, id, d->gw.decal3d_ct, &decal, sizeof(decal));
    cw_reflect_set_position3(w, id, point[0], point[1], point[2]);
    cw_game_world_settle(&d->gw);
    ed_select_set(d, id);
    ed_commit(d, "create decal");
    ed_status(d, false, "decal #%" PRId64 " at %.1f, %.1f, %.1f", id, (double)point[0], (double)point[1], (double)point[2]);
    return id;
}
size_t ed_rotate_selection(ed_document *d, float drx, float dry, float drz) {
    if (!d->world_ready) return 0;
    size_t n = 0;
    for (size_t i = 0; i < d->selection_count; ++i) {
        float rx, ry, rz;
        if (!cw_reflect_rotation3(&d->gw.world, d->selection[i], &rx, &ry, &rz)) continue;
        if (cw_reflect_set_rotation3(&d->gw.world, d->selection[i], rx + drx, ry + dry, rz + drz)) ++n;
    }
    return n;
}
size_t ed_scale_selection(ed_document *d, float fx, float fy, float fz) {
    if (!d->world_ready) return 0;
    size_t n = 0;
    for (size_t i = 0; i < d->selection_count; ++i) {
        float sx, sy, sz;
        if (!cw_reflect_size3(&d->gw.world, d->selection[i], &sx, &sy, &sz)) continue;
        sx *= fx; sy *= fy; sz *= fz;
        if (cw_reflect_set_size3(&d->gw.world, d->selection[i], sx < 0.01f ? 0.01f : sx, sy < 0.01f ? 0.01f : sy, sz < 0.01f ? 0.01f : sz)) ++n;
    }
    return n;
}

/* --- brushes --- */
static cw_brush *brush_of(ed_document *d, cw_entity_id id) {
    if (!d->world_ready || d->gw.brush_ct == CW_INVALID_COMPONENT || !cw_world_is_valid(&d->gw.world, id) || !cw_comp_has(&d->gw.world, id, d->gw.brush_ct)) return NULL;
    return cw_comp_ptr(&d->gw.world, id, d->gw.brush_ct);
}
bool ed_is_brush(const ed_document *d, cw_entity_id id) { return brush_of((ed_document *)d, id) != NULL; }
bool ed_brush_bounds(ed_document *d, cw_entity_id id, float lo[3], float hi[3]) {
    const cw_brush *b = brush_of(d, id);
    float c[3];
    if (!b || !cw_reflect_position3(&d->gw.world, id, &c[0], &c[1], &c[2])) return false;
    cw_brush_bounds(b, c, lo, hi);
    return true;
}
static cw_entity_id create_brush(ed_document *d, const float lo[3], const float hi[3], uint32_t material, int wedge_axis, float wedge_sign);
cw_entity_id ed_create_brush(ed_document *d, const float lo[3], const float hi[3], uint32_t material) { return create_brush(d, lo, hi, material, -1, 0); }
cw_entity_id ed_create_wedge(ed_document *d, const float lo[3], const float hi[3], uint32_t material, int axis, float sign) { return create_brush(d, lo, hi, material, axis, sign); }
static cw_entity_id create_brush(ed_document *d, const float lo[3], const float hi[3], uint32_t material, int wedge_axis, float wedge_sign) {
    if (!d->world_ready) return CW_INVALID_ENTITY;
    cw_world *w = &d->gw.world;
    cw_component_id pc = position_component(w);
    if (pc == CW_INVALID_COMPONENT || d->gw.brush_ct == CW_INVALID_COMPONENT) { ed_status(d, true, "brushes need a position component with x, y and z"); return CW_INVALID_ENTITY; }
    cw_entity_id id = cw_world_create_entity(w);
    const cw_type_info *t = cw_comp_type(w, pc);
    unsigned char value[1024];
    if (t->size <= sizeof(value)) {
        cw_type_default(t, value);
        cw_comp_set(w, id, pc, value, t->size);
    }
    cw_brush brush;
    cw_type_default(&cw_brush_type, &brush);
    float center[3];
    cw_brush_set_bounds(&brush, center, lo, hi, 0.01f);
    for (int f = 0; f < CW_BRUSH_MAX_FACES; ++f) brush.material[f] = material;
    bool wedge = wedge_axis >= 0 && cw_brush_make_wedge(&brush, center, wedge_axis, wedge_sign);
    cw_comp_set(w, id, d->gw.brush_ct, &brush, sizeof(brush));
    cw_reflect_set_position3(w, id, center[0], center[1], center[2]);
    cw_game_world_settle(&d->gw);
    ed_select_set(d, id);
    ed_commit(d, wedge ? "create wedge" : "create brush");
    ed_status(d, false, "%s #%" PRId64 ": %.2g x %.2g x %.2g at %.1f, %.1f, %.1f", wedge ? "wedge" : "brush", id, (double)brush.width, (double)brush.height, (double)brush.depth,
              (double)center[0], (double)center[1], (double)center[2]);
    return id;
}
size_t ed_clip_selection(ed_document *d, const float n[3], float dist, ed_clip_mode mode) {
    if (!d->world_ready) return 0;
    cw_world *w = &d->gw.world;
    cw_plane back = {{n[0], n[1], n[2]}, dist}, front = {{-n[0], -n[1], -n[2]}, -dist};   /* the solid keeps n . p <= d */
    size_t cut = 0, created = 0, removed = 0;
    size_t count = d->selection_count;
    cw_entity_id *targets = malloc(count * sizeof(*targets));
    if (!targets) return 0;
    memcpy(targets, d->selection, count * sizeof(*targets));
    for (size_t i = 0; i < count; ++i) {
        cw_entity_id id = targets[i];
        cw_brush *b = brush_of(d, id);
        float c[3];
        if (!b || !cw_reflect_position3(w, id, &c[0], &c[1], &c[2])) continue;
        if (cw_brush_plane_side(b, c, &back) != 0) continue;   /* the plane misses this brush */
        if (mode == ED_CLIP_KEEP_BOTH) {
            cw_entity_id copy = cw_world_create_entity(w);
            cw_reflect_copy_entity(w, id, copy);
            const char *name = ed_name_of(d, id);
            if (*name) {
                char text[CW_SCENE_NAME_CAPACITY];
                snprintf(text, sizeof(text), "%.*s cut", (int)(sizeof(text) - 5), name);
                set_name(d, copy, text);
            }
            cw_brush *cb = brush_of(d, copy);
            float cc[3] = {c[0], c[1], c[2]};
            if (cb && cw_brush_clip(cb, cc, &front)) { cw_reflect_set_position3(w, copy, cc[0], cc[1], cc[2]); ++created; ed_select_add(d, copy); }
            else { cw_world_mark_destroy(w, copy); }
        }
        if (cw_brush_clip(b, c, mode == ED_CLIP_KEEP_FRONT ? &front : &back)) {
            cw_comp_touch(w, id, d->gw.brush_ct);
            cw_reflect_set_position3(w, id, c[0], c[1], c[2]);
            ++cut;
        } else ++removed;
    }
    free(targets);
    cw_game_world_settle(&d->gw);
    if (cut || created) {
        ed_commit(d, "clip");
        ed_status(d, false, "clipped %zu brush%s%s", cut, cut == 1 ? "" : "es", created ? " (both sides kept)" : "");
    } else if (removed) ed_status(d, true, "the cut would leave nothing, or the brush already has %d clips", CW_BRUSH_MAX_CLIPS);
    else ed_status(d, true, "the plane misses the selected brushes");
    return cut + created;
}
/* A new entity carrying `like`'s components and name, with this brush at this centre. */
static cw_entity_id spawn_piece(ed_document *d, cw_entity_id like, const cw_brush *piece, const float center[3], const char *suffix) {
    cw_world *w = &d->gw.world;
    cw_entity_id id = cw_world_create_entity(w);
    cw_reflect_copy_entity(w, like, id);
    cw_comp_set(w, id, d->gw.brush_ct, piece, sizeof(*piece));
    cw_reflect_set_position3(w, id, center[0], center[1], center[2]);
    const char *name = ed_name_of(d, like);
    if (*name) {
        char text[CW_SCENE_NAME_CAPACITY];
        snprintf(text, sizeof(text), "%.*s %s", (int)(sizeof(text) - strlen(suffix) - 2), name, suffix);
        set_name(d, id, text);
    }
    return id;
}
size_t ed_csg_subtract(ed_document *d) {
    if (!d->world_ready || d->gw.brush_ct == CW_INVALID_COMPONENT) return 0;
    cw_world *w = &d->gw.world;
    size_t cutters = 0;
    for (size_t i = 0; i < d->selection_count; ++i) if (brush_of(d, d->selection[i])) ++cutters;
    if (!cutters) { ed_status(d, true, "select the brush to carve with first"); return 0; }
    /* the targets: every brush that is not a cutter */
    cw_entity_id targets[4096];
    size_t target_count = 0;
    cw_query q = cw_query_iter(w, cw_mask(&d->gw.brush_ct, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id) && target_count < 4096) if (!ed_is_selected(d, id)) targets[target_count++] = id;
    size_t made = 0, carved = 0;
    int dropped_total = 0;
    cw_entity_id *cutter_ids = malloc(d->selection_count * sizeof(*cutter_ids));
    if (!cutter_ids) return 0;
    size_t cutter_n = 0;
    for (size_t i = 0; i < d->selection_count; ++i) if (brush_of(d, d->selection[i])) cutter_ids[cutter_n++] = d->selection[i];
    for (size_t t = 0; t < target_count; ++t) {
        /* the target's pieces so far (it starts as itself); every cutter carves every piece */
        cw_brush work[CW_BRUSH_MAX_FACES * 4];
        float centers[CW_BRUSH_MAX_FACES * 4][3];
        int work_n = 1;
        work[0] = *brush_of(d, targets[t]);
        if (!cw_reflect_position3(w, targets[t], &centers[0][0], &centers[0][1], &centers[0][2])) continue;
        bool touched = false;
        for (size_t c = 0; c < cutter_n; ++c) {
            cw_plane planes[CW_BRUSH_MAX_FACES];
            float cc[3];
            if (!cw_reflect_position3(w, cutter_ids[c], &cc[0], &cc[1], &cc[2])) continue;
            int np = cw_brush_planes(brush_of(d, cutter_ids[c]), cc, planes);
            cw_brush next[CW_BRUSH_MAX_FACES * 4];
            float next_c[CW_BRUSH_MAX_FACES * 4][3];
            int next_n = 0;
            for (int k = 0; k < work_n; ++k) {
                cw_brush pieces[CW_BRUSH_MAX_FACES];
                float pc[CW_BRUSH_MAX_FACES][3];
                int dropped = 0, n = cw_brush_subtract(&work[k], centers[k], planes, np, pieces, pc, &dropped);
                dropped_total += dropped;
                if (n < 0) {   /* untouched by this cutter */
                    if (next_n < CW_BRUSH_MAX_FACES * 4) { next[next_n] = work[k]; memcpy(next_c[next_n], centers[k], sizeof(pc[0])); ++next_n; }
                    continue;
                }
                touched = true;
                for (int p = 0; p < n && next_n < CW_BRUSH_MAX_FACES * 4; ++p) { next[next_n] = pieces[p]; memcpy(next_c[next_n], pc[p], sizeof(pc[0])); ++next_n; }
            }
            memcpy(work, next, sizeof(cw_brush) * (size_t)next_n);
            memcpy(centers, next_c, sizeof(centers[0]) * (size_t)next_n);
            work_n = next_n;
        }
        if (!touched) continue;
        ++carved;
        for (int k = 0; k < work_n; ++k) { spawn_piece(d, targets[t], &work[k], centers[k], "piece"); ++made; }
        cw_world_mark_destroy(w, targets[t]);
    }
    if (carved) {
        for (size_t c = 0; c < cutter_n; ++c) cw_world_mark_destroy(w, cutter_ids[c]);
        ed_select_clear(d);
        cw_game_world_settle(&d->gw);
        ed_commit(d, "subtract");
        ed_status(d, false, "carved %zu brush%s into %zu piece%s%s", carved, carved == 1 ? "" : "es", made, made == 1 ? "" : "s", dropped_total ? " (some slivers needed too many planes and were dropped)" : "");
    } else ed_status(d, true, "the selected brushes touch no other brush");   /* nothing changed: the cutters stay */
    free(cutter_ids);
    return made;
}
size_t ed_csg_hollow(ed_document *d, float thickness) {
    if (!d->world_ready || thickness <= 0) return 0;
    cw_world *w = &d->gw.world;
    size_t made = 0, hollowed = 0;
    cw_entity_id *targets = malloc(d->selection_count * sizeof(*targets));
    if (!targets) return 0;
    size_t count = d->selection_count;
    memcpy(targets, d->selection, count * sizeof(*targets));
    ed_select_clear(d);
    for (size_t i = 0; i < count; ++i) {
        const cw_brush *b = brush_of(d, targets[i]);
        float c[3];
        if (!b || !cw_reflect_position3(w, targets[i], &c[0], &c[1], &c[2])) { ed_select_add(d, targets[i]); continue; }
        cw_brush pieces[CW_BRUSH_MAX_FACES];
        float pc[CW_BRUSH_MAX_FACES][3];
        int n = cw_brush_hollow(b, c, thickness, pieces, pc, NULL);
        if (n <= 0) { ed_select_add(d, targets[i]); continue; }
        for (int k = 0; k < n; ++k) { cw_entity_id piece = spawn_piece(d, targets[i], &pieces[k], pc[k], "wall"); ed_select_add(d, piece); ++made; }
        cw_world_mark_destroy(w, targets[i]);
        ++hollowed;
    }
    free(targets);
    cw_game_world_settle(&d->gw);
    if (hollowed) { ed_commit(d, "hollow"); ed_status(d, false, "hollowed %zu brush%s into %zu walls %.2g thick", hollowed, hollowed == 1 ? "" : "es", made, (double)thickness); }
    else ed_status(d, true, "select brushes to hollow (too thin ones cannot be)");
    return made;
}
size_t ed_snap_brushes(ed_document *d, float spacing) {
    if (!d->world_ready || spacing <= 0) return 0;
    size_t moved = 0;
    for (size_t i = 0; i < d->selection_count; ++i) {
        cw_brush *b = brush_of(d, d->selection[i]);
        float c[3];
        if (!b || !cw_reflect_position3(&d->gw.world, d->selection[i], &c[0], &c[1], &c[2])) continue;
        if (!cw_brush_snap_to_grid(b, c, spacing)) continue;
        cw_comp_touch(&d->gw.world, d->selection[i], d->gw.brush_ct);
        cw_reflect_set_position3(&d->gw.world, d->selection[i], c[0], c[1], c[2]);
        ++moved;
    }
    if (moved) { ed_commit(d, "snap to grid"); ed_status(d, false, "snapped %zu brush%s to the %g grid", moved, moved == 1 ? "" : "es", (double)spacing); }
    else ed_status(d, false, "the selected brushes already sit on the grid");
    return moved;
}
bool ed_brush_move_vertex(ed_document *d, cw_entity_id id, const float from[3], const float to[3]) {
    cw_brush *b = brush_of(d, id);
    float c[3];
    if (!b || !cw_reflect_position3(&d->gw.world, id, &c[0], &c[1], &c[2])) return false;
    if (!cw_brush_move_vertex(b, c, from, to)) return false;
    cw_comp_touch(&d->gw.world, id, d->gw.brush_ct);
    return cw_reflect_set_position3(&d->gw.world, id, c[0], c[1], c[2]);
}
bool ed_brush_set_face_align(ed_document *d, cw_entity_id id, int face, const cw_brush_align *align) {
    cw_brush *b = brush_of(d, id);
    if (!b || !align || face < 0 || face >= CW_BRUSH_MAX_FACES) return false;
    cw_brush_face_align_set(b, face, align);
    cw_comp_touch(&d->gw.world, id, d->gw.brush_ct);
    ed_commit(d, "align texture");
    return true;
}
bool ed_brush_set_face(ed_document *d, cw_entity_id id, int face, float plane) {
    cw_brush *b = brush_of(d, id);
    if (!b || face < 0 || face >= CW_BRUSH_FACES) return false;
    float c[3], lo[3], hi[3];
    if (!cw_reflect_position3(&d->gw.world, id, &c[0], &c[1], &c[2])) return false;
    cw_brush_bounds(b, c, lo, hi);
    int axis = cw_brush_face_axis((cw_brush_face)face);
    const float min_size = 0.05f;
    if (cw_brush_face_sign((cw_brush_face)face) > 0) hi[axis] = plane > lo[axis] + min_size ? plane : lo[axis] + min_size;
    else lo[axis] = plane < hi[axis] - min_size ? plane : hi[axis] - min_size;
    cw_brush_set_bounds(b, c, lo, hi, min_size);
    cw_comp_touch(&d->gw.world, id, d->gw.brush_ct);
    return cw_reflect_set_position3(&d->gw.world, id, c[0], c[1], c[2]);
}
bool ed_brush_set_face_material(ed_document *d, cw_entity_id id, int face, uint32_t material) {
    cw_brush *b = brush_of(d, id);
    if (!b || face >= CW_BRUSH_MAX_FACES) return false;
    for (int f = 0; f < CW_BRUSH_MAX_FACES; ++f) if (face < 0 || f == face) b->material[f] = material;
    cw_comp_touch(&d->gw.world, id, d->gw.brush_ct);
    const cw_asset *a = cw_assets_find(&d->assets, material);
    ed_commit(d, "texture face");
    if (face < 0) ed_status(d, false, "brush #%" PRId64 ": every face now %s", id, a ? a->name : "plain");
    else ed_status(d, false, "brush #%" PRId64 " %s face: %s", id, cw_brush_face_names[face], a ? a->name : "plain");
    return true;
}
size_t ed_paint(ed_document *d, const float point[3], float radius, uint8_t r, uint8_t g, uint8_t b, float strength) {
    if (!d->world_ready || d->gw.brush_ct == CW_INVALID_COMPONENT) return 0;
    cw_world *w = &d->gw.world;
    cw_query q = cw_query_iter(w, cw_mask(&d->gw.brush_ct, 1));
    cw_entity_id id;
    size_t touched = 0;
    while (cw_query_next(&q, &id)) {
        float c[3];
        if (!cw_reflect_position3(w, id, &c[0], &c[1], &c[2])) continue;
        cw_brush *brush = cw_comp_ptr(w, id, d->gw.brush_ct);
        size_t n = cw_brush_paint_at(brush, c, point, radius, r, g, b, strength);
        if (n) { touched += n; cw_comp_touch(w, id, d->gw.brush_ct); }
    }
    return touched;
}
size_t ed_paint_clear_selection(ed_document *d) {
    size_t n = 0;
    for (size_t i = 0; i < d->selection_count; ++i) {
        cw_brush *b = brush_of(d, d->selection[i]);
        if (!b || !b->paint.size) continue;
        cw_brush_paint_clear(b);
        cw_comp_touch(&d->gw.world, d->selection[i], d->gw.brush_ct);
        ++n;
    }
    if (n) { ed_commit(d, "clear paint"); ed_status(d, false, "cleared the paint of %zu brush%s", n, n == 1 ? "" : "es"); }
    else ed_status(d, false, "no painted brush selected");
    return n;
}
static bool apply_value(ed_document *d, cw_entity_id id, cw_component_id ct, const cw_type_info *t, const void *value, const char *label) {
    if (!cw_type_check(t, value)) { ed_status(d, true, "%s would be invalid", t->name); return false; }
    cw_comp_set(&d->gw.world, id, ct, value, t->size);
    ed_commit(d, label);
    return true;
}
static const cw_type_info *editable_type(ed_document *d, cw_entity_id id, cw_component_id ct) {
    if (!d->world_ready || !cw_world_is_valid(&d->gw.world, id)) return NULL;
    const cw_type_info *t = cw_comp_type(&d->gw.world, ct);
    if (!t || !cw_comp_has(&d->gw.world, id, ct)) return NULL;
    return t;
}
bool ed_set_field(ed_document *d, cw_entity_id id, cw_component_id ct, const cw_field *f, const char *text) {
    const cw_type_info *t = editable_type(d, id, ct);
    if (!t || !f) return false;
    unsigned char value[1024];
    if (t->size > sizeof(value)) return false;
    memcpy(value, cw_comp_ptr(&d->gw.world, id, ct), t->size);
    if (!cw_field_parse(f, value, text)) { ed_status(d, true, "'%s' is not a valid %s for %s.%s", text, cw_field_type_name(f->type), t->name, f->name); return false; }
    char label[96];
    snprintf(label, sizeof(label), "set %s.%s", t->name, f->name);
    if (!apply_value(d, id, ct, t, value, label)) return false;
    ed_status(d, false, "%s.%s = %s", t->name, f->name, text);
    return true;
}
bool ed_adjust_field(ed_document *d, cw_entity_id id, cw_component_id ct, const cw_field *f, int direction) {
    const cw_type_info *t = editable_type(d, id, ct);
    if (!t || !f) return false;
    unsigned char value[1024];
    if (t->size > sizeof(value)) return false;
    memcpy(value, cw_comp_ptr(&d->gw.world, id, ct), t->size);
    if (!cw_field_adjust(f, value, direction)) return false;
    char label[96], text[64];
    snprintf(label, sizeof(label), "adjust %s.%s", t->name, f->name);
    if (!apply_value(d, id, ct, t, value, label)) return false;
    cw_field_format(f, value, text, sizeof(text));
    ed_status(d, false, "%s.%s = %s", t->name, f->name, text);
    return true;
}
bool ed_add_component(ed_document *d, cw_entity_id id, cw_component_id ct) {
    if (!d->world_ready || !cw_world_is_valid(&d->gw.world, id)) return false;
    const cw_type_info *t = cw_comp_type(&d->gw.world, ct);
    if (!t || (t->flags & CW_TYPE_TRANSIENT) || cw_comp_has(&d->gw.world, id, ct)) return false;
    if ((t->flags & CW_TYPE_RUNTIME) && !d->playing) {
        ed_status(d, true, "%s is a runtime component: it is generated from config when the world runs", t->name);
        return false;
    }
    unsigned char value[1024];
    if (t->size > sizeof(value)) return false;
    cw_type_default(t, value);
    cw_comp_set(&d->gw.world, id, ct, value, t->size);
    char label[96];
    snprintf(label, sizeof(label), "add %s", t->name);
    ed_commit(d, label);
    ed_status(d, false, "added %s to #%" PRId64, t->name, id);
    return true;
}
bool ed_remove_component(ed_document *d, cw_entity_id id, cw_component_id ct) {
    const cw_type_info *t = editable_type(d, id, ct);
    if (!t) return false;
    cw_comp_remove(&d->gw.world, id, ct);
    char label[96];
    snprintf(label, sizeof(label), "remove %s", t->name);
    ed_commit(d, label);
    ed_status(d, false, "removed %s from #%" PRId64, t->name, id);
    return true;
}
bool ed_rename(ed_document *d, cw_entity_id id, const char *name) {
    if (!d->world_ready || !cw_world_is_valid(&d->gw.world, id)) return false;
    set_name(d, id, name);
    ed_commit(d, "rename");
    return true;
}

/* --- play mode --- */
bool ed_play(ed_document *d) {
    if (!d->world_ready) { ed_status(d, true, "nothing to play"); return false; }
    if (d->playing) { ed_toggle_pause(d); return true; }
    if (!capture(d, &d->play_backup, "play")) return false;
    d->playing = true;
    d->paused = false;
    d->play_ticks = 0;
    cw_clock_reset(&d->gw.clock);
    cw_game_world_set_baking(&d->gw, true);
    size_t baked = cw_game_world_bake_all(&d->gw);
    if (baked) ed_status(d, false, "playing: %zu runtime component%s generated from config (Space pauses, Escape or F5 stops)", baked, baked == 1 ? "" : "s");
    else ed_status(d, false, "playing (Space pauses, Escape or F5 stops and restores the scene)");
    return true;
}
void ed_stop(ed_document *d) {
    if (!d->playing) return;
    d->playing = false;
    d->paused = false;
    cw_game_world_set_baking(&d->gw, false);
    restore(d, &d->play_backup, false);   /* runtime components are not in the image: they are gone again */
    image_free(&d->play_backup);
    cw_clock_reset(&d->gw.clock);
    ed_status(d, false, "stopped after %" PRIu64 " ticks: scene restored", d->play_ticks);
}
void ed_toggle_pause(ed_document *d) {
    if (!d->playing) return;
    d->paused = !d->paused;
    cw_clock_reset(&d->gw.clock);
    ed_status(d, false, d->paused ? "paused at tick %" PRIu64 : "resumed at tick %" PRIu64, d->gw.world.tick);
}
void ed_step(ed_document *d, int ticks) {
    if (!d->world_ready) return;
    if (!d->playing) ed_play(d);
    d->paused = true;
    cw_game_world_tick(&d->gw, ticks);
    d->play_ticks += (uint64_t)ticks;
    cw_clock_reset(&d->gw.clock);
    ed_status(d, false, "stepped to tick %" PRIu64, d->gw.world.tick);
}
int ed_frame(ed_document *d, float dt) {
    if (!d->world_ready) return 0;
    if (!d->playing || d->paused) { cw_game_world_settle(&d->gw); return 0; }
    int steps = cw_game_world_advance(&d->gw, dt);
    d->play_ticks += (uint64_t)steps;
    selection_prune(d);
    return steps;
}
