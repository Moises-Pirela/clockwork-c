#define _CRT_SECURE_NO_WARNINGS 1
#ifndef _WIN32
#define _DEFAULT_SOURCE 1
#endif
#include "cw_game.h"
#include "cw_internal.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#endif

/* Bounded copy; the sources are lines whose length the compiler cannot
 * bound, so plain snprintf would warn about possible truncation. */
static void copy_str(char *dst, size_t capacity, const char *src) {
    if (!capacity) return;
    size_t n = strlen(src);
    if (n >= capacity) n = capacity - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}
static void append_str(char *dst, size_t capacity, const char *src) {
    size_t n = strlen(dst);
    if (n < capacity) copy_str(dst + n, capacity - n, src);
}

static void set_error(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message ? message : "unknown error");
}

/* --- loading --- */
const char *cw_game_library_suffix(void) {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}
bool cw_game_module_valid(const cw_game_module *m) {
    if (!m || m->abi_version != CW_GAME_ABI_VERSION) return false;
    if (!m->name || !*m->name || strpbrk(m->name, " \t\r\n\"")) return false;
    if (!m->register_components || !m->register_systems) return false;
    if (m->prefab_count && !m->prefabs) return false;
    for (size_t i = 0; i < m->prefab_count; ++i)
        if (!m->prefabs[i].name || !m->prefabs[i].spawn) return false;
    return m->tick_hz >= 0 && m->bounds[2] >= 0 && m->bounds[3] >= 0;
}
bool cw_game_use(cw_game *game, const cw_game_module *module, char *error, size_t error_capacity) {
    if (!game) return false;
    memset(game, 0, sizeof(*game));
    if (!cw_game_module_valid(module)) {
        set_error(error, error_capacity, module && module->abi_version != CW_GAME_ABI_VERSION
                  ? "module was built for a different engine ABI" : "invalid module table");
        return false;
    }
    game->module = module;
    snprintf(game->path, sizeof(game->path), "<static:%s>", module->name);
    return true;
}
#ifdef _WIN32
static void close_library(HMODULE lib, const char *original_path) {
    char loaded_path[CW_GAME_PATH_CAPACITY], original[CW_GAME_PATH_CAPACITY];
    DWORD length = GetModuleFileNameA(lib, loaded_path, sizeof(loaded_path));
    DWORD original_length = GetFullPathNameA(original_path, sizeof(original), original, NULL);
    bool is_copy = length > 0 && length < sizeof(loaded_path)
        && original_length > 0 && original_length < sizeof(original) && _stricmp(loaded_path, original) != 0;
    if (FreeLibrary(lib) && is_copy) DeleteFileA(loaded_path);
}
#endif
bool cw_game_open(cw_game *game, const char *path, char *error, size_t error_capacity) {
    if (!game) return false;
    memset(game, 0, sizeof(*game));
    if (!path || !*path) { set_error(error, error_capacity, "no module path"); return false; }
    cw_game_export_fn export_fn = NULL;
#ifdef _WIN32
    char directory[CW_GAME_PATH_CAPACITY], loaded_path[MAX_PATH];
    char *filename = NULL;
    DWORD length = GetFullPathNameA(path, sizeof(directory), directory, &filename);
    if (!length || length >= sizeof(directory) || !filename) {
        set_error(error, error_capacity, "cannot resolve module path");
        return false;
    }
    *filename = 0;
    bool copied = false;
    if (GetTempFileNameA(directory, "cwg", 0, loaded_path)) {
        copied = CopyFileA(path, loaded_path, FALSE) != 0;
        if (!copied) DeleteFileA(loaded_path);
    }
    HMODULE lib = LoadLibraryA(copied ? loaded_path : path);
    if (!lib) {
        char message[256];
        snprintf(message, sizeof(message), "cannot load %s (error %lu)", path, (unsigned long)GetLastError());
        if (copied) DeleteFileA(loaded_path);
        set_error(error, error_capacity, message);
        return false;
    }
    FARPROC proc = GetProcAddress(lib, CW_GAME_EXPORT_SYMBOL);
    memcpy(&export_fn, &proc, sizeof(export_fn));
    if (!export_fn) {
        close_library(lib, path);
        set_error(error, error_capacity, "the library does not export " CW_GAME_EXPORT_SYMBOL);
        return false;
    }
#else
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        const char *why = dlerror();
        char message[512];
        snprintf(message, sizeof(message), "cannot load %s: %s", path, why ? why : "dlopen failed");
        set_error(error, error_capacity, message);
        return false;
    }
    void *symbol = dlsym(lib, CW_GAME_EXPORT_SYMBOL);
    memcpy(&export_fn, &symbol, sizeof(export_fn));
    if (!export_fn) {
        dlclose(lib);
        set_error(error, error_capacity, "the library does not export " CW_GAME_EXPORT_SYMBOL);
        return false;
    }
#endif
    const cw_game_module *module = export_fn();
    if (!cw_game_module_valid(module)) {
        if (module && module->abi_version != CW_GAME_ABI_VERSION) {
            char message[160];
            snprintf(message, sizeof(message), "module was built for engine ABI %u, this engine is ABI %u: rebuild it", module->abi_version, (unsigned)CW_GAME_ABI_VERSION);
            set_error(error, error_capacity, message);
        } else set_error(error, error_capacity, "the module table is invalid");
#ifdef _WIN32
        close_library(lib, path);
#else
        dlclose(lib);
#endif
        return false;
    }
    game->module = module;
    game->library = lib;
    snprintf(game->path, sizeof(game->path), "%s", path);
    return true;
}
void cw_game_close(cw_game *game) {
    if (!game) return;
    if (game->library) {
#ifdef _WIN32
        close_library((HMODULE)game->library, game->path);
#else
        dlclose(game->library);
#endif
    }
    memset(game, 0, sizeof(*game));
}

