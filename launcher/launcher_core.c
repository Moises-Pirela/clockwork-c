#ifndef _WIN32
#define _DEFAULT_SOURCE 1
#endif
#define _CRT_SECURE_NO_WARNINGS 1
#include "launcher_core.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define popen _popen
#define pclose _pclose
#define make_dir(path) _mkdir(path)
#else
#include <dirent.h>
#include <unistd.h>
#define make_dir(path) mkdir(path, 0755)
#endif

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
static void quote(char *cmd, size_t capacity, const char *arg) {
    append_str(cmd, capacity, " \"");
    append_str(cmd, capacity, arg);
    append_str(cmd, capacity, "\"");
}
static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
static bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}
static time_t mtime_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}
static bool write_text(const char *path, const char *text) {
    cw_file_error err;
    return cw_file_write(path, text, strlen(text), &err);
}

void lc_status(lc_launcher *l, bool error, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(l->status, sizeof(l->status), fmt, args);
    va_end(args);
    l->status_error = error;
    printf("%s%s\n", error ? "ERROR: " : "", l->status);
    fflush(stdout);
}

/* --- entries --- */
static int compare_names(const void *a, const void *b) { return strcmp(a, b); }
static size_t list_scenes(const char *dir, char (*names)[128], size_t capacity) {
    size_t n = 0;
#ifdef _WIN32
    char pattern[CW_GAME_PATH_CAPACITY + 16];
    copy_str(pattern, sizeof(pattern), dir);
    append_str(pattern, sizeof(pattern), "\\*.cwscene");
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { if (n < capacity) copy_str(names[n++], 128, data.cFileName); } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 9 || strcmp(e->d_name + len - 8, ".cwscene") != 0) continue;
        if (n < capacity) copy_str(names[n++], 128, e->d_name);
    }
    closedir(d);
#endif
    qsort(names, n, 128, compare_names);
    return n;
}
void lc_refresh_entry(lc_entry *e) {
    char error[160];
    e->loadable = cw_project_load(&e->project, e->path, error, sizeof(error));
    e->module_built = false;
    e->scene_count = 0;
    e->modified = mtime_of(e->path);
    e->error[0] = 0;
    if (!e->loadable) { copy_str(e->error, sizeof(e->error), error); return; }
    e->module_built = file_exists(e->project.module);
    e->scene_count = list_scenes(e->project.scenes_dir, e->scenes, LC_MAX_SCENES);
    for (size_t i = 0; i < e->scene_count; ++i) {
        char path[CW_GAME_PATH_CAPACITY];
        copy_str(path, sizeof(path), e->project.scenes_dir);
        append_str(path, sizeof(path), "/");
        append_str(path, sizeof(path), e->scenes[i]);
        time_t t = mtime_of(path);
        if (t > e->modified) e->modified = t;
    }
}
void lc_refresh(lc_launcher *l) {
    for (size_t i = 0; i < l->count; ++i) lc_refresh_entry(&l->entries[i]);
}

static void absolute_path(const char *path, char *out, size_t capacity) {
    cw_project p;   /* cw_project_load resolves paths; for a file that does not parse we fall back */
    char error[64];
    if (cw_project_load(&p, path, error, sizeof(error))) { copy_str(out, capacity, p.path); return; }
#ifdef _WIN32
    char full[CW_GAME_PATH_CAPACITY];
    DWORD n = GetFullPathNameA(path, sizeof(full), full, NULL);
    copy_str(out, capacity, n && n < sizeof(full) ? full : path);
#else
    char cwd[CW_GAME_PATH_CAPACITY];
    if (path[0] == '/' || !getcwd(cwd, sizeof(cwd))) { copy_str(out, capacity, path); return; }
    copy_str(out, capacity, cwd);
    append_str(out, capacity, "/");
    append_str(out, capacity, path);
#endif
}
int lc_find(const lc_launcher *l, const char *path) {
    if (!l || !path || !*path) return -1;
    char full[CW_GAME_PATH_CAPACITY];
    absolute_path(path, full, sizeof(full));
    for (size_t i = 0; i < l->count; ++i)
        if (strcmp(l->entries[i].path, full) == 0) return (int)i;
    return -1;
}
int lc_add(lc_launcher *l, const char *path) {
    if (!path || !*path) return -1;
    char full[CW_GAME_PATH_CAPACITY];
    absolute_path(path, full, sizeof(full));
    int at = lc_find(l, full);
    if (at >= 0) { lc_refresh_entry(&l->entries[at]); return at; }
    if (!file_exists(full)) { lc_status(l, true, "no project file at %s", full); return -1; }
    if (l->count == LC_MAX_PROJECTS) { lc_status(l, true, "the registry is full"); return -1; }
    lc_entry *e = &l->entries[l->count++];
    memset(e, 0, sizeof(*e));
    copy_str(e->path, sizeof(e->path), full);
    lc_refresh_entry(e);
    return (int)l->count - 1;
}
bool lc_remove(lc_launcher *l, size_t index) {
    if (index >= l->count) return false;
    memmove(&l->entries[index], &l->entries[index + 1], (l->count - index - 1) * sizeof(*l->entries));
    --l->count;
    return true;
}
void lc_touch(lc_launcher *l, size_t index) {
    if (index >= l->count || index == 0) return;
    lc_entry e = l->entries[index];
    memmove(&l->entries[1], &l->entries[0], index * sizeof(*l->entries));
    l->entries[0] = e;
}

