#ifndef _WIN32
#define _DEFAULT_SOURCE 1
#endif
#define _CRT_SECURE_NO_WARNINGS 1
#include "cw_assets.h"
#include "cw_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <dirent.h>
#include <unistd.h>
#define make_dir(path) mkdir(path, 0755)
#endif

static const cw_assets *current;

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
static void set_error(char *error, size_t capacity, const char *message) {
    if (error && capacity) copy_str(error, capacity, message);
}
static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}
static bool stat_file(const char *path, uint64_t *size, bool *is_dir) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (size) *size = (uint64_t)st.st_size;
    if (is_dir) *is_dir = (st.st_mode & S_IFDIR) != 0;
    return true;
}
static void ensure_dir(const char *path) {
    /* creates every missing component */
    char buffer[CW_ASSET_PATH_CAPACITY];
    copy_str(buffer, sizeof(buffer), path);
    for (char *p = buffer + 1; *p; ++p) {
        if (*p != '/' && *p != '\\') continue;
        char c = *p;
        *p = 0;
        make_dir(buffer);
        *p = c;
    }
    make_dir(buffer);
}

uint32_t cw_asset_id(const char *name) {
    if (!name || !*name) return 0;
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) h = (h ^ *p) * 16777619u;
    return h ? h : 1;
}
static const char *const type_names[CW_ASSET_TYPES] = {"other", "scene", "image", "audio", "font", "data", "config", "source", "prefab", "animation", "material", "shader"};
static const char *const type_dirs[CW_ASSET_TYPES] = {"assets", "scenes", "assets/images", "assets/audio", "assets/fonts", "assets/data", "assets", ".", "prefabs", "animations", "materials", "shaders"};
const char *cw_asset_type_name(cw_asset_type type) { return type >= 0 && type < CW_ASSET_TYPES ? type_names[type] : "?"; }
const char *cw_asset_type_dir(cw_asset_type type) { return type >= 0 && type < CW_ASSET_TYPES ? type_dirs[type] : "assets"; }
static cw_asset_type type_by_name(const char *name) {
    for (int i = 0; i < CW_ASSET_TYPES; ++i)
        if (strcmp(type_names[i], name) == 0) return (cw_asset_type)i;
    return CW_ASSET_OTHER;
}
cw_asset_type cw_asset_type_for_field(const char *name) {
    static const struct { const char *word; cw_asset_type type; } words[] = {
        {"material", CW_ASSET_MATERIAL}, {"sprite", CW_ASSET_IMAGE}, {"image", CW_ASSET_IMAGE}, {"texture", CW_ASSET_IMAGE},
        {"clip", CW_ASSET_ANIMATION}, {"animation", CW_ASSET_ANIMATION}, {"anim", CW_ASSET_ANIMATION}, {"shader", CW_ASSET_SHADER},
        {"sound", CW_ASSET_AUDIO}, {"audio", CW_ASSET_AUDIO}, {"music", CW_ASSET_AUDIO}, {"font", CW_ASSET_FONT},
        {"prefab", CW_ASSET_PREFAB}, {"scene", CW_ASSET_SCENE}, {"level", CW_ASSET_SCENE},
    };
    if (!name) return CW_ASSET_OTHER;
    /* the name or one of its _-separated words: "hit_sound", "sprite", "idle_clip" */
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        size_t n = strlen(words[i].word);
        for (const char *p = name; *p; ++p) {
            if ((p == name || p[-1] == '_') && strncmp(p, words[i].word, n) == 0 && (p[n] == 0 || p[n] == '_')) return words[i].type;
        }
    }
    return CW_ASSET_OTHER;
}
cw_asset_type cw_asset_type_of(const char *filename) {
    const char *dot = filename ? strrchr(base_name(filename), '.') : NULL;
    if (!dot) return CW_ASSET_OTHER;
    char ext[16];
    size_t n = 0;
    for (const char *p = dot + 1; *p && n + 1 < sizeof(ext); ++p) ext[n++] = (char)tolower((unsigned char)*p);
    ext[n] = 0;
    static const struct { const char *ext; cw_asset_type type; } table[] = {
        {"cwscene", CW_ASSET_SCENE}, {"cwprefab", CW_ASSET_PREFAB}, {"cwanim", CW_ASSET_ANIMATION}, {"cwmat", CW_ASSET_MATERIAL}, {"cwshader", CW_ASSET_SHADER}, {"glsl", CW_ASSET_SHADER},
        {"png", CW_ASSET_IMAGE}, {"jpg", CW_ASSET_IMAGE}, {"jpeg", CW_ASSET_IMAGE}, {"bmp", CW_ASSET_IMAGE}, {"tga", CW_ASSET_IMAGE}, {"gif", CW_ASSET_IMAGE}, {"ppm", CW_ASSET_IMAGE}, {"qoi", CW_ASSET_IMAGE},
        {"wav", CW_ASSET_AUDIO}, {"ogg", CW_ASSET_AUDIO}, {"mp3", CW_ASSET_AUDIO}, {"flac", CW_ASSET_AUDIO},
        {"ttf", CW_ASSET_FONT}, {"otf", CW_ASSET_FONT},
        {"json", CW_ASSET_DATA}, {"csv", CW_ASSET_DATA}, {"txt", CW_ASSET_DATA}, {"tsv", CW_ASSET_DATA}, {"xml", CW_ASSET_DATA}, {"cwsn", CW_ASSET_DATA},
        {"cfg", CW_ASSET_CONFIG}, {"ini", CW_ASSET_CONFIG}, {"cwproj", CW_ASSET_CONFIG}, {"cwassets", CW_ASSET_CONFIG},
        {"c", CW_ASSET_SOURCE}, {"h", CW_ASSET_SOURCE},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
        if (strcmp(table[i].ext, ext) == 0) return table[i].type;
    return CW_ASSET_OTHER;
}

void cw_assets_init(cw_assets *a, const char *manifest_path) {
    memset(a, 0, sizeof(*a));
    copy_str(a->path, sizeof(a->path), manifest_path ? manifest_path : "");
    copy_str(a->dir, sizeof(a->dir), a->path);
    char *slash = strrchr(a->dir, '/');
#ifdef _WIN32
    char *bslash = strrchr(a->dir, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    if (slash) slash[1] = 0; else a->dir[0] = 0;
}
void cw_assets_destroy(cw_assets *a) {
    if (current == a) current = NULL;
    free(a->items);
    memset(a, 0, sizeof(*a));
}
void cw_assets_resolve(const cw_assets *a, const char *relative, char *out, size_t capacity) {
    bool absolute = relative && (relative[0] == '/' || (relative[0] && relative[1] == ':'));
    if (absolute || !a) { copy_str(out, capacity, relative ? relative : ""); return; }
    copy_str(out, capacity, a->dir);
    append_str(out, capacity, relative ? relative : "");
}
static cw_asset *find_mut(cw_assets *a, uint32_t id) {
    for (size_t i = 0; i < a->count; ++i)
        if (a->items[i].id == id) return &a->items[i];
    return NULL;
}
const cw_asset *cw_assets_find(const cw_assets *a, uint32_t id) { return a && id ? find_mut((cw_assets *)a, id) : NULL; }
const cw_asset *cw_assets_find_name(const cw_assets *a, const char *name) { return a && name ? cw_assets_find(a, cw_asset_id(name)) : NULL; }
const cw_asset *cw_assets_find_path(const cw_assets *a, const char *relative) {
    if (!a || !relative) return NULL;
    for (size_t i = 0; i < a->count; ++i)
        if (strcmp(a->items[i].path, relative) == 0) return &a->items[i];
    return NULL;
}
bool cw_assets_path(const cw_assets *a, uint32_t id, char *out, size_t capacity) {
    const cw_asset *asset = cw_assets_find(a, id);
    if (!asset) { if (out && capacity) out[0] = 0; return false; }
    cw_assets_resolve(a, asset->path, out, capacity);
    return true;
}
static void refresh_one(const cw_assets *a, cw_asset *asset) {
    char full[CW_ASSET_PATH_CAPACITY];
    cw_assets_resolve(a, asset->path, full, sizeof(full));
    bool dir = false;
    asset->missing = !stat_file(full, &asset->size, &dir) || dir;
    if (asset->missing) asset->size = 0;
}
void cw_assets_refresh(cw_assets *a) {
    for (size_t i = 0; a && i < a->count; ++i) refresh_one(a, &a->items[i]);
}

/* --- manifest --- */
static void write_quoted(cw_writer *out, const char *s) {
    cw_writer_u8(out, '"');
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') cw_writer_u8(out, '\\');
        cw_writer_u8(out, (uint8_t)*s);
    }
    cw_writer_u8(out, '"');
}
bool cw_assets_save(cw_assets *a, char *error, size_t error_capacity) {
    if (!a || !a->path[0]) { set_error(error, error_capacity, "no manifest path"); return false; }
    cw_writer out;
    cw_writer_init(&out);
    char line[64];
    snprintf(line, sizeof(line), "cwassets %d\n", CW_ASSETS_FORMAT_VERSION);
    cw_writer_bytes(&out, line, strlen(line));
    for (size_t i = 0; i < a->count; ++i) {
        const cw_asset *x = &a->items[i];
        snprintf(line, sizeof(line), "asset %08x %s ", x->id, cw_asset_type_name(x->type));
        cw_writer_bytes(&out, line, strlen(line));
        write_quoted(&out, x->path);
        cw_writer_u8(&out, ' ');
        write_quoted(&out, x->name);
        cw_writer_u8(&out, '\n');
    }
    cw_file_error ferr;
    bool ok = cw_file_write(a->path, out.data, out.size, &ferr);
    cw_writer_destroy(&out);
    if (!ok) {
        char message[CW_ASSET_PATH_CAPACITY + 64];
        snprintf(message, sizeof(message), "cannot write %s: %s", a->path, cw_file_status_string(ferr.status));
        set_error(error, error_capacity, message);
        return false;
    }
    a->dirty = false;
    return true;
}
/* reads a "..." token at p; returns the position after it, or NULL */
static const char *read_quoted(const char *p, const char *end, char *out, size_t capacity) {
    while (p < end && isspace((unsigned char)*p)) ++p;
    if (p >= end || *p != '"') return NULL;
    ++p;
    size_t n = 0;
    while (p < end && *p != '"') {
        char c = *p++;
        if (c == '\\' && p < end) c = *p++;
        if (n + 1 < capacity) out[n++] = c;
    }
    out[n] = 0;
    return p < end ? p + 1 : NULL;
}
static cw_asset *push(cw_assets *a) {
    if (a->count == a->capacity) {
        a->capacity = a->capacity ? a->capacity * 2 : 32;
        a->items = cw_resize(a->items, a->capacity, sizeof(*a->items));
    }
    cw_asset *x = &a->items[a->count++];
    memset(x, 0, sizeof(*x));
    return x;
}
bool cw_assets_load(cw_assets *a, char *error, size_t error_capacity) {
    if (!a) return false;
    a->count = 0;
    a->dirty = false;
    cw_buffer text;
    cw_file_error ferr;
    if (!cw_file_read(a->path, 16 << 20, &text, &ferr)) {
        if (ferr.status == CW_FILE_ERROR_OPEN) return true;   /* no manifest yet */
        set_error(error, error_capacity, cw_file_status_string(ferr.status));
        return false;
    }
    const char *p = (const char *)text.data, *end = p + text.size;
    bool header = false, ok = true;
    int line_no = 0;
    while (ok && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p)), *line_end = nl ? nl : end;
        ++line_no;
        const char *line = p;
        p = nl ? nl + 1 : end;
        while (line < line_end && isspace((unsigned char)*line)) ++line;
        if (line == line_end || *line == '#') continue;
        if (!header) {
            if (strncmp(line, "cwassets 1", 10) != 0) { set_error(error, error_capacity, "not an asset manifest"); ok = false; }
            header = true;
            continue;
        }
        if (strncmp(line, "asset ", 6) != 0) { set_error(error, error_capacity, "unknown manifest line"); ok = false; break; }
        unsigned id = 0;
        char type_name[16] = "";
        int consumed = 0;
        if (sscanf(line + 6, "%8x %15s%n", &id, type_name, &consumed) != 2) { set_error(error, error_capacity, "bad asset line"); ok = false; break; }
        cw_asset entry;
        memset(&entry, 0, sizeof(entry));
        const char *q = read_quoted(line + 6 + consumed, line_end, entry.path, sizeof(entry.path));
        q = q ? read_quoted(q, line_end, entry.name, sizeof(entry.name)) : NULL;
        if (!q || !entry.name[0] || !entry.path[0]) { set_error(error, error_capacity, "bad asset line"); ok = false; break; }
        entry.id = cw_asset_id(entry.name);   /* the stored id is informative; the name is the truth */
        entry.type = type_by_name(type_name);
        if (find_mut(a, entry.id)) continue;  /* duplicates are dropped */
        *push(a) = entry;
        (void)id;
    }
    cw_buffer_free(&text);
    if (!ok) { a->count = 0; return false; }
    cw_assets_refresh(a);
    return true;
}