/* --- bound worlds --- */
/* The engine's own system: bakes the entities that entered the world. */
static void sys_bake(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    cw_game_world *gw = cw_game_world_of(w);
    if (!gw || !gw->baking) return;
    cw_id_view created = cw_world_created(w);
    for (size_t i = 0; i < created.count; ++i)
        if (cw_world_is_valid(w, created.data[i])) cw_reflect_bake(w, created.data[i]);
}
/* --- the Sprite component --- */
static const cw_field sprite_fields[] = {
    CW_FIELD_SEM(cw_sprite, material, CW_FIELD_U32, CW_SEM_ASSET),
    CW_FIELD_RANGE(cw_sprite, width, CW_FIELD_F32, 1, 4096, 4),
    CW_FIELD_RANGE(cw_sprite, height, CW_FIELD_F32, 1, 4096, 4),
    CW_FIELD_SEM(cw_sprite, rotation, CW_FIELD_F32, CW_SEM_ANGLE),
    CW_FIELD_RANGE(cw_sprite, layer, CW_FIELD_I32, -100, 100, 1),
    CW_FIELD(cw_sprite, flip_x, CW_FIELD_BOOL),
    CW_FIELD(cw_sprite, flip_y, CW_FIELD_BOOL),
    CW_FIELD_SEM(cw_sprite, r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(cw_sprite, g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(cw_sprite, b, CW_FIELD_U8, CW_SEM_COLOR_B),
    CW_FIELD_RANGE(cw_sprite, a, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_RANGE(cw_sprite, columns, CW_FIELD_I32, 1, 64, 1),
    CW_FIELD_RANGE(cw_sprite, rows, CW_FIELD_I32, 1, 64, 1),
    CW_FIELD_RANGE(cw_sprite, frame, CW_FIELD_I32, 0, 4095, 1),
};
static const cw_sprite sprite_defaults = {0, 64, 64, 0, 0, false, false, 255, 255, 255, 255, 1, 1, 0};
const cw_type_info cw_sprite_type = {"Sprite", sizeof(cw_sprite), _Alignof(cw_sprite), sprite_fields, 14, &sprite_defaults, NULL, 0, NULL, NULL};
static void draw_sprites(cw_game_world *gw, const cw_canvas *c) {
    if (!c->sprite || gw->sprite_ct == CW_INVALID_COMPONENT) return;
    cw_world *w = &gw->world;
    cw_query q = cw_query_iter(w, cw_mask(&gw->sprite_ct, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        const cw_sprite *s = cw_comp_ptr(w, id, gw->sprite_ct);
        float x, y;
        if (!cw_reflect_position(w, id, &x, &y)) continue;
        int cols = s->columns > 0 ? s->columns : 1, rows = s->rows > 0 ? s->rows : 1;
        int frame = s->frame % (cols * rows);
        if (frame < 0) frame += cols * rows;
        float fw = 1.0f / (float)cols, fh = 1.0f / (float)rows;
        float u0 = (float)(frame % cols) * fw, v0 = (float)(frame / cols) * fh, u1 = u0 + fw, v1 = v0 + fh;
        if (s->flip_x) { float t = u0; u0 = u1; u1 = t; }
        if (s->flip_y) { float t = v0; v0 = v1; v1 = t; }
        c->sprite(c->impl, s->material, x, y, s->width, s->height, s->rotation, cw_rgba_make(s->r, s->g, s->b, s->a), s->layer, u0, v0, u1, v1);
        if (c->pick) c->pick(c->impl, id, x, y, (s->width > s->height ? s->width : s->height) * 0.5f);
    }
}

/* --- the Shape3D component --- */
const char *const cw_shape3d_kind_names[CW_SHAPE_KINDS] = {"sphere", "capsule", "cube", "plane"};
static const cw_field shape3d_fields[] = {
    CW_FIELD_NAMED(cw_shape3d, kind, CW_FIELD_ENUM, cw_shape3d_kind_names, CW_SHAPE_KINDS),
    CW_FIELD_RANGE_SEM(cw_shape3d, width, CW_FIELD_F32, 0.01f, 10000, 0.1f, CW_SEM_SIZE_X),
    CW_FIELD_RANGE_SEM(cw_shape3d, height, CW_FIELD_F32, 0, 10000, 0.1f, CW_SEM_SIZE_Y),
    CW_FIELD_RANGE_SEM(cw_shape3d, depth, CW_FIELD_F32, 0.01f, 10000, 0.1f, CW_SEM_SIZE_Z),
    CW_FIELD_SEM(cw_shape3d, rx, CW_FIELD_F32, CW_SEM_ROTATION_X),
    CW_FIELD_SEM(cw_shape3d, ry, CW_FIELD_F32, CW_SEM_ROTATION_Y),
    CW_FIELD_SEM(cw_shape3d, rz, CW_FIELD_F32, CW_SEM_ROTATION_Z),
    CW_FIELD_SEM(cw_shape3d, r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(cw_shape3d, g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(cw_shape3d, b, CW_FIELD_U8, CW_SEM_COLOR_B),
    CW_FIELD_RANGE(cw_shape3d, a, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_RANGE(cw_shape3d, checker, CW_FIELD_F32, 0, 100, 0.5f),
    CW_FIELD(cw_shape3d, wireframe, CW_FIELD_BOOL),
    CW_FIELD_SEM(cw_shape3d, material, CW_FIELD_U32, CW_SEM_ASSET),
};
static const cw_shape3d shape3d_defaults = {CW_SHAPE_CUBE, 1, 1, 1, 0, 0, 0, 200, 200, 210, 255, 0, false, 0};
const cw_type_info cw_shape3d_type = {"Shape3D", sizeof(cw_shape3d), _Alignof(cw_shape3d), shape3d_fields, 14, &shape3d_defaults, NULL, 0, NULL, NULL};
void cw_shape3d_to_canvas(const cw_shape3d *s, float x, float y, float z, cw_entity_id id, cw_canvas_shape3d *out) {
    memset(out, 0, sizeof(*out));
    out->kind = s->kind < CW_SHAPE_KINDS ? (cw_shape3d_kind)s->kind : CW_SHAPE_CUBE;
    out->x = x; out->y = y; out->z = z;
    out->rx = s->rx; out->ry = s->ry; out->rz = s->rz;
    out->sx = s->width; out->sy = s->height; out->sz = s->depth;
    out->color = cw_rgba_make(s->r, s->g, s->b, s->a);
    out->checker = s->checker;
    out->wireframe = s->wireframe;
    out->material = s->material;
    out->id = id;
}
/* --- the Light3D component --- */
const char *const cw_light3d_kind_names[CW_LIGHT_KINDS] = {"ambient", "directional", "point", "spot"};
static const cw_field light3d_fields[] = {
    CW_FIELD_NAMED(cw_light3d, kind, CW_FIELD_ENUM, cw_light3d_kind_names, CW_LIGHT_KINDS),
    CW_FIELD_SEM(cw_light3d, r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(cw_light3d, g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(cw_light3d, b, CW_FIELD_U8, CW_SEM_COLOR_B),
    CW_FIELD_RANGE(cw_light3d, intensity, CW_FIELD_F32, 0, 100, 0.1f),
    CW_FIELD_RANGE(cw_light3d, range, CW_FIELD_F32, 0.01f, 10000, 0.5f),
    CW_FIELD_SEM(cw_light3d, pitch, CW_FIELD_F32, CW_SEM_ANGLE),
    CW_FIELD_SEM(cw_light3d, yaw, CW_FIELD_F32, CW_SEM_ANGLE),
    CW_FIELD_RANGE_SEM(cw_light3d, cone, CW_FIELD_F32, 0.01f, 3.1f, 0.0872665f, CW_SEM_ANGLE),
    CW_FIELD(cw_light3d, shadows, CW_FIELD_BOOL),
};
static const cw_light3d light3d_defaults = {CW_LIGHT_POINT, 255, 244, 220, 1, 10, -0.9f, 0.6f, 0.8f, true};
const cw_type_info cw_light3d_type = {"Light3D", sizeof(cw_light3d), _Alignof(cw_light3d), light3d_fields, 10, &light3d_defaults, NULL, 0, NULL, NULL};
void cw_light3d_to_canvas(const cw_light3d *l, float x, float y, float z, cw_entity_id id, cw_canvas_light3d *out) {
    memset(out, 0, sizeof(*out));
    out->kind = l->kind < CW_LIGHT_KINDS ? (cw_light3d_kind)l->kind : CW_LIGHT_POINT;
    out->x = x; out->y = y; out->z = z;
    /* pitch 0 is level, negative points down; yaw 0 points along -z, turning towards +x */
    float cp = cosf(l->pitch);
    out->dx = sinf(l->yaw) * cp;
    out->dy = sinf(l->pitch);
    out->dz = -cosf(l->yaw) * cp;
    out->color = cw_rgba_make(l->r, l->g, l->b, 255);
    out->intensity = l->intensity;
    out->range = l->range;
    out->cone = l->cone;
    out->shadows = l->shadows;
    out->id = id;
}
static const cw_field sky3d_fields[] = {
    CW_FIELD_RANGE(cw_sky3d, zenith_r, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_RANGE(cw_sky3d, zenith_g, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_RANGE(cw_sky3d, zenith_b, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_RANGE(cw_sky3d, horizon_r, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_RANGE(cw_sky3d, horizon_g, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_RANGE(cw_sky3d, horizon_b, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_RANGE(cw_sky3d, ground_r, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_RANGE(cw_sky3d, ground_g, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_RANGE(cw_sky3d, ground_b, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_RANGE(cw_sky3d, sun_size, CW_FIELD_F32, 0, 1, 0.005f),
    CW_FIELD_RANGE(cw_sky3d, sun_intensity, CW_FIELD_F32, 0, 20, 0.1f),
    CW_FIELD_SEM(cw_sky3d, image_east, CW_FIELD_U32, CW_SEM_ASSET),
    CW_FIELD_SEM(cw_sky3d, image_west, CW_FIELD_U32, CW_SEM_ASSET),
    CW_FIELD_SEM(cw_sky3d, image_up, CW_FIELD_U32, CW_SEM_ASSET),
    CW_FIELD_SEM(cw_sky3d, image_down, CW_FIELD_U32, CW_SEM_ASSET),
    CW_FIELD_SEM(cw_sky3d, image_south, CW_FIELD_U32, CW_SEM_ASSET),
    CW_FIELD_SEM(cw_sky3d, image_north, CW_FIELD_U32, CW_SEM_ASSET),
};
static const cw_sky3d sky3d_defaults = {70, 110, 190, 190, 210, 235, 60, 58, 62, 0.03f, 3, 0, 0, 0, 0, 0, 0};
const cw_type_info cw_sky3d_type = {"Sky3D", sizeof(cw_sky3d), _Alignof(cw_sky3d), sky3d_fields, 17, &sky3d_defaults, NULL, 0, NULL, NULL};
void cw_sky3d_to_canvas(const cw_sky3d *s, cw_entity_id id, cw_canvas_sky3d *out) {
    memset(out, 0, sizeof(*out));
    out->zenith = cw_rgba_make(s->zenith_r, s->zenith_g, s->zenith_b, 255);
    out->horizon = cw_rgba_make(s->horizon_r, s->horizon_g, s->horizon_b, 255);
    out->ground = cw_rgba_make(s->ground_r, s->ground_g, s->ground_b, 255);
    out->sun_size = s->sun_size;
    out->sun_intensity = s->sun_intensity;
    uint32_t images[6] = {s->image_east, s->image_west, s->image_up, s->image_down, s->image_south, s->image_north};
    bool all = true;
    for (int i = 0; i < 6; ++i) if (!images[i]) all = false;
    if (all) memcpy(out->images, images, sizeof(images));   /* a partial set falls back to the gradient */
    out->id = id;
}
static const cw_field decal3d_fields[] = {
    CW_FIELD_RANGE_SEM(cw_decal3d, width, CW_FIELD_F32, 0.01f, 10000, 0.1f, CW_SEM_SIZE_X),
    CW_FIELD_RANGE_SEM(cw_decal3d, height, CW_FIELD_F32, 0.01f, 10000, 0.1f, CW_SEM_SIZE_Z),
    CW_FIELD_RANGE_SEM(cw_decal3d, depth, CW_FIELD_F32, 0.01f, 10000, 0.1f, CW_SEM_SIZE_Y),
    CW_FIELD_SEM(cw_decal3d, rx, CW_FIELD_F32, CW_SEM_ROTATION_X),
    CW_FIELD_SEM(cw_decal3d, ry, CW_FIELD_F32, CW_SEM_ROTATION_Y),
    CW_FIELD_SEM(cw_decal3d, rz, CW_FIELD_F32, CW_SEM_ROTATION_Z),
    CW_FIELD_SEM(cw_decal3d, r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(cw_decal3d, g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(cw_decal3d, b, CW_FIELD_U8, CW_SEM_COLOR_B),
    CW_FIELD_RANGE(cw_decal3d, a, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_SEM(cw_decal3d, material, CW_FIELD_U32, CW_SEM_ASSET),
    CW_FIELD_SEM(cw_decal3d, max_angle, CW_FIELD_F32, CW_SEM_ANGLE),
};
static const cw_decal3d decal3d_defaults = {1, 1, 0.5f, 0, 0, 0, 255, 255, 255, 255, 0, 1.3962634f};   /* 80 degrees */
const cw_type_info cw_decal3d_type = {"Decal3D", sizeof(cw_decal3d), _Alignof(cw_decal3d), decal3d_fields, 12, &decal3d_defaults, NULL, 0, NULL, NULL};
void cw_decal3d_to_canvas(const cw_decal3d *d, float x, float y, float z, cw_entity_id id, cw_canvas_decal3d *out) {
    memset(out, 0, sizeof(*out));
    out->x = x; out->y = y; out->z = z;
    out->rx = d->rx; out->ry = d->ry; out->rz = d->rz;
    out->sx = d->width; out->sy = d->depth; out->sz = d->height;
    out->color = cw_rgba_make(d->r, d->g, d->b, d->a);
    out->material = d->material;
    out->max_angle = d->max_angle;
    out->id = id;
}
static void draw_decals3d(cw_game_world *gw, const cw_canvas *c) {
    if (!c->decal3d || gw->decal3d_ct == CW_INVALID_COMPONENT) return;
    cw_world *w = &gw->world;
    cw_query q = cw_query_iter(w, cw_mask(&gw->decal3d_ct, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        float x = 0, y = 0, z = 0;
        cw_reflect_position3(w, id, &x, &y, &z);
        cw_canvas_decal3d decal;
        cw_decal3d_to_canvas(cw_comp_ptr(w, id, gw->decal3d_ct), x, y, z, id, &decal);
        c->decal3d(c->impl, &decal);
    }
}
static void draw_sky3d(cw_game_world *gw, const cw_canvas *c) {
    if (!c->sky3d || gw->sky3d_ct == CW_INVALID_COMPONENT) return;
    cw_world *w = &gw->world;
    cw_query q = cw_query_iter(w, cw_mask(&gw->sky3d_ct, 1));
    cw_entity_id id;
    if (!cw_query_next(&q, &id)) return;
    cw_canvas_sky3d sky;
    cw_sky3d_to_canvas(cw_comp_ptr(w, id, gw->sky3d_ct), id, &sky);
    c->sky3d(c->impl, &sky);
}
static void draw_lights3d(cw_game_world *gw, const cw_canvas *c) {
    if (!c->light3d || gw->light3d_ct == CW_INVALID_COMPONENT) return;
    cw_world *w = &gw->world;
    cw_query q = cw_query_iter(w, cw_mask(&gw->light3d_ct, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        float x = 0, y = 0, z = 0;
        cw_reflect_position3(w, id, &x, &y, &z);   /* ambient and directional lights need no position */
        cw_canvas_light3d light;
        cw_light3d_to_canvas(cw_comp_ptr(w, id, gw->light3d_ct), x, y, z, id, &light);
        c->light3d(c->impl, &light);
    }
}
static void draw_brushes(cw_game_world *gw, const cw_canvas *c) {
    if (!c->brush3d || gw->brush_ct == CW_INVALID_COMPONENT) return;
    cw_world *w = &gw->world;
    cw_query q = cw_query_iter(w, cw_mask(&gw->brush_ct, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        cw_canvas_brush b;
        if (!cw_reflect_position3(w, id, &b.x, &b.y, &b.z)) continue;
        b.brush = cw_comp_ptr(w, id, gw->brush_ct);
        b.id = id;
        c->brush3d(c->impl, &b);
    }
}
static void draw_shapes3d(cw_game_world *gw, const cw_canvas *c) {
    if (!c->shape3d || gw->shape3d_ct == CW_INVALID_COMPONENT) return;
    cw_world *w = &gw->world;
    cw_query q = cw_query_iter(w, cw_mask(&gw->shape3d_ct, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        float x, y, z;
        if (!cw_reflect_position3(w, id, &x, &y, &z)) continue;
        cw_canvas_shape3d shape;
        cw_shape3d_to_canvas(cw_comp_ptr(w, id, gw->shape3d_ct), x, y, z, id, &shape);
        c->shape3d(c->impl, &shape);
    }
}

/* The interface: laid out on the screen the host gave, drawn element by element through the hud ops. */
static void draw_ui(cw_game_world *gw, const cw_canvas *c, const cw_draw_context *ctx) {
    if (!cw_ui_ready(&gw->ui)) return;
    cw_game_world_ui_layout(gw);
    if (!c->hud_image && !c->hud_text) return;
    cw_world *w = &gw->world;
    cw_entity_id ids[4096];
    size_t n = cw_ui_collect(w, &gw->ui, ids, 4096);
    for (size_t i = 0; i < n; ++i) {
        cw_entity_id id = ids[i];
        if (!cw_comp_has(w, id, gw->ui.layout)) continue;
        const cw_ui_layout *l = cw_comp_ptr(w, id, gw->ui.layout);
        if (!l->shown || l->w <= 0 || l->h <= 0) continue;
        const cw_ui_button *b = cw_comp_has(w, id, gw->ui.button) ? cw_comp_ptr(w, id, gw->ui.button) : NULL;
        if (cw_comp_has(w, id, gw->ui.image) && c->hud_image) {
            const cw_ui_image *im = cw_comp_ptr(w, id, gw->ui.image);
            cw_rgba tint = cw_rgba_make(im->r, im->g, im->b, im->a);
            if (b) {
                if (!b->enabled) tint.a = (uint8_t)(tint.a / 2);
                else if (l->pressed && l->hovered) tint = cw_rgba_make((uint8_t)(im->r * b->press_r / 255), (uint8_t)(im->g * b->press_g / 255), (uint8_t)(im->b * b->press_b / 255), im->a);
                else if (l->hovered) tint = cw_rgba_make((uint8_t)(im->r * b->hover_r / 255), (uint8_t)(im->g * b->hover_g / 255), (uint8_t)(im->b * b->hover_b / 255), im->a);
            }
            float x = l->x, y = l->y, w2 = l->w, h2 = l->h, u0 = 0, v0 = 0, u1 = 1, v1 = 1;
            float fill = im->fill < 0 ? 0 : (im->fill > 1 ? 1 : im->fill);
            if (im->fill_mode == CW_UI_FILL_HORIZONTAL) { w2 = l->w * fill; u1 = fill; }
            else if (im->fill_mode == CW_UI_FILL_VERTICAL) { h2 = l->h * fill; y = l->y + l->h - h2; v0 = 1 - fill; }
            cw_canvas_dynamic dyn = {{l->hover_t, l->press_t, fill, l->since}, {im->param_x, im->param_y, im->param_z, im->param_w}};
            if (w2 > 0 && h2 > 0) c->hud_image(c->impl, im->material, x, y, w2, h2, tint, im->border, l->scale, u0, v0, u1, v1, &dyn);
        }
        if (cw_comp_has(w, id, gw->ui.text) && c->hud_text && c->hud_text_width) {
            const cw_ui_text *t = cw_comp_ptr(w, id, gw->ui.text);
            if (!t->text[0]) continue;
            float scale = t->size * l->scale, tw = c->hud_text_width(c->impl, scale, t->text), th = 8 * scale;
            float x = l->x, y = l->y;
            if (t->align == CW_UI_ALIGN_CENTER) x = l->x + (l->w - tw) * 0.5f;
            else if (t->align == CW_UI_ALIGN_RIGHT) x = l->x + l->w - tw;
            if (t->valign == CW_UI_VALIGN_MIDDLE) y = l->y + (l->h - th) * 0.5f;
            else if (t->valign == CW_UI_VALIGN_BOTTOM) y = l->y + l->h - th;
            cw_rgba color = cw_rgba_make(t->r, t->g, t->b, t->a);
            if (b && !b->enabled) color.a = (uint8_t)(color.a / 2);
            c->hud_text(c->impl, x, y, scale, color, t->text);
        }
    }
    (void)ctx;
}
/* The engine's interface system: buttons react to the mouse the host fed since the last tick. */
static void sys_ui(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    cw_game_world *gw = cw_game_world_of(w);
    if (!gw || !cw_ui_ready(&gw->ui)) return;
    if (!gw->ui_laid_out) cw_game_world_ui_layout(gw);
    cw_ui_update(w, &gw->ui, &gw->ui_input, dt);
    gw->ui_input.pressed = gw->ui_input.released = false;   /* edges consumed */
}
/* The engine's animation system: every Animation component advances and applies its clip. */
static void sys_anim(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    cw_game_world *gw = cw_game_world_of(w);
    if (!gw || gw->animation_ct == CW_INVALID_COMPONENT) return;
    cw_query q = cw_query_iter(w, cw_mask(&gw->animation_ct, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) cw_anim_step(w, id, gw->animation_ct, &gw->anims, gw->assets, dt);
}
static bool bind(cw_game_world *gw, char *error, size_t error_capacity) {
    const cw_game_module *m = gw->module;
    cw_world_init(&gw->world, NULL);
    gw->world.host = gw;
    cw_sim_init(&gw->sim, CW_HOST);
    if (!m->register_components(&gw->world)) {
        set_error(error, error_capacity, "the module's component registration failed");
        goto fail;
    }
    if (cw_reflect_has_bakes(&gw->world)) {
        const cw_system_desc bake = {"cw_bake", CW_PHASE(CW_ENTITY_CREATED), sys_bake, CW_SHARED, -1000, true};
        if (!cw_sim_register(&gw->sim, bake)) { set_error(error, error_capacity, "cannot register the bake system"); goto fail; }
    }
    /* the engine's Animation component and system, after the module's block */
    gw->animation_ct = cw_comp_block(&gw->world, 1);
    if (gw->animation_ct == CW_INVALID_COMPONENT || !cw_reflect_register(&gw->world, gw->animation_ct, &cw_animation_type)) {
        set_error(error, error_capacity, "cannot register the Animation component");
        goto fail;
    }
    {
        const cw_system_desc anim = {"cw_anim", CW_PHASE(CW_UPDATE), sys_anim, CW_SHARED, -500, false};
        if (!cw_sim_register(&gw->sim, anim)) { set_error(error, error_capacity, "cannot register the animation system"); goto fail; }
    }
    gw->sprite_ct = cw_comp_block(&gw->world, 1);
    if (gw->sprite_ct == CW_INVALID_COMPONENT || !cw_reflect_register(&gw->world, gw->sprite_ct, &cw_sprite_type)) {
        set_error(error, error_capacity, "cannot register the Sprite component");
        goto fail;
    }
    gw->shape3d_ct = cw_comp_block(&gw->world, 1);
    if (gw->shape3d_ct == CW_INVALID_COMPONENT || !cw_reflect_register(&gw->world, gw->shape3d_ct, &cw_shape3d_type)) {
        set_error(error, error_capacity, "cannot register the Shape3D component");
        goto fail;
    }
    gw->light3d_ct = cw_comp_block(&gw->world, 1);
    if (gw->light3d_ct == CW_INVALID_COMPONENT || !cw_reflect_register(&gw->world, gw->light3d_ct, &cw_light3d_type)) {
        set_error(error, error_capacity, "cannot register the Light3D component");
        goto fail;
    }
    gw->brush_ct = cw_comp_block(&gw->world, 1);
    if (gw->brush_ct == CW_INVALID_COMPONENT || !cw_reflect_register(&gw->world, gw->brush_ct, &cw_brush_type)) {
        set_error(error, error_capacity, "cannot register the Brush component");
        goto fail;
    }
    gw->sky3d_ct = cw_comp_block(&gw->world, 1);
    if (gw->sky3d_ct == CW_INVALID_COMPONENT || !cw_reflect_register(&gw->world, gw->sky3d_ct, &cw_sky3d_type)) {
        set_error(error, error_capacity, "cannot register the Sky3D component");
        goto fail;
    }
    gw->decal3d_ct = cw_comp_block(&gw->world, 1);
    if (gw->decal3d_ct == CW_INVALID_COMPONENT || !cw_reflect_register(&gw->world, gw->decal3d_ct, &cw_decal3d_type)) {
        set_error(error, error_capacity, "cannot register the Decal3D component");
        goto fail;
    }
    if (!cw_ui_register(&gw->world, &gw->ui)) {
        set_error(error, error_capacity, "cannot register the Ui components");
        goto fail;
    }
    {
        const cw_system_desc ui = {"cw_ui", CW_PHASE(CW_UPDATE), sys_ui, CW_SHARED, -600, false};
        if (!cw_sim_register(&gw->sim, ui)) { set_error(error, error_capacity, "cannot register the interface system"); goto fail; }
    }
    if (!m->register_systems(&gw->sim)) {
        set_error(error, error_capacity, "the module's system registration failed");
        goto fail;
    }
    if (m->bind && !m->bind(&gw->world)) {
        set_error(error, error_capacity, "the module refused to bind the world");
        goto fail;
    }
    gw->bound = true;
    return true;
fail:
    cw_sim_destroy(&gw->sim);
    cw_world_destroy(&gw->world);
    return false;
}
static void unbind(cw_game_world *gw) {
    if (!gw->bound) return;
    if (gw->module->unbind) gw->module->unbind(&gw->world);
    cw_sim_destroy(&gw->sim);
    cw_world_destroy(&gw->world);
    gw->bound = false;
}
bool cw_game_world_init(cw_game_world *gw, const cw_game_module *module, char *error, size_t error_capacity) {
    if (!gw) return false;
    memset(gw, 0, sizeof(*gw));
    if (!cw_game_module_valid(module)) { set_error(error, error_capacity, "invalid module"); return false; }
    gw->module = module;
    gw->baking = true;
    gw->animation_ct = gw->sprite_ct = gw->shape3d_ct = gw->light3d_ct = gw->brush_ct = gw->sky3d_ct = gw->decal3d_ct = CW_INVALID_COMPONENT;
    gw->ui.rect = gw->ui.image = gw->ui.text = gw->ui.button = gw->ui.layout = CW_INVALID_COMPONENT;
    gw->ui_canvas = cw_ui_canvas_default();
    gw->ui_screen.x = gw->ui_screen.y = 0;
    gw->ui_screen.w = gw->ui_canvas.width;
    gw->ui_screen.h = gw->ui_canvas.height;
    cw_anim_cache_init(&gw->anims);
    gw->clock = cw_clock_make(module->tick_hz > 0 ? module->tick_hz : CW_DEFAULT_TICK_HZ);
    return bind(gw, error, error_capacity);
}
void cw_game_world_destroy(cw_game_world *gw) {
    if (!gw) return;
    unbind(gw);
    cw_anim_cache_destroy(&gw->anims);
    memset(gw, 0, sizeof(*gw));
}
void cw_game_world_reload_animations(cw_game_world *gw) { if (gw) cw_anim_cache_clear(&gw->anims); }
void cw_game_world_clear(cw_game_world *gw) {
    if (!gw || !gw->bound) return;
    /* The destroy phase sees the entities go, then the module state restarts. */
    cw_world_reset(&gw->world);
    if (gw->module->unbind) gw->module->unbind(&gw->world);
    if (gw->module->bind) gw->module->bind(&gw->world);
    cw_clock_reset(&gw->clock);
    gw->ui_laid_out = false;
}
void cw_game_world_settle(cw_game_world *gw) {
    if (!gw || !gw->bound) return;
    if (gw->world.pending_destroy.count || gw->world.created.count) cw_sim_entity_lifecycle(&gw->sim, &gw->world, 0);
}
void cw_game_world_tick(cw_game_world *gw, int steps) {
    if (!gw || !gw->bound) return;
    for (int i = 0; i < steps; ++i) cw_sim_tick(&gw->sim, &gw->world, gw->clock.dt);
}
int cw_game_world_advance(cw_game_world *gw, float frame_dt) {
    if (!gw || !gw->bound) return 0;
    int steps = cw_clock_advance(&gw->clock, frame_dt);
    cw_game_world_tick(gw, steps);
    return steps;
}
float cw_game_world_alpha(const cw_game_world *gw) {
    return gw && gw->bound ? cw_clock_alpha(&gw->clock) : 1.0f;
}
cw_entity_id cw_game_world_spawn(cw_game_world *gw, size_t prefab, float x, float y) {
    if (!gw || !gw->bound || prefab >= gw->module->prefab_count) return CW_INVALID_ENTITY;
    cw_entity_id id = cw_world_create_entity(&gw->world);
    if (!gw->module->prefabs[prefab].spawn(&gw->world, id, x, y)) {
        cw_world_mark_destroy(&gw->world, id);
        cw_world_sweep(&gw->world);
        return CW_INVALID_ENTITY;
    }
    return id;
}
bool cw_game_world_save_scene(cw_game_world *gw, const char *path, const cw_scene_save_config *config,
                              cw_scene_result *result) {
    if (!gw || !gw->bound) return false;
    cw_scene_save_config cfg = {0};
    if (config) cfg = *config;
    if (!cfg.game) { cfg.game = gw->module->name; cfg.scene_version = gw->module->scene_version; }
    return cw_scene_save_file(&gw->world, path, &cfg, result);
}
bool cw_game_world_read_scene(cw_game_world *gw, const char *text, size_t size, const cw_scene_load_config *config,
                              cw_scene_result *result) {
    if (!gw || !gw->bound) return false;
    cw_scene_load_config cfg = {0};
    if (config) cfg = *config;
    if (!cfg.game) cfg.game = gw->module->name;
    cw_game_world_clear(gw);
    return cw_scene_read(&gw->world, text, size, &cfg, result);
}
bool cw_game_world_load_scene(cw_game_world *gw, const char *path, const cw_scene_load_config *config,
                              cw_scene_result *result) {
    if (!gw || !gw->bound) return false;
    cw_scene_load_config cfg = {0};
    if (config) cfg = *config;
    if (!cfg.game) cfg.game = gw->module->name;
    cw_game_world_clear(gw);
    return cw_scene_load_file(&gw->world, path, &cfg, result);
}
void cw_game_world_draw(cw_game_world *gw, const cw_canvas *canvas, const cw_draw_context *ctx) {
    if (!gw || !gw->bound || !canvas) return;
    cw_draw_context c = {1, 0, false, false};
    if (ctx) c = *ctx;
    if (gw->module->draw) gw->module->draw(&gw->world, canvas, &c);
    draw_sprites(gw, canvas);   /* the engine's sprites, for every game */
    draw_sky3d(gw, canvas);     /* its sky, 3D lights, brushes and primitives, when the host has a 3D view */
    draw_lights3d(gw, canvas);
    draw_brushes(gw, canvas);
    draw_shapes3d(gw, canvas);
    draw_decals3d(gw, canvas);  /* after the surfaces they project onto */
    draw_ui(gw, canvas, &c);    /* and its interface */
}
void cw_game_world_set_ui_canvas(cw_game_world *gw, const cw_ui_canvas *canvas) {
    if (!gw) return;
    gw->ui_canvas = canvas ? *canvas : cw_ui_canvas_default();
}
void cw_game_world_set_ui_screen(cw_game_world *gw, cw_ui_screen screen) { if (gw) gw->ui_screen = screen; }
void cw_game_world_ui_input(cw_game_world *gw, float x, float y, bool down, bool pressed, bool released) {
    if (!gw) return;
    gw->ui_input.x = x;
    gw->ui_input.y = y;
    gw->ui_input.down = down;
    gw->ui_input.pressed = gw->ui_input.pressed || pressed;
    gw->ui_input.released = gw->ui_input.released || released;
}
size_t cw_game_world_ui_layout(cw_game_world *gw) {
    if (!gw || !gw->bound || !cw_ui_ready(&gw->ui)) return 0;
    gw->ui_laid_out = true;
    return cw_ui_layout_all(&gw->world, &gw->ui, &gw->ui_canvas, gw->ui_screen);
}

void cw_game_world_set_assets(cw_game_world *gw, const cw_assets *assets) {
    if (!gw) return;
    gw->assets = assets;
    cw_assets_set_current(assets);
}
void cw_game_world_set_baking(cw_game_world *gw, bool baking) { if (gw) gw->baking = baking; }
size_t cw_game_world_bake_all(cw_game_world *gw) {
    if (!gw || !gw->bound) return 0;
    size_t made = 0;
    cw_world *w = &gw->world;
    for (size_t i = 0; i < w->capacity; ++i)
        if (cw_query_visible(w->entities[i])) made += cw_reflect_bake(w, (cw_entity_id)i);
    return made;
}

/* --- projects --- */
const char *cw_renderer_name(cw_renderer_kind kind) { return kind == CW_RENDERER_3D ? "3d" : "2d"; }
bool cw_renderer_parse(const char *text, cw_renderer_kind *out) {
    if (!text) return false;
    if (strcmp(text, "2d") == 0) { if (out) *out = CW_RENDERER_2D; return true; }
    if (strcmp(text, "3d") == 0) { if (out) *out = CW_RENDERER_3D; return true; }
    return false;
}
static bool is_absolute(const char *p) {
#ifdef _WIN32
    return (p[0] && p[1] == ':') || p[0] == '\\' || p[0] == '/';
#else
    return p[0] == '/';
#endif
}
void cw_project_path(const cw_project *project, const char *relative, char *out, size_t capacity) {
    if (!out || !capacity) return;
    if (!relative || !*relative) { out[0] = 0; return; }
    if (is_absolute(relative) || !project) snprintf(out, capacity, "%s", relative);
    else snprintf(out, capacity, "%s%s", project->dir, relative);
}
static void make_absolute(const char *path, char *out, size_t capacity) {
#ifdef _WIN32
    char full[CW_GAME_PATH_CAPACITY];
    DWORD n = GetFullPathNameA(path, sizeof(full), full, NULL);
    snprintf(out, capacity, "%s", n && n < sizeof(full) ? full : path);
#else
    char full[PATH_MAX];
    if (is_absolute(path) || !getcwd(full, sizeof(full))) { copy_str(out, capacity, path); return; }
    copy_str(out, capacity, full);
    append_str(out, capacity, "/");
    append_str(out, capacity, path);
#endif
}
bool cw_project_load(cw_project *project, const char *path, char *error, size_t error_capacity) {
    if (!project) return false;
    memset(project, 0, sizeof(*project));
    if (!path || !*path) { set_error(error, error_capacity, "no project path"); return false; }
    cw_buffer image;
    cw_file_error ferr;
    if (!cw_file_read(path, 1 << 20, &image, &ferr)) {
        char message[CW_GAME_PATH_CAPACITY + 96];
        snprintf(message, sizeof(message), "cannot read %s: %s", path, cw_file_status_string(ferr.status));
        set_error(error, error_capacity, message);
        return false;
    }
    make_absolute(path, project->path, sizeof(project->path));
    snprintf(project->dir, sizeof(project->dir), "%s", project->path);
    char *slash = strrchr(project->dir, '/');
#ifdef _WIN32
    char *bslash = strrchr(project->dir, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    if (slash) slash[1] = 0; else project->dir[0] = 0;

    bool header = false, ok = true;
    copy_str(project->scenes_rel, sizeof(project->scenes_rel), "scenes");
    copy_str(project->render_rel, sizeof(project->render_rel), "render.cfg");
    copy_str(project->assets_rel, sizeof(project->assets_rel), "assets.cwassets");
    project->window_width = 1280;
    project->window_height = 800;
    project->ui = cw_ui_canvas_default();
    project->renderer = CW_RENDERER_2D;
    const char *p = (const char *)image.data, *end = p + image.size;
    int line_no = 0;
    while (ok && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        ++line_no;
        char line[CW_GAME_PATH_CAPACITY + 32];
        if (len >= sizeof(line)) { set_error(error, error_capacity, "project line too long"); ok = false; break; }
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (nl ? 1 : 0);
        char *s = line;
        while (*s && isspace((unsigned char)*s)) ++s;
        char *e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
        if (!*s || *s == '#') continue;
        char *value = s;
        while (*value && !isspace((unsigned char)*value)) ++value;
        if (*value) { *value++ = 0; while (*value && isspace((unsigned char)*value)) ++value; }
        if (!header) {
            if (strcmp(s, "cwproject") != 0 || strcmp(value, "1") != 0) { set_error(error, error_capacity, "not a project file (expected 'cwproject 1')"); ok = false; }
            header = true;
        } else if (strcmp(s, "name") == 0) copy_str(project->name, sizeof(project->name), value);
        else if (strcmp(s, "module") == 0) copy_str(project->module_rel, sizeof(project->module_rel), value);
        else if (strcmp(s, "scenes") == 0) copy_str(project->scenes_rel, sizeof(project->scenes_rel), value);
        else if (strcmp(s, "main_scene") == 0) copy_str(project->main_scene_rel, sizeof(project->main_scene_rel), value);
        else if (strcmp(s, "render") == 0) copy_str(project->render_rel, sizeof(project->render_rel), value);
        else if (strcmp(s, "assets") == 0) copy_str(project->assets_rel, sizeof(project->assets_rel), value);
        else if (strcmp(s, "title") == 0) copy_str(project->title, sizeof(project->title), value);
        else if (strcmp(s, "window") == 0) {
            int w = 0, h = 0;
            if (sscanf(value, "%d %d", &w, &h) != 2 || w < 64 || h < 64 || w > 16384 || h > 16384) {
                char message[96];
                snprintf(message, sizeof(message), "line %d: window needs a width and a height (64..16384)", line_no);
                set_error(error, error_capacity, message);
                ok = false;
            } else { project->window_width = w; project->window_height = h; }
        } else if (strcmp(s, "ui") == 0) {
            float w = 0, h = 0;
            char mode[32] = "";
            int got = sscanf(value, "%f %f %31s", &w, &h, mode);
            if (got < 2 || !(w >= 64) || !(h >= 64) || w > 16384 || h > 16384 || (got == 3 && !cw_ui_scale_mode_parse(mode, &project->ui.mode))) {
                char message[96];
                snprintf(message, sizeof(message), "line %d: ui needs a reference width and height (64..16384) and a scale mode", line_no);
                set_error(error, error_capacity, message);
                ok = false;
            } else { project->ui.width = w; project->ui.height = h; }
        } else if (strcmp(s, "renderer") == 0) {
            if (!cw_renderer_parse(value, &project->renderer)) {
                char message[96];
                snprintf(message, sizeof(message), "line %d: renderer must be 2d or 3d", line_no);
                set_error(error, error_capacity, message);
                ok = false;
            }
        } else if (strcmp(s, "source") == 0) {
            if (project->source_count == CW_PROJECT_MAX_SOURCES) { set_error(error, error_capacity, "too many sources"); ok = false; }
            else {
                copy_str(project->sources_rel[project->source_count], CW_GAME_PATH_CAPACITY, value);
                cw_project_path(project, value, project->sources[project->source_count++], CW_GAME_PATH_CAPACITY);
            }
        } else {
            char message[128];
            snprintf(message, sizeof(message), "line %d: unknown key '%.32s'", line_no, s);
            set_error(error, error_capacity, message);
            ok = false;
        }
    }
    cw_buffer_free(&image);
    if (ok && !header) { set_error(error, error_capacity, "empty project file"); ok = false; }
    if (ok && !project->name[0]) { set_error(error, error_capacity, "the project has no name"); ok = false; }
    if (ok && !project->module_rel[0]) { set_error(error, error_capacity, "the project names no module"); ok = false; }
    if (!ok) return false;
    cw_project_path(project, project->module_rel, project->module, sizeof(project->module));
    append_str(project->module, sizeof(project->module), cw_game_library_suffix());
    cw_project_path(project, project->scenes_rel, project->scenes_dir, sizeof(project->scenes_dir));
    cw_project_path(project, project->main_scene_rel, project->main_scene, sizeof(project->main_scene));
    cw_project_path(project, project->render_rel, project->render, sizeof(project->render));
    cw_project_path(project, project->assets_rel, project->assets, sizeof(project->assets));
    if (!project->title[0]) copy_str(project->title, sizeof(project->title), project->name);
    return true;
}
static void write_text(cw_writer *out, const char *text) { cw_writer_bytes(out, text, strlen(text)); }
bool cw_project_save(const cw_project *project, char *error, size_t error_capacity) {
    if (!project || !project->path[0]) { set_error(error, error_capacity, "no project path"); return false; }
    cw_writer out;
    cw_writer_init(&out);
    char line[CW_GAME_PATH_CAPACITY + 64];
    write_text(&out, "cwproject 1\n");
    snprintf(line, sizeof(line), "name %s\n", project->name);
    write_text(&out, line);
    write_text(&out, "# the shared library the editor and player load (platform suffix added)\n");
    snprintf(line, sizeof(line), "module %s\n", project->module_rel);
    write_text(&out, line);
    write_text(&out, "# sources compiled into the module and into exported games\n");
    for (size_t i = 0; i < project->source_count; ++i) {
        snprintf(line, sizeof(line), "source %s\n", project->sources_rel[i]);
        write_text(&out, line);
    }
    snprintf(line, sizeof(line), "scenes %s\n", project->scenes_rel);
    write_text(&out, line);
    if (project->main_scene_rel[0]) {
        snprintf(line, sizeof(line), "main_scene %s\n", project->main_scene_rel);
        write_text(&out, line);
    }
    write_text(&out, "# rendering settings (render2d tunables, edited in the editor's settings panel)\n");
    snprintf(line, sizeof(line), "render %s\n", project->render_rel);
    write_text(&out, line);
    write_text(&out, "# the asset manifest (the editor's asset browser maintains it)\n");
    snprintf(line, sizeof(line), "assets %s\n", project->assets_rel);
    write_text(&out, line);
    write_text(&out, "# the player's window\n");
    snprintf(line, sizeof(line), "window %d %d\ntitle %s\n", project->window_width, project->window_height, project->title);
    write_text(&out, line);
    write_text(&out, "# the interface's reference resolution and how it scales to the window (expand|shrink|width|height|pixels)\n");
    snprintf(line, sizeof(line), "ui %g %g %s\n", (double)project->ui.width, (double)project->ui.height, cw_ui_scale_mode_name(project->ui.mode));
    write_text(&out, line);
    write_text(&out, "# how the hosts view the world: 2d (render2d's camera) or 3d (render3d: Shape3D primitives, a perspective view)\n");
    snprintf(line, sizeof(line), "renderer %s\n", cw_renderer_name(project->renderer));
    write_text(&out, line);
    cw_file_error ferr;
    bool ok = cw_file_write(project->path, out.data, out.size, &ferr);
    cw_writer_destroy(&out);
    if (!ok) {
        char message[CW_GAME_PATH_CAPACITY + 64];
        snprintf(message, sizeof(message), "cannot write %s: %s", project->path, cw_file_status_string(ferr.status));
        set_error(error, error_capacity, message);
    }
    return ok;
}