/* --- registry file --- */
void lc_init(lc_launcher *l, const char *engine_root, const char *tool_dir) {
    memset(l, 0, sizeof(*l));
    copy_str(l->engine_root, sizeof(l->engine_root), engine_root ? engine_root : "");
    const char *registry = getenv("CW_PROJECTS");
    if (registry && *registry) copy_str(l->registry, sizeof(l->registry), registry);
    else {
        copy_str(l->registry, sizeof(l->registry), l->engine_root[0] ? l->engine_root : ".");
        append_str(l->registry, sizeof(l->registry), "/projects.cwlist");
    }
    const char *editor = getenv("CW_EDITOR"), *player = getenv("CW_PLAYER");
#ifdef _WIN32
    const char *exe = ".exe";
#else
    const char *exe = "";
#endif
    if (editor && *editor) copy_str(l->editor, sizeof(l->editor), editor);
    else { copy_str(l->editor, sizeof(l->editor), tool_dir ? tool_dir : ""); append_str(l->editor, sizeof(l->editor), "clockwork_editor"); append_str(l->editor, sizeof(l->editor), exe); }
    if (player && *player) copy_str(l->player, sizeof(l->player), player);
    else { copy_str(l->player, sizeof(l->player), tool_dir ? tool_dir : ""); append_str(l->player, sizeof(l->player), "clockwork_player"); append_str(l->player, sizeof(l->player), exe); }
    lc_status(l, false, "select a project, or create one");
}
static void scan_games(lc_launcher *l) {
    if (!l->engine_root[0]) return;
    char games[CW_GAME_PATH_CAPACITY];
    copy_str(games, sizeof(games), l->engine_root);
    append_str(games, sizeof(games), "/games");
#ifdef _WIN32
    char pattern[CW_GAME_PATH_CAPACITY + 8];
    copy_str(pattern, sizeof(pattern), games);
    append_str(pattern, sizeof(pattern), "\\*");
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || data.cFileName[0] == '.') continue;
        char path[CW_GAME_PATH_CAPACITY];
        copy_str(path, sizeof(path), games);
        append_str(path, sizeof(path), "\\"); append_str(path, sizeof(path), data.cFileName);
        append_str(path, sizeof(path), "\\"); append_str(path, sizeof(path), data.cFileName);
        append_str(path, sizeof(path), ".cwproj");
        if (file_exists(path)) lc_add(l, path);
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR *d = opendir(games);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        /* a project directory holds <dir>/<dir>.cwproj by convention */
        char path[CW_GAME_PATH_CAPACITY];
        copy_str(path, sizeof(path), games);
        append_str(path, sizeof(path), "/"); append_str(path, sizeof(path), e->d_name);
        append_str(path, sizeof(path), "/"); append_str(path, sizeof(path), e->d_name);
        append_str(path, sizeof(path), ".cwproj");
        if (file_exists(path)) lc_add(l, path);
    }
    closedir(d);