/* --- registering --- */
static void unique_name(const cw_assets *a, const char *wanted, char *out, size_t capacity) {
    copy_str(out, capacity, wanted);
    for (int suffix = 2; cw_assets_find_name(a, out) && suffix < 10000; ++suffix) {
        const char *dot = strrchr(wanted, '.');
        if (dot && dot != wanted) snprintf(out, capacity, "%.*s_%d%s", (int)(dot - wanted), wanted, suffix, dot);
        else snprintf(out, capacity, "%s_%d", wanted, suffix);
    }
}
static void normalise(char *path) {
    for (char *p = path; *p; ++p) if (*p == '\\') *p = '/';
    while (path[0] == '.' && path[1] == '/') memmove(path, path + 2, strlen(path + 2) + 1);
}
const cw_asset *cw_assets_add(cw_assets *a, const char *relative, const char *name, char *error, size_t error_capacity) {
    if (!a || !relative || !*relative) { set_error(error, error_capacity, "no path"); return NULL; }
    char rel[CW_ASSET_PATH_CAPACITY];
    copy_str(rel, sizeof(rel), relative);
    normalise(rel);
    if (cw_assets_find_path(a, rel)) { set_error(error, error_capacity, "already registered"); return cw_assets_find_path(a, rel); }
    char full[CW_ASSET_PATH_CAPACITY];
    cw_assets_resolve(a, rel, full, sizeof(full));
    uint64_t size = 0;
    bool dir = false;
    if (!stat_file(full, &size, &dir) || dir) { set_error(error, error_capacity, "file not found"); return NULL; }
    cw_asset *x = push(a);
    copy_str(x->path, sizeof(x->path), rel);
    unique_name(a, name && *name ? name : base_name(rel), x->name, sizeof(x->name));
    x->id = cw_asset_id(x->name);
    x->type = cw_asset_type_of(rel);
    x->size = size;
    a->dirty = true;
    return x;
}
static bool copy_file(const char *from, const char *to, char *error, size_t error_capacity) {
    cw_buffer data;
    cw_file_error ferr;
    if (!cw_file_read(from, 1u << 30, &data, &ferr)) { set_error(error, error_capacity, "cannot read the source file"); return false; }
    bool ok = cw_file_write(to, data.data, data.size, &ferr);
    cw_buffer_free(&data);
    if (!ok) set_error(error, error_capacity, "cannot write into the project");
    return ok;
}
const cw_asset *cw_assets_import(cw_assets *a, const char *source, const char *subdir, bool replace, char *error, size_t error_capacity) {
    if (!a || !source || !*source) { set_error(error, error_capacity, "no source file"); return NULL; }
    uint64_t size = 0;
    bool dir = false;
    if (!stat_file(source, &size, &dir)) { set_error(error, error_capacity, "source file not found"); return NULL; }
    if (dir) { set_error(error, error_capacity, "the source is a directory"); return NULL; }
    cw_asset_type type = cw_asset_type_of(source);
    char rel[CW_ASSET_PATH_CAPACITY];
    copy_str(rel, sizeof(rel), subdir && *subdir ? subdir : cw_asset_type_dir(type));
    normalise(rel);
    if (strcmp(rel, ".") == 0) rel[0] = 0;
    if (rel[0]) append_str(rel, sizeof(rel), "/");
    append_str(rel, sizeof(rel), base_name(source));
    char full[CW_ASSET_PATH_CAPACITY], dest_dir[CW_ASSET_PATH_CAPACITY];
    cw_assets_resolve(a, rel, full, sizeof(full));
    copy_str(dest_dir, sizeof(dest_dir), full);
    char *slash = strrchr(dest_dir, '/');
    if (slash) { *slash = 0; ensure_dir(dest_dir); }
    bool exists = stat_file(full, NULL, NULL);
    if (exists && !replace) {
        const cw_asset *known = cw_assets_find_path(a, rel);
        if (known) { set_error(error, error_capacity, "already imported (use replace to overwrite)"); return NULL; }
    }
    if (!copy_file(source, full, error, error_capacity)) return NULL;
    const cw_asset *known = cw_assets_find_path(a, rel);
    if (known) { refresh_one(a, (cw_asset *)known); a->dirty = true; return known; }
    return cw_assets_add(a, rel, NULL, error, error_capacity);
}
static size_t scan_dir(cw_assets *a, const char *relative, int depth) {
    if (depth > 8) return 0;
    char full[CW_ASSET_PATH_CAPACITY];
    cw_assets_resolve(a, relative[0] ? relative : ".", full, sizeof(full));
    size_t added = 0;
#ifdef _WIN32
    char pattern[CW_ASSET_PATH_CAPACITY + 4];
    copy_str(pattern, sizeof(pattern), full);
    append_str(pattern, sizeof(pattern), "\\*");
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const char *name = data.cFileName;
        bool is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    DIR *d = opendir(full);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        char child_full[CW_ASSET_PATH_CAPACITY];
        copy_str(child_full, sizeof(child_full), full);
        append_str(child_full, sizeof(child_full), "/");
        append_str(child_full, sizeof(child_full), name);
        bool is_dir = false;
        stat_file(child_full, NULL, &is_dir);
#endif
        if (name[0] == '.' || strcmp(name, "build") == 0 || strcmp(name, "obj") == 0) continue;
        char child[CW_ASSET_PATH_CAPACITY];
        copy_str(child, sizeof(child), relative);
        if (child[0]) append_str(child, sizeof(child), "/");
        append_str(child, sizeof(child), name);
        if (is_dir) added += scan_dir(a, child, depth + 1);
        else if (!cw_assets_find_path(a, child) && cw_asset_type_of(child) != CW_ASSET_CONFIG && cw_assets_add(a, child, NULL, NULL, 0)) ++added;
#ifdef _WIN32
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    }
    closedir(d);
