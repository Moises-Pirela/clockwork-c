/* host_canvas: a cw_canvas implemented with render2d, for the desktop hosts
 * (the editor and the player). Shapes go to the world-space layer, lights
 * and occluders to r2d_lighting, and every pick a module declares is
 * recorded so the host can hit-test it (r2d_pick_circle) and draw hover
 * and selection feedback after the module has drawn. */
#ifndef HOST_CANVAS_H
#define HOST_CANVAS_H

#include "cw_game.h"
#include "r2d_sprite.h"

typedef struct { cw_entity_id id; float x, y, radius; } host_pick;
/* Materials, textures and custom shaders resolved from the project's assets
 * by id and kept for the window's lifetime (host_canvas_reload forgets
 * them, e.g. after the material editor saved). */
typedef struct { uint32_t id; r2d_material material; uint32_t texture, shader; bool missing; char error[160]; } host_material;
typedef struct { uint32_t id; r2d_texture texture; bool missing; char error[160]; } host_texture;
typedef struct { uint32_t id; r2d_shader shader; bool missing; } host_shader;
typedef struct {
    cw_canvas canvas;          /* pass &hc->canvas to the module */
    host_pick *picks;
    size_t pick_count, pick_capacity;
    bool lights;               /* forward lights and occluders to the renderer */
    bool pickable;             /* register picks with render2d */
    const cw_assets *assets;   /* where materials, textures and shaders come from */
    host_material *materials;
    size_t material_count, material_capacity;
    host_texture *textures;
    size_t texture_count, texture_capacity;
    host_shader *shaders;
    size_t shader_count, shader_capacity;
    bool reload_pending;       /* caches are dropped at the next host_canvas_begin */
} host_canvas;

void host_canvas_init(host_canvas *hc);
void host_canvas_destroy(host_canvas *hc);
void host_canvas_begin(host_canvas *hc);                 /* once per frame, before the module draws */
const host_pick *host_canvas_find(const host_canvas *hc, cw_entity_id id);
void host_canvas_set_assets(host_canvas *hc, const cw_assets *assets);
/* Drops every cached material, texture and shader. Deferred to the next
 * host_canvas_begin so sprites already queued this frame keep their GPU
 * resources until they are flushed. */
void host_canvas_reload(host_canvas *hc);
const host_material *host_canvas_material(host_canvas *hc, uint32_t asset_id);   /* NULL when unknown; check ->missing */
const host_texture *host_canvas_texture(host_canvas *hc, uint32_t asset_id);
const host_shader *host_canvas_shader(host_canvas *hc, uint32_t asset_id);
/* Draws a material directly (the material editor's preview). */
void host_canvas_draw_material(host_canvas *hc, const r2d_material *material, r2d_vec2 center, r2d_vec2 size, float rotation, int layer);

#endif