#endif
}
void lc_load(lc_launcher *l) {
    l->count = 0;
    cw_buffer text;
    cw_file_error err;
    if (cw_file_read(l->registry, 1 << 20, &text, &err)) {
        const char *p = (const char *)text.data, *end = p + text.size;
        bool header = false;
        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
            char line[CW_GAME_PATH_CAPACITY];
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, p, len);
            line[len] = 0;
            while (len && isspace((unsigned char)line[len - 1])) line[--len] = 0;
            p += (nl ? (size_t)(nl - p) + 1 : (size_t)(end - p));
            if (!header) { header = true; continue; }   /* "cwprojects 1" */
            if (line[0] && line[0] != '#') lc_add(l, line);
        }
        cw_buffer_free(&text);
    }
    size_t known = l->count;
    scan_games(l);
    if (l->count > known) lc_save(l);
    lc_status(l, false, "%zu project%s", l->count, l->count == 1 ? "" : "s");
}
bool lc_save(const lc_launcher *l) {
    cw_writer out;
    cw_writer_init(&out);
    cw_writer_bytes(&out, "cwprojects 1\n", 13);
    for (size_t i = 0; i < l->count; ++i) {
        cw_writer_bytes(&out, l->entries[i].path, strlen(l->entries[i].path));
        cw_writer_u8(&out, '\n');
    }
    cw_file_error err;
    bool ok = cw_file_write(l->registry, out.data, out.size, &err);
    cw_writer_destroy(&out);
    return ok;
}

/* --- creating --- */
bool lc_name_valid(const char *name) {
    if (!name || !*name || strlen(name) > 40) return false;
    if (!isalpha((unsigned char)name[0])) return false;
    for (const char *p = name; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '_') return false;
    return true;
}
const char *lc_template_dir(lc_template kind) { return kind == LC_TEMPLATE_3D ? "launcher/template3d" : "launcher/template"; }
const char *lc_template_name(lc_template kind) { return kind == LC_TEMPLATE_3D ? "3D" : "2D"; }
static bool read_template(const lc_launcher *l, lc_template kind, const char *file, const char *name, char **out) {
    char path[CW_GAME_PATH_CAPACITY];
    copy_str(path, sizeof(path), l->engine_root);
    append_str(path, sizeof(path), "/");
    append_str(path, sizeof(path), lc_template_dir(kind));
    append_str(path, sizeof(path), "/");
    append_str(path, sizeof(path), file);
    cw_buffer text;
    cw_file_error err;
    if (!cw_file_read(path, 1 << 20, &text, &err)) return false;
    /* replace every "starter" with the project name */
    const char *needle = "\"starter\"";
    size_t nlen = strlen(needle), name_len = strlen(name), count = 0;
    for (const char *p = (const char *)text.data; (p = strstr(p, needle)) != NULL; p += nlen) ++count;
    size_t size = text.size + count * (name_len + 2) + 1;
    char *result = malloc(size);
    if (!result) { cw_buffer_free(&text); return false; }
    char *w = result;
    const char *p = (const char *)text.data, *end = p + text.size;
    while (p < end) {
        const char *hit = strstr(p, needle);
        if (!hit || hit >= end) { memcpy(w, p, (size_t)(end - p)); w += end - p; break; }
        memcpy(w, p, (size_t)(hit - p)); w += hit - p;
        *w++ = '"'; memcpy(w, name, name_len); w += name_len; *w++ = '"';
        p = hit + nlen;
    }
    *w = 0;
    cw_buffer_free(&text);
    *out = result;
    return true;
}
/* In the scene template the game line is unquoted: "game starter 1". */
static void patch_scene_game(char *scene, const char *name, char **out) {
    const char *line = "game starter 1";
    char *hit = strstr(scene, line);
    size_t size = strlen(scene) + strlen(name) + 16;
    char *result = malloc(size);
    if (!result) { *out = NULL; return; }
    if (!hit) { copy_str(result, size, scene); *out = result; return; }
    size_t prefix = (size_t)(hit - scene);
    memcpy(result, scene, prefix);
    result[prefix] = 0;
    append_str(result, size, "game ");
    append_str(result, size, name);
    append_str(result, size, " 1");
    append_str(result, size, hit + strlen(line));
    *out = result;
}
/* Copies every file under launcher/template/<rel> (except the two patched
 * templates) into the new project, recreating directories as needed. */