#endif
    return added;
}
size_t cw_assets_scan(cw_assets *a, const char *relative_dir) {
    if (!a) return 0;
    char rel[CW_ASSET_PATH_CAPACITY];
    copy_str(rel, sizeof(rel), relative_dir ? relative_dir : "");
    normalise(rel);
    if (strcmp(rel, ".") == 0) rel[0] = 0;
    return scan_dir(a, rel, 0);
}
bool cw_assets_rename(cw_assets *a, uint32_t id, const char *name) {
    cw_asset *x = a ? find_mut(a, id) : NULL;
    if (!x || !name || !*name || strpbrk(name, "\r\n")) return false;
    if (cw_assets_find_name(a, name) && cw_assets_find_name(a, name) != x) return false;
    copy_str(x->name, sizeof(x->name), name);
    x->id = cw_asset_id(name);
    a->dirty = true;
    return true;
}
bool cw_assets_remove(cw_assets *a, uint32_t id, bool delete_file) {
    cw_asset *x = a ? find_mut(a, id) : NULL;
    if (!x) return false;
    if (delete_file) {
        char full[CW_ASSET_PATH_CAPACITY];
        cw_assets_resolve(a, x->path, full, sizeof(full));
        remove(full);
    }
    size_t i = (size_t)(x - a->items);
    memmove(a->items + i, a->items + i + 1, (a->count - i - 1) * sizeof(*a->items));
    --a->count;
    a->dirty = true;
    return true;
}
void cw_assets_set_current(const cw_assets *a) { current = a; }
const cw_assets *cw_assets_current(void) { return current; }