static bool copy_file(const char *from, const char *to) {
    cw_buffer data;
    cw_file_error err;
    if (!cw_file_read(from, 64u << 20, &data, &err)) return false;
    bool ok = cw_file_write(to, data.data, data.size, &err);
    cw_buffer_free(&data);
    return ok;
}
static bool copy_template_tree(const char *from, const char *to, int depth) {
    if (depth > 8) return true;
    bool ok = true;
#ifdef _WIN32
    char pattern[CW_GAME_PATH_CAPACITY + 4];
    copy_str(pattern, sizeof(pattern), from);
    append_str(pattern, sizeof(pattern), "\\*");
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return true;
    do {
        const char *name = data.cFileName;
        bool is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    DIR *d = opendir(from);
    if (!d) return true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
#endif
        if (name[0] == '.' || strcmp(name, "game.c") == 0 || strcmp(name, "main.cwscene") == 0) continue;
        char src[CW_GAME_PATH_CAPACITY], dst[CW_GAME_PATH_CAPACITY];
        copy_str(src, sizeof(src), from); append_str(src, sizeof(src), "/"); append_str(src, sizeof(src), name);
        copy_str(dst, sizeof(dst), to); append_str(dst, sizeof(dst), "/"); append_str(dst, sizeof(dst), name);
#ifndef _WIN32
        bool is_dir = dir_exists(src);
#endif
        if (is_dir) {
            if (!dir_exists(dst) && make_dir(dst) != 0) ok = false;
            else ok = copy_template_tree(src, dst, depth + 1) && ok;
        } else if (!copy_file(src, dst)) {
            ok = false;
        }
#ifdef _WIN32
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    }
    closedir(d);
#endif
    return ok;
}
bool lc_create(lc_launcher *l, const char *parent, const char *name, lc_template kind, size_t *index) {
    if ((unsigned)kind >= LC_TEMPLATE_KINDS) kind = LC_TEMPLATE_2D;
    if (!lc_name_valid(name)) { lc_status(l, true, "'%s' is not a valid project name (letters, digits and _ , starting with a letter)", name ? name : ""); return false; }
    if (!parent || !*parent) { lc_status(l, true, "choose a directory for the project"); return false; }
    if (!dir_exists(parent)) { lc_status(l, true, "directory %s does not exist", parent); return false; }
    char dir[CW_GAME_PATH_CAPACITY], scenes[CW_GAME_PATH_CAPACITY], file[CW_GAME_PATH_CAPACITY];
    copy_str(dir, sizeof(dir), parent);
    append_str(dir, sizeof(dir), "/");
    append_str(dir, sizeof(dir), name);
    if (dir_exists(dir)) { lc_status(l, true, "%s already exists", dir); return false; }
    char *game = NULL, *scene_raw = NULL, *scene = NULL;
    if (!read_template(l, kind, "game.c", name, &game) || !read_template(l, kind, "main.cwscene", name, &scene_raw)) {
        free(game);
        lc_status(l, true, "cannot read the templates under %s/%s", l->engine_root, lc_template_dir(kind));
        return false;
    }
    patch_scene_game(scene_raw, name, &scene);
    free(scene_raw);
    bool ok = scene != NULL && make_dir(dir) == 0;
    copy_str(scenes, sizeof(scenes), dir);
    append_str(scenes, sizeof(scenes), "/scenes");
    ok = ok && make_dir(scenes) == 0;
    copy_str(file, sizeof(file), dir); append_str(file, sizeof(file), "/"); append_str(file, sizeof(file), name); append_str(file, sizeof(file), "_game.c");
    ok = ok && write_text(file, game);
    copy_str(file, sizeof(file), scenes); append_str(file, sizeof(file), "/main.cwscene");
    ok = ok && write_text(file, scene);
    /* the rest of the template (starter images, materials, ...) is copied verbatim */
    copy_str(file, sizeof(file), l->engine_root);
    append_str(file, sizeof(file), "/");
    append_str(file, sizeof(file), lc_template_dir(kind));
    ok = ok && copy_template_tree(file, dir, 0);
    char project[1024];
    snprintf(project, sizeof(project),
             "cwproject 1\nname %s\n# the shared library the editor and player load (platform suffix added)\nmodule build/%s_game\n"
             "# sources compiled into the module and into exported games\nsource %s_game.c\nscenes scenes\nmain_scene scenes/main.cwscene\n%s",
             name, name, name, kind == LC_TEMPLATE_3D ? "# how the hosts view the world: 2d or 3d\nrenderer 3d\n" : "");
    copy_str(file, sizeof(file), dir); append_str(file, sizeof(file), "/"); append_str(file, sizeof(file), name); append_str(file, sizeof(file), ".cwproj");
    ok = ok && write_text(file, project);
    free(game);
    free(scene);
    if (!ok) { lc_status(l, true, "could not create the project files in %s", dir); return false; }
    int at = lc_add(l, file);
    if (at < 0) return false;
    lc_touch(l, (size_t)at);
    lc_save(l);
    if (index) *index = 0;
    lc_status(l, false, "created the %s project %s in %s (build it to open it)", lc_template_name(kind), name, dir);
    return true;
}
const char *lc_template_source(void) { return "launcher/template/game.c"; }

/* --- actions --- */
static void launch_command(const lc_launcher *l, const char *exe, size_t index, const char *scene, char *out, size_t capacity) {
    out[0] = 0;
    if (index >= l->count) return;
#ifdef _WIN32
    append_str(out, capacity, "start \"\"");
#endif
    quote(out, capacity, exe);
    append_str(out, capacity, " --project");
    quote(out, capacity, l->entries[index].path);
    if (scene && *scene) {
        char path[CW_GAME_PATH_CAPACITY];
        copy_str(path, sizeof(path), l->entries[index].project.scenes_dir);
        append_str(path, sizeof(path), "/");
        append_str(path, sizeof(path), scene);
        append_str(out, capacity, " --scene");
        quote(out, capacity, path);
    }
#ifndef _WIN32
    append_str(out, capacity, " &");
#endif
}
void lc_editor_command(const lc_launcher *l, size_t index, const char *scene, char *out, size_t capacity) {
    launch_command(l, l->editor, index, scene, out, capacity);
}
void lc_player_command(const lc_launcher *l, size_t index, const char *scene, char *out, size_t capacity) {
    launch_command(l, l->player, index, scene, out, capacity);
}
static bool launch(lc_launcher *l, const char *exe, const char *what, size_t index, const char *scene) {
    if (index >= l->count) return false;
    lc_entry *e = &l->entries[index];
    if (!e->loadable) { lc_status(l, true, "%s cannot be opened: %s", e->path, e->error); return false; }
    if (!file_exists(exe)) { lc_status(l, true, "%s not found at %s", what, exe); return false; }
    char cmd[2048];
    launch_command(l, exe, index, scene, cmd, sizeof(cmd));
    int status = system(cmd);
    if (status != 0) { lc_status(l, true, "could not start the %s", what); return false; }
    lc_touch(l, index);
    lc_save(l);
    lc_status(l, false, "started the %s on %s%s%s", what, l->entries[0].project.name, scene ? " / " : "", scene ? scene : "");
    return true;
}
bool lc_open_in_editor(lc_launcher *l, size_t index, const char *scene) { return launch(l, l->editor, "editor", index, scene); }
bool lc_run(lc_launcher *l, size_t index, const char *scene) {
    if (index < l->count && l->entries[index].loadable && !l->entries[index].module_built) {
        lc_status(l, true, "build %s first", l->entries[index].project.name);
        return false;
    }
    return launch(l, l->player, "player", index, scene);
}
bool lc_build(lc_launcher *l, size_t index) {
    if (index >= l->count) return false;
    lc_entry *e = &l->entries[index];
    if (!e->loadable) { lc_status(l, true, "%s cannot be built: %s", e->path, e->error); return false; }
    if (!l->engine_root[0]) { lc_status(l, true, "engine root unknown: start the launcher with --engine <path>"); return false; }
    char cmd[4096] = "", script[CW_GAME_PATH_CAPACITY];
    copy_str(script, sizeof(script), l->engine_root);
#ifdef _WIN32
    append_str(cmd, sizeof(cmd), "powershell -NoProfile -ExecutionPolicy Bypass -File");
    append_str(script, sizeof(script), "\\tools\\build-game.ps1");
#else
    append_str(cmd, sizeof(cmd), "sh");
    append_str(script, sizeof(script), "/tools/build-game.sh");
#endif
    quote(cmd, sizeof(cmd), script);
    quote(cmd, sizeof(cmd), l->engine_root);
    quote(cmd, sizeof(cmd), e->path);
    quote(cmd, sizeof(cmd), "module");
    quote(cmd, sizeof(cmd), e->project.module);
    for (size_t i = 0; i < e->project.source_count; ++i) quote(cmd, sizeof(cmd), e->project.sources[i]);
    append_str(cmd, sizeof(cmd), " 2>&1");
    lc_status(l, false, "building %s...", e->project.name);
    l->log[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) { lc_status(l, true, "cannot run the build script"); return false; }
    size_t len = 0;
    char line[512];
    while (fgets(line, sizeof(line), p)) {
        fputs(line, stdout);
        size_t n = strlen(line);
        if (len + n + 1 < sizeof(l->log)) { memcpy(l->log + len, line, n); len += n; l->log[len] = 0; }
    }
    int status = pclose(p);
#ifndef _WIN32
    if (status != -1) status = (status >> 8) & 0xff;
#endif
    lc_refresh_entry(e);
    if (status != 0) { lc_status(l, true, "build of %s failed (exit %d): see the log", e->project.name, status); return false; }
    lc_status(l, false, "built %s", e->project.name);
    return true;
}
