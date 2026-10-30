#include "render3d.h"
#include "r3d_mesh.h"
#include "r3d_shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_log.h"

#define TARGET_FORMAT SG_PIXELFORMAT_RGBA8
#define DEPTH_FORMAT SG_PIXELFORMAT_DEPTH
#define MAX_COMPOSITE_PIPELINES 8

typedef struct { sg_buffer vertices, indices, edges; int index_count, edge_count; r3d_mesh cpu; } gpu_mesh;   /* the CPU copy feeds the decal clipping */
typedef struct {
    r3d_primitive prim;   /* kind R3D_PRIMITIVE_COUNT marks a dynamic mesh */
    r3d_material material;
    r3d_mat4 model;
    float depth;          /* distance along the view direction, for sorting translucent shapes */
    bool translucent;
    bool decal;                    /* a decal's projected triangles: blended over the opaque shapes, before the translucent ones */
    int index_base, index_count;   /* dynamic meshes: their range in the frame's index buffer */
    r3d_vec3 lo, hi;               /* world bounds */
} item;
typedef struct { float x, y, z; uint8_t rgba[4]; } line_vertex;
typedef struct { float x, y, u, v; } quad_vertex;
typedef struct { uint64_t id; r3d_vec3 lo, hi; r3d_plane planes[R3D_MAX_PICK_PLANES]; int plane_count; } pick_box;   /* plane_count 0: a box */
typedef struct { sg_pixel_format color, depth; int samples; sg_pipeline pipeline; } composite_pipeline;
typedef struct { float mvp[16], model[16], shape[4], uvt[4]; } mesh_vs_uniforms;
typedef struct {
    float tint[4], specular[4], emissive[4], params[4], ambient[4], camera[4], shadow_params[4], shadow_matrix[16];
    float local_params[4], local_matrices[R3D_SHADOW_LAYERS][16], lights[R3D_MAX_LIGHTS * 4][4];
} mesh_fs_uniforms;
typedef struct { float mvp[16], shape[4]; } shadow_vs_uniforms;
typedef struct { float inv_view_proj[16], camera[4]; } sky_vs_uniforms;
typedef struct { float zenith[4], horizon[4], ground[4], sun[4], sun_color[4]; } sky_fs_uniforms;
#define MESH_KIND R3D_PRIMITIVE_COUNT

static struct {
    bool valid, setup_gfx, zero_to_one, origin_top_left;
    r3d_desc desc;
    r3d_camera camera;
    float vx, vy, vw, vh, dpi;          /* the viewport rectangle, logical pixels */
    int target_w, target_h, samples;
    sg_image color_msaa, color, depth;
    sg_view color_attachment, resolve_attachment, color_texture, depth_attachment;
    sg_sampler sampler;
    sg_shader mesh_shader, line_shader, composite_shader;
    sg_pipeline pip_solid, pip_solid_twosided, pip_blend, pip_wire, pip_lines, pip_lines_overlay;
    composite_pipeline composites[MAX_COMPOSITE_PIPELINES];
    int composite_count;
    sg_buffer line_buffer, quad_buffer, mesh_vertices, mesh_indices;
    sg_sampler samplers[4];             /* [nearest][repeat] */
    r3d_texture white;
    gpu_mesh meshes[R3D_PRIMITIVE_COUNT];
    item *items;
    int item_count;
    int *order;                         /* item indices in draw order */
    line_vertex *lines, *overlay;       /* depth-tested lines; overlay lines (always visible) */
    int line_count, overlay_count;      /* vertices */
    r3d_vertex *dyn_vertices;           /* this frame's dynamic meshes */
    uint32_t *dyn_indices;
    int dyn_vertex_count, dyn_index_count;
    pick_box *boxes;
    int box_count;
    r3d_decal decals[R3D_MAX_DECALS];   /* this frame's decals, their materials copied */
    r3d_material decal_materials[R3D_MAX_DECALS];
    int decal_count;
    sg_pipeline pip_decal;
    r3d_light lights[R3D_MAX_LIGHTS];
    int light_count;
    float ambient[3];
    bool default_lighting;
    /* shadows: an R32F depth map rendered from the first shadowing directional light */
    bool shadows, shadow_ok;
    int shadow_size;
    sg_image shadow_color, shadow_depth;
    sg_view shadow_attachment, shadow_depth_attachment, shadow_texture;
    sg_sampler shadow_sampler;
    sg_shader shadow_shader;
    sg_pipeline pip_shadow, pip_shadow_mesh;
    /* spot and point shadows: layers of one R32F array target */
    int local_size;
    sg_image local_color, local_depth;
    sg_view local_attachments[R3D_SHADOW_LAYERS], local_depth_attachment, local_texture;
    sg_pipeline pip_shadow_local;
    r3d_vec3 scene_lo, scene_hi;        /* bounds of this frame's geometry */
    bool scene_bounded;
    /* the sky */
    r3d_sky sky;
    bool sky_set;
    sg_shader sky_shader;
    sg_pipeline pip_sky;
    sg_buffer sky_quad;
    r3d_texture black_cube;
    r3d_mat4 view, proj, view_proj;
    r3d_view_mode view_mode;
    bool rendered, quad_written;
    r3d_stats stats, pending;
} S;

/* ---------------------------------------------------------------------------
 * resources
 * ------------------------------------------------------------------------- */
static void destroy_target(void) {
    if (S.color_attachment.id) sg_destroy_view(S.color_attachment);
    if (S.resolve_attachment.id) sg_destroy_view(S.resolve_attachment);
    if (S.color_texture.id) sg_destroy_view(S.color_texture);
    if (S.depth_attachment.id) sg_destroy_view(S.depth_attachment);
    if (S.color_msaa.id) sg_destroy_image(S.color_msaa);
    if (S.color.id) sg_destroy_image(S.color);
    if (S.depth.id) sg_destroy_image(S.depth);
    S.color_attachment.id = S.resolve_attachment.id = S.color_texture.id = S.depth_attachment.id = 0;
    S.color_msaa.id = S.color.id = S.depth.id = 0;
    S.target_w = S.target_h = 0;
}
static bool make_target(int width, int height) {
    destroy_target();
    sg_image_desc img;
    sg_view_desc view;
    memset(&img, 0, sizeof(img));
    img.width = width;
    img.height = height;
    img.pixel_format = TARGET_FORMAT;
    if (S.samples > 1) {
        img.sample_count = S.samples;
        img.usage.color_attachment = true;
        img.label = "r3d-color-msaa";
        S.color_msaa = sg_make_image(&img);
        memset(&view, 0, sizeof(view));
        view.color_attachment.image = S.color_msaa;
        S.color_attachment = sg_make_view(&view);
        img.sample_count = 1;
        img.usage.color_attachment = false;
        img.usage.resolve_attachment = true;
        img.label = "r3d-color";
        S.color = sg_make_image(&img);
        memset(&view, 0, sizeof(view));
        view.resolve_attachment.image = S.color;
        S.resolve_attachment = sg_make_view(&view);
    } else {
        img.sample_count = 1;
        img.usage.color_attachment = true;
        img.label = "r3d-color";
        S.color = sg_make_image(&img);
        memset(&view, 0, sizeof(view));
        view.color_attachment.image = S.color;
        S.color_attachment = sg_make_view(&view);
    }
    memset(&view, 0, sizeof(view));
    view.texture.image = S.color;
    S.color_texture = sg_make_view(&view);
    memset(&img, 0, sizeof(img));
    img.width = width;
    img.height = height;
    img.pixel_format = DEPTH_FORMAT;
    img.sample_count = S.samples;
    img.usage.depth_stencil_attachment = true;
    img.label = "r3d-depth";
    S.depth = sg_make_image(&img);
    memset(&view, 0, sizeof(view));
    view.depth_stencil_attachment.image = S.depth;
    S.depth_attachment = sg_make_view(&view);
    bool ok = sg_query_image_state(S.color) == SG_RESOURCESTATE_VALID && sg_query_image_state(S.depth) == SG_RESOURCESTATE_VALID
        && sg_query_view_state(S.color_texture) == SG_RESOURCESTATE_VALID && sg_query_view_state(S.depth_attachment) == SG_RESOURCESTATE_VALID
        && (S.samples > 1 ? sg_query_image_state(S.color_msaa) == SG_RESOURCESTATE_VALID : true);
    if (!ok) { destroy_target(); return false; }
    S.target_w = width;
    S.target_h = height;
    return true;
}
static void select_sources(sg_shader_desc *d, const char *glsl_vs, const char *glsl_fs, const char *hlsl_vs, const char *hlsl_fs, const char *msl_vs, const char *msl_fs) {
    switch (sg_query_backend()) {
        case SG_BACKEND_GLCORE:
            d->vertex_func.source = glsl_vs;
            d->fragment_func.source = glsl_fs;
            break;
        case SG_BACKEND_D3D11:
            d->vertex_func.source = hlsl_vs;
            d->vertex_func.entry = "vs_main";
            d->fragment_func.source = hlsl_fs;
            d->fragment_func.entry = "fs_main";
            break;
        default:
            d->vertex_func.source = msl_vs;
            d->vertex_func.entry = "vs_main";
            d->fragment_func.source = msl_fs;
            d->fragment_func.entry = "fs_main";
            break;
    }
}
static sg_shader make_mesh_shader(void) {
    sg_shader_desc d;
    memset(&d, 0, sizeof(d));
    select_sources(&d, r3d_mesh_vs_glsl, r3d_mesh_fs_glsl, r3d_mesh_vs_hlsl, r3d_mesh_fs_hlsl, r3d_mesh_vs_msl, r3d_mesh_fs_msl);
    d.attrs[0].glsl_name = "pos";
    d.attrs[0].hlsl_sem_name = "POSITION";
    d.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.attrs[1].glsl_name = "normal";
    d.attrs[1].hlsl_sem_name = "NORMAL";
    d.attrs[1].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.attrs[2].glsl_name = "uv";
    d.attrs[2].hlsl_sem_name = "TEXCOORD";
    d.attrs[2].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.attrs[3].glsl_name = "color_in";
    d.attrs[3].hlsl_sem_name = "COLOR";
    d.attrs[3].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    d.uniform_blocks[0].size = sizeof(mesh_vs_uniforms);
    d.uniform_blocks[0].hlsl_register_b_n = 0;
    d.uniform_blocks[0].msl_buffer_n = 0;
    d.uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_MAT4;
    d.uniform_blocks[0].glsl_uniforms[0].glsl_name = "mvp";
    d.uniform_blocks[0].glsl_uniforms[1].type = SG_UNIFORMTYPE_MAT4;
    d.uniform_blocks[0].glsl_uniforms[1].glsl_name = "model";
    d.uniform_blocks[0].glsl_uniforms[2].type = SG_UNIFORMTYPE_FLOAT4;
    d.uniform_blocks[0].glsl_uniforms[2].glsl_name = "shape";
    d.uniform_blocks[0].glsl_uniforms[3].type = SG_UNIFORMTYPE_FLOAT4;
    d.uniform_blocks[0].glsl_uniforms[3].glsl_name = "uvt";
    d.uniform_blocks[1].stage = SG_SHADERSTAGE_FRAGMENT;
    d.uniform_blocks[1].size = sizeof(mesh_fs_uniforms);
    d.uniform_blocks[1].hlsl_register_b_n = 0;
    d.uniform_blocks[1].msl_buffer_n = 0;
    d.uniform_blocks[1].layout = SG_UNIFORMLAYOUT_STD140;
    static const char *const fs_names[7] = {"tint", "specular", "emissive", "params", "ambient", "camera", "shadow_params"};
    for (int i = 0; i < 7; ++i) {
        d.uniform_blocks[1].glsl_uniforms[i].type = SG_UNIFORMTYPE_FLOAT4;
        d.uniform_blocks[1].glsl_uniforms[i].glsl_name = fs_names[i];
    }
    d.uniform_blocks[1].glsl_uniforms[7].type = SG_UNIFORMTYPE_MAT4;
    d.uniform_blocks[1].glsl_uniforms[7].glsl_name = "shadow_matrix";
    d.uniform_blocks[1].glsl_uniforms[8].type = SG_UNIFORMTYPE_FLOAT4;
    d.uniform_blocks[1].glsl_uniforms[8].glsl_name = "local_params";
    d.uniform_blocks[1].glsl_uniforms[9].type = SG_UNIFORMTYPE_MAT4;
    d.uniform_blocks[1].glsl_uniforms[9].array_count = R3D_SHADOW_LAYERS;
    d.uniform_blocks[1].glsl_uniforms[9].glsl_name = "local_matrices";
    d.uniform_blocks[1].glsl_uniforms[10].type = SG_UNIFORMTYPE_FLOAT4;
    d.uniform_blocks[1].glsl_uniforms[10].array_count = R3D_MAX_LIGHTS * 4;
    d.uniform_blocks[1].glsl_uniforms[10].glsl_name = "lights";
    d.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    d.views[0].texture.image_type = SG_IMAGETYPE_2D;
    d.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    d.views[0].texture.hlsl_register_t_n = 0;
    d.views[0].texture.msl_texture_n = 0;
    d.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    d.samplers[0].hlsl_register_s_n = 0;
    d.samplers[0].msl_sampler_n = 0;
    d.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[0].view_slot = 0;
    d.texture_sampler_pairs[0].sampler_slot = 0;
    d.texture_sampler_pairs[0].glsl_name = "tex";
    /* the shadow map: an R32F target, read with a non-filtering sampler */
    d.views[1].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    d.views[1].texture.image_type = SG_IMAGETYPE_2D;
    d.views[1].texture.sample_type = SG_IMAGESAMPLETYPE_UNFILTERABLE_FLOAT;
    d.views[1].texture.hlsl_register_t_n = 1;
    d.views[1].texture.msl_texture_n = 1;
    d.samplers[1].stage = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[1].sampler_type = SG_SAMPLERTYPE_NONFILTERING;
    d.samplers[1].hlsl_register_s_n = 1;
    d.samplers[1].msl_sampler_n = 1;
    d.texture_sampler_pairs[1].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[1].view_slot = 1;
    d.texture_sampler_pairs[1].sampler_slot = 1;
    d.texture_sampler_pairs[1].glsl_name = "shadow_map";
    /* the spot and point shadow layers, with the same non-filtering sampler */
    d.views[2].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    d.views[2].texture.image_type = SG_IMAGETYPE_ARRAY;
    d.views[2].texture.sample_type = SG_IMAGESAMPLETYPE_UNFILTERABLE_FLOAT;
    d.views[2].texture.hlsl_register_t_n = 2;
    d.views[2].texture.msl_texture_n = 2;
    d.texture_sampler_pairs[2].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[2].view_slot = 2;
    d.texture_sampler_pairs[2].sampler_slot = 1;
    d.texture_sampler_pairs[2].glsl_name = "shadow_layers";
    d.label = "r3d-mesh";
    return sg_make_shader(&d);
}
static sg_shader make_sky_shader(void) {
    sg_shader_desc d;
    memset(&d, 0, sizeof(d));
    select_sources(&d, r3d_sky_vs_glsl, r3d_sky_fs_glsl, r3d_sky_vs_hlsl, r3d_sky_fs_hlsl, r3d_sky_vs_msl, r3d_sky_fs_msl);
    d.attrs[0].glsl_name = "pos";
    d.attrs[0].hlsl_sem_name = "POSITION";
    d.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    d.uniform_blocks[0].size = sizeof(sky_vs_uniforms);
    d.uniform_blocks[0].hlsl_register_b_n = 0;
    d.uniform_blocks[0].msl_buffer_n = 0;
    d.uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_MAT4;
    d.uniform_blocks[0].glsl_uniforms[0].glsl_name = "inv_view_proj";
    d.uniform_blocks[0].glsl_uniforms[1].type = SG_UNIFORMTYPE_FLOAT4;
    d.uniform_blocks[0].glsl_uniforms[1].glsl_name = "camera";
    d.uniform_blocks[1].stage = SG_SHADERSTAGE_FRAGMENT;
    d.uniform_blocks[1].size = sizeof(sky_fs_uniforms);
    d.uniform_blocks[1].hlsl_register_b_n = 0;
    d.uniform_blocks[1].msl_buffer_n = 0;
    d.uniform_blocks[1].layout = SG_UNIFORMLAYOUT_STD140;
    static const char *const names[5] = {"zenith", "horizon", "ground", "sun", "sun_color"};
    for (int i = 0; i < 5; ++i) {
        d.uniform_blocks[1].glsl_uniforms[i].type = SG_UNIFORMTYPE_FLOAT4;
        d.uniform_blocks[1].glsl_uniforms[i].glsl_name = names[i];
    }
    d.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    d.views[0].texture.image_type = SG_IMAGETYPE_CUBE;
    d.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    d.views[0].texture.hlsl_register_t_n = 0;
    d.views[0].texture.msl_texture_n = 0;
    d.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    d.samplers[0].hlsl_register_s_n = 0;
    d.samplers[0].msl_sampler_n = 0;
    d.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[0].view_slot = 0;
    d.texture_sampler_pairs[0].sampler_slot = 0;
    d.texture_sampler_pairs[0].glsl_name = "sky_cube";
    d.label = "r3d-sky";
    return sg_make_shader(&d);
}
static sg_shader make_shadow_shader(void) {
    sg_shader_desc d;
    memset(&d, 0, sizeof(d));
    select_sources(&d, r3d_shadow_vs_glsl, r3d_shadow_fs_glsl, r3d_shadow_vs_hlsl, r3d_shadow_fs_hlsl, r3d_shadow_vs_msl, r3d_shadow_fs_msl);
    d.attrs[0].glsl_name = "pos";
    d.attrs[0].hlsl_sem_name = "POSITION";
    d.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    d.uniform_blocks[0].size = sizeof(shadow_vs_uniforms);
    d.uniform_blocks[0].hlsl_register_b_n = 0;
    d.uniform_blocks[0].msl_buffer_n = 0;
    d.uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_MAT4;
    d.uniform_blocks[0].glsl_uniforms[0].glsl_name = "mvp";
    d.uniform_blocks[0].glsl_uniforms[1].type = SG_UNIFORMTYPE_FLOAT4;
    d.uniform_blocks[0].glsl_uniforms[1].glsl_name = "shape";
    d.label = "r3d-shadow";
    return sg_make_shader(&d);
}
/* The shadow map and its pipelines; false when the backend cannot render R32F. */
static bool make_shadow_resources(void) {
    if (S.shadow_size <= 0) return false;
    sg_pixelformat_info info = sg_query_pixelformat(SG_PIXELFORMAT_R32F);
    if (!info.render) return false;
    S.shadow_shader = make_shadow_shader();
    if (sg_query_shader_state(S.shadow_shader) != SG_RESOURCESTATE_VALID) return false;
    sg_image_desc img;
    memset(&img, 0, sizeof(img));
    img.width = img.height = S.shadow_size;
    img.pixel_format = SG_PIXELFORMAT_R32F;
    img.sample_count = 1;
    img.usage.color_attachment = true;
    img.label = "r3d-shadow-map";
    S.shadow_color = sg_make_image(&img);
    img.pixel_format = DEPTH_FORMAT;
    img.usage.color_attachment = false;
    img.usage.depth_stencil_attachment = true;
    img.label = "r3d-shadow-depth";
    S.shadow_depth = sg_make_image(&img);
    sg_view_desc view;
    memset(&view, 0, sizeof(view));
    view.color_attachment.image = S.shadow_color;
    S.shadow_attachment = sg_make_view(&view);
    memset(&view, 0, sizeof(view));
    view.depth_stencil_attachment.image = S.shadow_depth;
    S.shadow_depth_attachment = sg_make_view(&view);
    memset(&view, 0, sizeof(view));
    view.texture.image = S.shadow_color;
    S.shadow_texture = sg_make_view(&view);
    sg_sampler_desc smp;
    memset(&smp, 0, sizeof(smp));
    smp.min_filter = smp.mag_filter = SG_FILTER_NEAREST;
    smp.wrap_u = smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    smp.label = "r3d-shadow-sampler";
    S.shadow_sampler = sg_make_sampler(&smp);
    sg_pipeline_desc p;
    memset(&p, 0, sizeof(p));
    p.shader = S.shadow_shader;
    p.layout.buffers[0].stride = sizeof(r3d_vertex);
    p.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT4;
    p.index_type = SG_INDEXTYPE_UINT32;
    p.cull_mode = SG_CULLMODE_NONE;
    p.colors[0].pixel_format = SG_PIXELFORMAT_R32F;
    p.depth.pixel_format = DEPTH_FORMAT;
    p.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    p.depth.write_enabled = true;
    p.sample_count = 1;
    p.label = "r3d-shadow";
    S.pip_shadow = sg_make_pipeline(&p);
    /* the spot/point layers: one array image, an attachment view per layer, one depth buffer reused */
    memset(&img, 0, sizeof(img));
    img.type = SG_IMAGETYPE_ARRAY;
    img.width = img.height = S.local_size;
    img.num_slices = R3D_SHADOW_LAYERS;
    img.pixel_format = SG_PIXELFORMAT_R32F;
    img.sample_count = 1;
    img.usage.color_attachment = true;
    img.label = "r3d-shadow-layers";
    S.local_color = sg_make_image(&img);
    memset(&img, 0, sizeof(img));
    img.width = img.height = S.local_size;
    img.pixel_format = DEPTH_FORMAT;
    img.sample_count = 1;
    img.usage.depth_stencil_attachment = true;
    img.label = "r3d-shadow-layers-depth";
    S.local_depth = sg_make_image(&img);
    for (int i = 0; i < R3D_SHADOW_LAYERS; ++i) {
        memset(&view, 0, sizeof(view));
        view.color_attachment.image = S.local_color;
        view.color_attachment.slice = i;
        S.local_attachments[i] = sg_make_view(&view);
    }
    memset(&view, 0, sizeof(view));
    view.depth_stencil_attachment.image = S.local_depth;
    S.local_depth_attachment = sg_make_view(&view);
    memset(&view, 0, sizeof(view));
    view.texture.image = S.local_color;
    S.local_texture = sg_make_view(&view);
    p.label = "r3d-shadow-local";
    S.pip_shadow_local = sg_make_pipeline(&p);   /* same formats, same shader */
    return sg_query_image_state(S.shadow_color) == SG_RESOURCESTATE_VALID && sg_query_image_state(S.shadow_depth) == SG_RESOURCESTATE_VALID
        && sg_query_view_state(S.shadow_texture) == SG_RESOURCESTATE_VALID && sg_query_pipeline_state(S.pip_shadow) == SG_RESOURCESTATE_VALID
        && sg_query_image_state(S.local_color) == SG_RESOURCESTATE_VALID && sg_query_view_state(S.local_texture) == SG_RESOURCESTATE_VALID
        && sg_query_pipeline_state(S.pip_shadow_local) == SG_RESOURCESTATE_VALID;
}
static bool make_sky_resources(void) {
    S.sky_shader = make_sky_shader();
    if (sg_query_shader_state(S.sky_shader) != SG_RESOURCESTATE_VALID) return false;
    sg_pipeline_desc p;
    memset(&p, 0, sizeof(p));
    p.shader = S.sky_shader;
    p.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    p.colors[0].pixel_format = TARGET_FORMAT;
    p.depth.pixel_format = DEPTH_FORMAT;
    p.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;   /* only where nothing was drawn: the quad sits at the far plane */
    p.depth.write_enabled = false;
    p.sample_count = S.samples;
    p.label = "r3d-sky";
    S.pip_sky = sg_make_pipeline(&p);
    static const float quad[12] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
    sg_buffer_desc vb;
    memset(&vb, 0, sizeof(vb));
    vb.data.ptr = quad;
    vb.data.size = sizeof(quad);
    vb.label = "r3d-sky-quad";
    S.sky_quad = sg_make_buffer(&vb);
    static const uint8_t black[4] = {0, 0, 0, 255};
    const void *faces[6] = {black, black, black, black, black, black};
    if (!r3d_texture_create_cube(&S.black_cube, 1, faces)) return false;
    return sg_query_pipeline_state(S.pip_sky) == SG_RESOURCESTATE_VALID && sg_query_buffer_state(S.sky_quad) == SG_RESOURCESTATE_VALID;
}
static void destroy_shadow_resources(void) {
    if (S.pip_shadow_local.id) sg_destroy_pipeline(S.pip_shadow_local);
    if (S.local_texture.id) sg_destroy_view(S.local_texture);
    if (S.local_depth_attachment.id) sg_destroy_view(S.local_depth_attachment);
    for (int i = 0; i < R3D_SHADOW_LAYERS; ++i) { if (S.local_attachments[i].id) sg_destroy_view(S.local_attachments[i]); S.local_attachments[i].id = 0; }
    if (S.local_color.id) sg_destroy_image(S.local_color);
    if (S.local_depth.id) sg_destroy_image(S.local_depth);
    S.pip_shadow_local.id = 0; S.local_texture.id = 0; S.local_depth_attachment.id = 0; S.local_color.id = 0; S.local_depth.id = 0;
    if (S.pip_shadow.id) sg_destroy_pipeline(S.pip_shadow);
    if (S.shadow_sampler.id) sg_destroy_sampler(S.shadow_sampler);
    if (S.shadow_texture.id) sg_destroy_view(S.shadow_texture);
    if (S.shadow_attachment.id) sg_destroy_view(S.shadow_attachment);
    if (S.shadow_depth_attachment.id) sg_destroy_view(S.shadow_depth_attachment);
    if (S.shadow_color.id) sg_destroy_image(S.shadow_color);
    if (S.shadow_depth.id) sg_destroy_image(S.shadow_depth);
    if (S.shadow_shader.id) sg_destroy_shader(S.shadow_shader);
    S.pip_shadow.id = 0; S.shadow_sampler.id = 0; S.shadow_texture.id = 0; S.shadow_attachment.id = 0; S.shadow_depth_attachment.id = 0;
    S.shadow_color.id = 0; S.shadow_depth.id = 0; S.shadow_shader.id = 0;
}
static sg_shader make_line_shader(void) {
    sg_shader_desc d;
    memset(&d, 0, sizeof(d));
    select_sources(&d, r3d_line_vs_glsl, r3d_line_fs_glsl, r3d_line_vs_hlsl, r3d_line_fs_hlsl, r3d_line_vs_msl, r3d_line_fs_msl);
    d.attrs[0].glsl_name = "pos";
    d.attrs[0].hlsl_sem_name = "POSITION";
    d.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.attrs[1].glsl_name = "color_in";
    d.attrs[1].hlsl_sem_name = "COLOR";
    d.attrs[1].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    d.uniform_blocks[0].size = 64;
    d.uniform_blocks[0].hlsl_register_b_n = 0;
    d.uniform_blocks[0].msl_buffer_n = 0;
    d.uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_MAT4;
    d.uniform_blocks[0].glsl_uniforms[0].glsl_name = "mvp";
    d.label = "r3d-line";
    return sg_make_shader(&d);
}
static sg_shader make_composite_shader(void) {
    sg_shader_desc d;
    memset(&d, 0, sizeof(d));
    select_sources(&d, r3d_composite_vs_glsl, r3d_composite_fs_glsl, r3d_composite_vs_hlsl, r3d_composite_fs_hlsl, r3d_composite_vs_msl, r3d_composite_fs_msl);
    d.attrs[0].glsl_name = "pos";
    d.attrs[0].hlsl_sem_name = "POSITION";
    d.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.attrs[1].glsl_name = "uv_in";
    d.attrs[1].hlsl_sem_name = "TEXCOORD";
    d.attrs[1].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    d.views[0].texture.image_type = SG_IMAGETYPE_2D;
    d.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    d.views[0].texture.hlsl_register_t_n = 0;
    d.views[0].texture.msl_texture_n = 0;
    d.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    d.samplers[0].hlsl_register_s_n = 0;
    d.samplers[0].msl_sampler_n = 0;
    d.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[0].view_slot = 0;
    d.texture_sampler_pairs[0].sampler_slot = 0;
    d.texture_sampler_pairs[0].glsl_name = "tex";
    d.label = "r3d-composite";
    return sg_make_shader(&d);
}
static sg_pipeline_desc mesh_pipeline_desc(sg_primitive_type primitive, sg_cull_mode cull, bool blend, bool depth_write) {
    sg_pipeline_desc p;
    memset(&p, 0, sizeof(p));
    p.shader = S.mesh_shader;
    p.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT4;
    p.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT3;
    p.layout.attrs[2].format = SG_VERTEXFORMAT_FLOAT2;
    p.layout.attrs[3].format = SG_VERTEXFORMAT_UBYTE4N;
    p.index_type = SG_INDEXTYPE_UINT32;
    p.primitive_type = primitive;
    p.cull_mode = cull;
    p.face_winding = SG_FACEWINDING_CCW;
    p.colors[0].pixel_format = TARGET_FORMAT;
    p.depth.pixel_format = DEPTH_FORMAT;
    p.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    p.depth.write_enabled = depth_write;
    p.sample_count = S.samples;
    if (blend) {
        p.colors[0].blend.enabled = true;
        p.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        p.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        p.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
        p.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    }
    return p;
}
static sg_pipeline_desc line_pipeline_desc(bool depth_test) {
    sg_pipeline_desc p;
    memset(&p, 0, sizeof(p));
    p.shader = S.line_shader;
    p.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT3;
    p.layout.attrs[1].format = SG_VERTEXFORMAT_UBYTE4N;
    p.primitive_type = SG_PRIMITIVETYPE_LINES;
    p.colors[0].pixel_format = TARGET_FORMAT;
    p.depth.pixel_format = DEPTH_FORMAT;
    p.depth.compare = depth_test ? SG_COMPAREFUNC_LESS_EQUAL : SG_COMPAREFUNC_ALWAYS;
    p.depth.write_enabled = false;
    p.sample_count = S.samples;
    p.colors[0].blend.enabled = true;
    p.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    p.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    p.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    p.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    return p;
}
static bool make_pipelines(void) {
    sg_pipeline_desc p;
    p = mesh_pipeline_desc(SG_PRIMITIVETYPE_TRIANGLES, SG_CULLMODE_BACK, false, true);
    p.label = "r3d-solid";
    S.pip_solid = sg_make_pipeline(&p);
    p = mesh_pipeline_desc(SG_PRIMITIVETYPE_TRIANGLES, SG_CULLMODE_NONE, false, true);
    p.label = "r3d-solid-twosided";
    S.pip_solid_twosided = sg_make_pipeline(&p);
    p = mesh_pipeline_desc(SG_PRIMITIVETYPE_TRIANGLES, SG_CULLMODE_NONE, true, false);
    p.label = "r3d-blend";
    S.pip_blend = sg_make_pipeline(&p);
    p = mesh_pipeline_desc(SG_PRIMITIVETYPE_TRIANGLES, SG_CULLMODE_NONE, true, false);
    p.depth.bias = -2;   /* decals sit on their surfaces: pull them a little towards the camera */
    p.depth.bias_slope_scale = -1;
    p.label = "r3d-decal";
    S.pip_decal = sg_make_pipeline(&p);
    p = mesh_pipeline_desc(SG_PRIMITIVETYPE_LINES, SG_CULLMODE_NONE, true, true);
    p.label = "r3d-wire";
    S.pip_wire = sg_make_pipeline(&p);
    p = line_pipeline_desc(true);
    p.label = "r3d-lines";
    S.pip_lines = sg_make_pipeline(&p);
    p = line_pipeline_desc(false);
    p.label = "r3d-lines-overlay";
    S.pip_lines_overlay = sg_make_pipeline(&p);
    return sg_query_pipeline_state(S.pip_solid) == SG_RESOURCESTATE_VALID && sg_query_pipeline_state(S.pip_solid_twosided) == SG_RESOURCESTATE_VALID && sg_query_pipeline_state(S.pip_decal) == SG_RESOURCESTATE_VALID
        && sg_query_pipeline_state(S.pip_blend) == SG_RESOURCESTATE_VALID && sg_query_pipeline_state(S.pip_wire) == SG_RESOURCESTATE_VALID
        && sg_query_pipeline_state(S.pip_lines) == SG_RESOURCESTATE_VALID && sg_query_pipeline_state(S.pip_lines_overlay) == SG_RESOURCESTATE_VALID;
}
static bool upload_mesh(gpu_mesh *g, r3d_mesh *m, const char *label) {
    if (!r3d_mesh_build_edges(m)) return false;
    sg_buffer_desc d;
    memset(&d, 0, sizeof(d));
    d.data.ptr = m->vertices;
    d.data.size = m->vertex_count * sizeof(*m->vertices);
    d.label = label;
    g->vertices = sg_make_buffer(&d);
    memset(&d, 0, sizeof(d));
    d.usage.index_buffer = true;
    d.data.ptr = m->indices;
    d.data.size = m->index_count * sizeof(*m->indices);
    d.label = label;
    g->indices = sg_make_buffer(&d);
    memset(&d, 0, sizeof(d));
    d.usage.index_buffer = true;
    d.data.ptr = m->edges;
    d.data.size = m->edge_index_count * sizeof(*m->edges);
    d.label = label;
    g->edges = sg_make_buffer(&d);
    g->index_count = (int)m->index_count;
    g->edge_count = (int)m->edge_index_count;
    return sg_query_buffer_state(g->vertices) == SG_RESOURCESTATE_VALID && sg_query_buffer_state(g->indices) == SG_RESOURCESTATE_VALID
        && sg_query_buffer_state(g->edges) == SG_RESOURCESTATE_VALID;
}
static bool make_meshes(void) {
    bool ok = true;
    int rings = S.desc.sphere_rings, sectors = S.desc.sphere_sectors;
    gpu_mesh *g = S.meshes;
    ok = ok && r3d_mesh_sphere(&g[R3D_SPHERE].cpu, rings, sectors) && upload_mesh(&g[R3D_SPHERE], &g[R3D_SPHERE].cpu, "r3d-sphere");
    ok = ok && r3d_mesh_capsule(&g[R3D_CAPSULE].cpu, rings / 2 > 0 ? rings / 2 : 1, sectors) && upload_mesh(&g[R3D_CAPSULE], &g[R3D_CAPSULE].cpu, "r3d-capsule");
    ok = ok && r3d_mesh_cube(&g[R3D_CUBE].cpu) && upload_mesh(&g[R3D_CUBE], &g[R3D_CUBE].cpu, "r3d-cube");
    ok = ok && r3d_mesh_plane(&g[R3D_PLANE].cpu) && upload_mesh(&g[R3D_PLANE], &g[R3D_PLANE].cpu, "r3d-plane");
    return ok;
}
static void destroy_meshes(void) {
    for (int i = 0; i < R3D_PRIMITIVE_COUNT; ++i) {
        gpu_mesh *g = &S.meshes[i];
        if (g->vertices.id) sg_destroy_buffer(g->vertices);
        if (g->indices.id) sg_destroy_buffer(g->indices);
        if (g->edges.id) sg_destroy_buffer(g->edges);
        r3d_mesh_free(&g->cpu);
        memset(g, 0, sizeof(*g));
    }
}

bool r3d_setup(const r3d_desc *desc) {
    r3d_desc d;
    if (desc) d = *desc; else memset(&d, 0, sizeof(d));
    memset(&S, 0, sizeof(S));
    if (d.background.a == 0 && d.background.r == 0 && d.background.g == 0 && d.background.b == 0) d.background = r3d_rgb(30, 33, 40);
    if (d.sample_count <= 0) d.sample_count = 4;
    if (d.max_primitives <= 0) d.max_primitives = 16384;
    if (d.max_lines <= 0) d.max_lines = 65536;
    if (d.max_mesh_vertices <= 0) d.max_mesh_vertices = 262144;
    if (d.max_pick_boxes <= 0) d.max_pick_boxes = 16384;
    if (d.shadow_map_size == 0) d.shadow_map_size = 2048;
    if (d.local_shadow_map_size <= 0) d.local_shadow_map_size = 1024;
    if (d.sphere_rings < 2) d.sphere_rings = 24;
    if (d.sphere_sectors < 3) d.sphere_sectors = 48;
    S.desc = d;
    S.camera = r3d_camera_make(r3d_v3(8, 6, 10), r3d_v3(0, 0, 0));
    S.dpi = 1;
    S.default_lighting = true;
    S.shadow_size = d.shadow_map_size;
    S.local_size = d.local_shadow_map_size;
    if (!d.skip_gfx_setup) {
        sg_desc gfx;
        memset(&gfx, 0, sizeof(gfx));
        gfx.environment = sglue_environment();
        gfx.logger.func = slog_func;
        sg_setup(&gfx);
        S.setup_gfx = true;
    }
    if (!sg_isvalid()) return false;
    S.zero_to_one = sg_query_backend() != SG_BACKEND_GLCORE && sg_query_backend() != SG_BACKEND_GLES3;
    S.origin_top_left = sg_query_features().origin_top_left;
    sg_pixelformat_info info = sg_query_pixelformat(TARGET_FORMAT);
    S.samples = d.sample_count > 1 && info.msaa && sg_query_pixelformat(DEPTH_FORMAT).msaa ? d.sample_count : 1;

    S.mesh_shader = make_mesh_shader();
    S.line_shader = make_line_shader();
    S.composite_shader = make_composite_shader();
    if (sg_query_shader_state(S.mesh_shader) != SG_RESOURCESTATE_VALID || sg_query_shader_state(S.line_shader) != SG_RESOURCESTATE_VALID
        || sg_query_shader_state(S.composite_shader) != SG_RESOURCESTATE_VALID) {
        fputs("render3d: a shader failed to compile; the 3D renderer is disabled\n", stderr);
        return false;
    }
    if (!make_pipelines() || !make_meshes()) {
        fputs("render3d: cannot create pipelines or meshes; the 3D renderer is disabled\n", stderr);
        return false;
    }
    sg_buffer_desc vb;
    memset(&vb, 0, sizeof(vb));
    vb.size = sizeof(line_vertex) * (size_t)d.max_lines * 2;
    vb.usage.write_transient = true;
    vb.label = "r3d-lines";
    S.line_buffer = sg_make_buffer(&vb);
    memset(&vb, 0, sizeof(vb));
    vb.size = sizeof(quad_vertex) * 6;
    vb.usage.write_transient = true;
    vb.label = "r3d-composite-quad";
    S.quad_buffer = sg_make_buffer(&vb);
    memset(&vb, 0, sizeof(vb));
    vb.size = sizeof(r3d_vertex) * (size_t)d.max_mesh_vertices;
    vb.usage.write_transient = true;
    vb.label = "r3d-mesh-vertices";
    S.mesh_vertices = sg_make_buffer(&vb);
    memset(&vb, 0, sizeof(vb));
    vb.size = sizeof(uint32_t) * (size_t)d.max_mesh_vertices * 3;
    vb.usage.index_buffer = true;
    vb.usage.write_transient = true;
    vb.label = "r3d-mesh-indices";
    S.mesh_indices = sg_make_buffer(&vb);
    for (int nearest = 0; nearest < 2; ++nearest)
        for (int repeat = 0; repeat < 2; ++repeat) {
            sg_sampler_desc smp;
            memset(&smp, 0, sizeof(smp));
            smp.min_filter = smp.mag_filter = nearest ? SG_FILTER_NEAREST : SG_FILTER_LINEAR;
            smp.mipmap_filter = SG_FILTER_NEAREST;
            smp.wrap_u = smp.wrap_v = repeat ? SG_WRAP_REPEAT : SG_WRAP_CLAMP_TO_EDGE;
            smp.label = "r3d-sampler";
            S.samplers[nearest * 2 + repeat] = sg_make_sampler(&smp);
        }
    S.sampler = S.samplers[0];
    static const uint8_t white_pixel[4] = {255, 255, 255, 255};
    if (!r3d_texture_create(&S.white, 1, 1, white_pixel)) { fputs("render3d: cannot create the white texture\n", stderr); return false; }
    S.shadow_ok = make_shadow_resources();
    if (!S.shadow_ok) destroy_shadow_resources();
    S.shadows = S.shadow_ok;
    if (!make_sky_resources()) { fputs("render3d: the sky shader failed; no sky will be drawn\n", stderr); }
    S.items = malloc(sizeof(item) * (size_t)d.max_primitives);
    S.order = malloc(sizeof(int) * (size_t)d.max_primitives);
    S.lines = malloc(sizeof(line_vertex) * (size_t)d.max_lines * 2);
    S.overlay = malloc(sizeof(line_vertex) * (size_t)d.max_lines * 2);
    S.dyn_vertices = malloc(sizeof(r3d_vertex) * (size_t)d.max_mesh_vertices);
    S.dyn_indices = malloc(sizeof(uint32_t) * (size_t)d.max_mesh_vertices * 3);
    S.boxes = malloc(sizeof(pick_box) * (size_t)d.max_pick_boxes);
    if (!S.items || !S.order || !S.lines || !S.overlay || !S.dyn_vertices || !S.dyn_indices || !S.boxes) { fputs("render3d: out of memory\n", stderr); return false; }
    S.valid = true;
    return true;
}
void r3d_shutdown(void) {
    if (S.valid || S.mesh_shader.id) {
        destroy_target();
        destroy_meshes();
        for (int i = 0; i < S.composite_count; ++i) sg_destroy_pipeline(S.composites[i].pipeline);
        if (S.pip_solid.id) sg_destroy_pipeline(S.pip_solid);
        if (S.pip_solid_twosided.id) sg_destroy_pipeline(S.pip_solid_twosided);
        if (S.pip_blend.id) sg_destroy_pipeline(S.pip_blend);
        if (S.pip_wire.id) sg_destroy_pipeline(S.pip_wire);
        if (S.pip_decal.id) sg_destroy_pipeline(S.pip_decal);
        if (S.pip_lines.id) sg_destroy_pipeline(S.pip_lines);
        if (S.pip_lines_overlay.id) sg_destroy_pipeline(S.pip_lines_overlay);
        if (S.line_buffer.id) sg_destroy_buffer(S.line_buffer);
        if (S.quad_buffer.id) sg_destroy_buffer(S.quad_buffer);
        if (S.mesh_vertices.id) sg_destroy_buffer(S.mesh_vertices);
        if (S.mesh_indices.id) sg_destroy_buffer(S.mesh_indices);
        for (int i = 0; i < 4; ++i) if (S.samplers[i].id) sg_destroy_sampler(S.samplers[i]);
        r3d_texture_destroy(&S.white);
        r3d_texture_destroy(&S.black_cube);
        destroy_shadow_resources();
        if (S.pip_sky.id) sg_destroy_pipeline(S.pip_sky);
        if (S.sky_quad.id) sg_destroy_buffer(S.sky_quad);
        if (S.sky_shader.id) sg_destroy_shader(S.sky_shader);
        if (S.mesh_shader.id) sg_destroy_shader(S.mesh_shader);
        if (S.line_shader.id) sg_destroy_shader(S.line_shader);
        if (S.composite_shader.id) sg_destroy_shader(S.composite_shader);
    }
    if (S.setup_gfx && sg_isvalid()) sg_shutdown();
    free(S.items);
    free(S.order);
    free(S.lines);
    free(S.overlay);
    free(S.dyn_vertices);
    free(S.dyn_indices);
    free(S.boxes);
    memset(&S, 0, sizeof(S));
}
bool r3d_valid(void) { return S.valid; }

/* ---------------------------------------------------------------------------
 * frame and camera
 * ------------------------------------------------------------------------- */
static void update_matrices(void) {
    float aspect = S.vh > 0 ? S.vw / S.vh : 1;
    S.view = r3d_camera_view(&S.camera);
    S.proj = r3d_camera_projection(&S.camera, aspect, S.zero_to_one);
    S.view_proj = r3d_mat4_multiply(S.proj, S.view);
}
void r3d_begin_frame(float x, float y, float w, float h, float dpi_scale) {
    if (!S.valid) return;
    S.vx = x; S.vy = y;
    S.vw = w > 1 ? w : 1;
    S.vh = h > 1 ? h : 1;
    S.dpi = dpi_scale > 0 ? dpi_scale : 1;
    S.item_count = 0;
    S.line_count = S.overlay_count = 0;
    S.dyn_vertex_count = S.dyn_index_count = 0;
    S.box_count = 0;
    S.decal_count = 0;
    S.light_count = 0;
    S.ambient[0] = S.ambient[1] = S.ambient[2] = 0;
    S.scene_bounded = false;
    S.sky_set = false;
    S.rendered = S.quad_written = false;
    memset(&S.pending, 0, sizeof(S.pending));
    update_matrices();
}
/* --- textures, materials, lights --- */
bool r3d_texture_create(r3d_texture *t, int width, int height, const void *rgba8) {
    if (!t) return false;
    memset(t, 0, sizeof(*t));
    if (!rgba8 || width <= 0 || height <= 0 || !sg_isvalid()) return false;
    sg_image_desc d;
    memset(&d, 0, sizeof(d));
    d.width = width;
    d.height = height;
    d.pixel_format = SG_PIXELFORMAT_RGBA8;
    d.data.ptr = rgba8;
    d.data.size = (size_t)width * (size_t)height * 4;
    d.label = "r3d-texture";
    t->image = sg_make_image(&d);
    if (sg_query_image_state(t->image) != SG_RESOURCESTATE_VALID) { memset(t, 0, sizeof(*t)); return false; }
    sg_view_desc v;
    memset(&v, 0, sizeof(v));
    v.texture.image = t->image;
    t->view = sg_make_view(&v);
    t->width = width;
    t->height = height;
    return true;
}
void r3d_texture_destroy(r3d_texture *t) {
    if (!t) return;
    if (t->view.id) sg_destroy_view(t->view);
    if (t->image.id) sg_destroy_image(t->image);
    memset(t, 0, sizeof(*t));
}
bool r3d_texture_create_cube(r3d_texture *t, int size, const void *const faces[6]) {
    if (!t) return false;
    memset(t, 0, sizeof(*t));
    if (!faces || size <= 0 || !sg_isvalid()) return false;
    sg_image_desc d;
    memset(&d, 0, sizeof(d));
    d.type = SG_IMAGETYPE_CUBE;
    d.width = d.height = size;
    d.pixel_format = SG_PIXELFORMAT_RGBA8;
    /* sokol wants one tightly packed block: faces +X -X +Y -Y +Z -Z. */
    size_t face_bytes = (size_t)size * (size_t)size * 4;
    unsigned char *packed = (unsigned char *)malloc(face_bytes * 6);
    if (!packed) return false;
    for (int f = 0; f < 6; ++f) {
        if (!faces[f]) { free(packed); return false; }
        memcpy(packed + face_bytes * (size_t)f, faces[f], face_bytes);
    }
    d.data.ptr = packed;
    d.data.size = face_bytes * 6;
    d.label = "r3d-cube";
    t->image = sg_make_image(&d);
    free(packed);
    if (sg_query_image_state(t->image) != SG_RESOURCESTATE_VALID) { memset(t, 0, sizeof(*t)); return false; }
    sg_view_desc v;
    memset(&v, 0, sizeof(v));
    v.texture.image = t->image;
    t->view = sg_make_view(&v);
    t->width = t->height = size;
    return true;
}
void r3d_set_sky(const r3d_sky *sky) {
    S.sky_set = sky != NULL;
    if (sky) S.sky = *sky;
}
r3d_material r3d_material_default(void) {
    r3d_material m;
    memset(&m, 0, sizeof(m));
    m.tint[0] = m.tint[1] = m.tint[2] = m.tint[3] = 1;
    m.tiling[0] = m.tiling[1] = 1;
    m.specular[0] = m.specular[1] = m.specular[2] = 0.3f;
    m.shininess = 32;
    return m;
}
void r3d_set_ambient(r3d_color c, float intensity) {
    S.ambient[0] = c.r / 255.0f * intensity;
    S.ambient[1] = c.g / 255.0f * intensity;
    S.ambient[2] = c.b / 255.0f * intensity;
}
void r3d_add_ambient(r3d_color c, float intensity) {
    S.ambient[0] += c.r / 255.0f * intensity;
    S.ambient[1] += c.g / 255.0f * intensity;
    S.ambient[2] += c.b / 255.0f * intensity;
}
bool r3d_add_light(const r3d_light *light) {
    if (!S.valid || !light || S.light_count >= R3D_MAX_LIGHTS) return false;
    r3d_light l = *light;
    l.direction = r3d_length(l.direction) > 1e-6f ? r3d_normalize(l.direction) : r3d_v3(0, -1, 0);
    S.lights[S.light_count++] = l;
    return true;
}
void r3d_set_default_lighting(bool enabled) { S.default_lighting = enabled; }
void r3d_set_shadows(bool enabled) { S.shadows = enabled && S.shadow_ok; }
bool r3d_shadows_enabled(void) { return S.shadows; }
static void grow_bounds(r3d_vec3 lo, r3d_vec3 hi) {
    if (!S.scene_bounded) { S.scene_lo = lo; S.scene_hi = hi; S.scene_bounded = true; return; }
    S.scene_lo = r3d_v3(r3d_minf(S.scene_lo.x, lo.x), r3d_minf(S.scene_lo.y, lo.y), r3d_minf(S.scene_lo.z, lo.z));
    S.scene_hi = r3d_v3(r3d_maxf(S.scene_hi.x, hi.x), r3d_maxf(S.scene_hi.y, hi.y), r3d_maxf(S.scene_hi.z, hi.z));
}
int r3d_light_count(void) { return S.light_count; }
void r3d_set_background(r3d_color color) { S.desc.background = color; }
void r3d_set_view_mode(r3d_view_mode mode) { S.view_mode = (unsigned)mode < R3D_VIEW_MODES ? mode : R3D_VIEW_COLOR; }
r3d_view_mode r3d_view_mode_get(void) { return S.view_mode; }
r3d_camera *r3d_camera_ptr(void) { update_matrices(); return &S.camera; }
void r3d_set_camera(const r3d_camera *camera) { if (camera) { S.camera = *camera; update_matrices(); } }
float r3d_aspect(void) { return S.vh > 0 ? S.vw / S.vh : 1; }
r3d_ray r3d_screen_ray(float sx, float sy) { return r3d_camera_ray(&S.camera, S.vw, S.vh, sx - S.vx, sy - S.vy); }
bool r3d_world_to_screen(r3d_vec3 world, float *sx, float *sy) {
    update_matrices();
    float x, y;
    if (!r3d_project(&S.view_proj, S.vw, S.vh, world, &x, &y, NULL)) return false;
    if (sx) *sx = x + S.vx;
    if (sy) *sy = y + S.vy;
    return true;
}
float r3d_px(float pixels) { return r3d_camera_units_per_pixel(&S.camera, S.vh) * pixels; }
float r3d_px_at(r3d_vec3 world, float pixels) {
    if (S.camera.fov_y <= 0) return r3d_px(pixels);
    float d = r3d_dot(r3d_sub(world, S.camera.position), r3d_camera_forward(&S.camera));
    if (d < S.camera.near_plane) d = S.camera.near_plane;
    return 2 * d * tanf(S.camera.fov_y * 0.5f) / S.vh * pixels;
}

/* ---------------------------------------------------------------------------
 * drawing
 * ------------------------------------------------------------------------- */
void r3d_draw_primitive(const r3d_primitive *p) {
    if (!S.valid || !p || (unsigned)p->kind >= R3D_PRIMITIVE_COUNT || S.item_count >= S.desc.max_primitives) return;
    item *it = &S.items[S.item_count++];
    it->prim = *p;
    it->material = p->material ? *p->material : r3d_material_default();
    it->prim.material = NULL;
    it->model = r3d_primitive_matrix(p);
    it->translucent = p->color.a < 255 || it->material.tint[3] < 1;
    it->depth = r3d_dot(r3d_sub(p->position, S.camera.position), r3d_camera_forward(&S.camera));
    it->index_base = it->index_count = 0;
    r3d_primitive_bounds(p, &it->lo, &it->hi);
    grow_bounds(it->lo, it->hi);
}
void r3d_draw_mesh(const r3d_vertex *vertices, int vertex_count, const uint32_t *indices, int index_count,
                   const r3d_material *material, r3d_color tint, bool wireframe) {
    if (!S.valid || !vertices || !indices || vertex_count <= 0 || index_count <= 0 || S.item_count >= S.desc.max_primitives) return;
    int stored = wireframe ? index_count * 2 : index_count;   /* wireframe: the three edges of every triangle as line pairs */
    if (S.dyn_vertex_count + vertex_count > S.desc.max_mesh_vertices || S.dyn_index_count + stored > S.desc.max_mesh_vertices * 3) return;
    uint32_t base = (uint32_t)S.dyn_vertex_count;
    memcpy(S.dyn_vertices + S.dyn_vertex_count, vertices, sizeof(r3d_vertex) * (size_t)vertex_count);
    uint32_t *out = S.dyn_indices + S.dyn_index_count;
    if (!wireframe) {
        for (int i = 0; i < index_count; ++i) { uint32_t k = indices[i]; out[i] = (k < (uint32_t)vertex_count ? k : 0) + base; }
    } else {
        int n = 0;
        for (int i = 0; i + 2 < index_count; i += 3)
            for (int e = 0; e < 3; ++e) {
                uint32_t a = indices[i + e], b = indices[i + (e + 1) % 3];
                out[n++] = (a < (uint32_t)vertex_count ? a : 0) + base;
                out[n++] = (b < (uint32_t)vertex_count ? b : 0) + base;
            }
        stored = n;
    }
    item *it = &S.items[S.item_count++];
    memset(it, 0, sizeof(*it));
    it->prim.kind = MESH_KIND;
    it->prim.color = tint;
    it->prim.wireframe = wireframe;
    it->prim.id = R3D_ID_NONE;
    it->material = material ? *material : r3d_material_default();
    it->model = r3d_mat4_identity();
    it->translucent = tint.a < 255 || it->material.tint[3] < 1;
    /* sort translucent meshes by their centroid; the bounds feed the shadow frustum */
    r3d_vec3 c = r3d_v3(0, 0, 0), lo = r3d_v3(INFINITY, INFINITY, INFINITY), hi = r3d_v3(-INFINITY, -INFINITY, -INFINITY);
    for (int i = 0; i < vertex_count; ++i) {
        r3d_vec3 p = r3d_v3(vertices[i].x, vertices[i].y, vertices[i].z);
        c = r3d_add(c, p);
        lo = r3d_v3(r3d_minf(lo.x, p.x), r3d_minf(lo.y, p.y), r3d_minf(lo.z, p.z));
        hi = r3d_v3(r3d_maxf(hi.x, p.x), r3d_maxf(hi.y, p.y), r3d_maxf(hi.z, p.z));
    }
    c = r3d_scale(c, 1.0f / (float)vertex_count);
    it->lo = lo; it->hi = hi;
    grow_bounds(lo, hi);
    it->depth = r3d_dot(r3d_sub(c, S.camera.position), r3d_camera_forward(&S.camera));
    it->index_base = S.dyn_index_count;
    it->index_count = stored;
    S.dyn_vertex_count += vertex_count;
    S.dyn_index_count += stored;
}
void r3d_pick_aabb(uint64_t id, r3d_vec3 lo, r3d_vec3 hi) {
    if (!S.valid || S.box_count >= S.desc.max_pick_boxes) return;
    pick_box *b = &S.boxes[S.box_count++];
    b->id = id;
    b->lo = r3d_v3(r3d_minf(lo.x, hi.x), r3d_minf(lo.y, hi.y), r3d_minf(lo.z, hi.z));
    b->hi = r3d_v3(r3d_maxf(lo.x, hi.x), r3d_maxf(lo.y, hi.y), r3d_maxf(lo.z, hi.z));
    b->plane_count = 0;
}
void r3d_pick_convex(uint64_t id, const r3d_plane *planes, int count) {
    if (!S.valid || !planes || count < 4 || S.box_count >= S.desc.max_pick_boxes) return;
    if (count > R3D_MAX_PICK_PLANES) count = R3D_MAX_PICK_PLANES;
    pick_box *b = &S.boxes[S.box_count++];
    b->id = id;
    b->lo = b->hi = r3d_v3(0, 0, 0);
    memcpy(b->planes, planes, sizeof(r3d_plane) * (size_t)count);
    b->plane_count = count;
}
void r3d_draw_sphere(r3d_vec3 center, float radius, r3d_color color) {
    r3d_primitive p = r3d_primitive_make(R3D_SPHERE, center, r3d_v3(radius, radius, radius), color);
    r3d_draw_primitive(&p);
}
void r3d_draw_capsule(r3d_vec3 center, float radius, float height, r3d_vec3 rotation, r3d_color color) {
    r3d_primitive p = r3d_primitive_make(R3D_CAPSULE, center, r3d_v3(radius, height, radius), color);
    p.rotation = rotation;
    r3d_draw_primitive(&p);
}
void r3d_draw_cube(r3d_vec3 center, r3d_vec3 size, r3d_vec3 rotation, r3d_color color) {
    r3d_primitive p = r3d_primitive_make(R3D_CUBE, center, size, color);
    p.rotation = rotation;
    r3d_draw_primitive(&p);
}
void r3d_draw_plane(r3d_vec3 center, float width, float depth, r3d_vec3 rotation, r3d_color color) {
    r3d_primitive p = r3d_primitive_make(R3D_PLANE, center, r3d_v3(width, 1, depth), color);
    p.rotation = rotation;
    r3d_draw_primitive(&p);
}
static void push_line(r3d_vec3 a, r3d_vec3 b, r3d_color c, bool overlay) {
    if (!S.valid) return;
    /* the two kinds share one vertex buffer (depth-tested first), so together they fit max_lines */
    if (S.line_count + S.overlay_count + 2 > S.desc.max_lines * 2) return;
    line_vertex *v = overlay ? &S.overlay[S.overlay_count] : &S.lines[S.line_count];
    v[0].x = a.x; v[0].y = a.y; v[0].z = a.z;
    v[1].x = b.x; v[1].y = b.y; v[1].z = b.z;
    for (int i = 0; i < 2; ++i) { v[i].rgba[0] = c.r; v[i].rgba[1] = c.g; v[i].rgba[2] = c.b; v[i].rgba[3] = c.a; }
    if (overlay) S.overlay_count += 2; else S.line_count += 2;
}
void r3d_draw_line(r3d_vec3 a, r3d_vec3 b, r3d_color color) { push_line(a, b, color, false); }
void r3d_draw_line_overlay(r3d_vec3 a, r3d_vec3 b, r3d_color color) { push_line(a, b, color, true); }
void r3d_draw_outline(const r3d_primitive *p, r3d_color color) {
    if (!S.valid || !p) return;
    r3d_mat4 rot = r3d_mat4_rotation(p->rotation);
    r3d_vec3 ax = r3d_mat4_transform_vector(&rot, r3d_v3(1, 0, 0)), ay = r3d_mat4_transform_vector(&rot, r3d_v3(0, 1, 0)), az = r3d_mat4_transform_vector(&rot, r3d_v3(0, 0, 1));
    const int segments = 48;
    switch (p->kind) {
        case R3D_SPHERE:
        case R3D_CAPSULE: {
            float r = fabsf(p->size.x), h = p->kind == R3D_CAPSULE ? r3d_maxf(p->size.y, 0) * 0.5f : 0;
            r3d_vec3 top = r3d_add(p->position, r3d_scale(ay, h)), bottom = r3d_sub(p->position, r3d_scale(ay, h));
            /* two rings around the axis and two great-circle profiles */
            for (int i = 0; i < segments; ++i) {
                float a0 = 2 * R3D_PI * (float)i / segments, a1 = 2 * R3D_PI * (float)(i + 1) / segments;
                r3d_vec3 d0 = r3d_add(r3d_scale(ax, cosf(a0) * r), r3d_scale(az, sinf(a0) * r)), d1 = r3d_add(r3d_scale(ax, cosf(a1) * r), r3d_scale(az, sinf(a1) * r));
                push_line(r3d_add(top, d0), r3d_add(top, d1), color, true);
                if (h > 0) push_line(r3d_add(bottom, d0), r3d_add(bottom, d1), color, true);
                /* profiles in the xy and zy planes, caps pushed apart by h */
                for (int k = 0; k < 2; ++k) {
                    r3d_vec3 side = k ? az : ax;
                    r3d_vec3 p0 = r3d_add(r3d_scale(side, cosf(a0) * r), r3d_scale(ay, sinf(a0) * r + (sinf(a0) >= 0 ? h : -h)));
                    r3d_vec3 p1 = r3d_add(r3d_scale(side, cosf(a1) * r), r3d_scale(ay, sinf(a1) * r + (sinf(a1) >= 0 ? h : -h)));
                    push_line(r3d_add(p->position, p0), r3d_add(p->position, p1), color, true);
                }
            }
            break;
        }
        case R3D_CUBE:
        case R3D_PLANE: {
            r3d_vec3 s = r3d_primitive_scale(p);
            float hy = p->kind == R3D_PLANE ? 0 : s.y * 0.5f;
            r3d_vec3 corners[8];
            for (int i = 0; i < 8; ++i) {
                float x = (i & 1) ? s.x * 0.5f : -s.x * 0.5f, y = (i & 2) ? hy : -hy, z = (i & 4) ? s.z * 0.5f : -s.z * 0.5f;
                corners[i] = r3d_add(p->position, r3d_add(r3d_scale(ax, x), r3d_add(r3d_scale(ay, y), r3d_scale(az, z))));
            }
            static const int edges[12][2] = {{0, 1}, {1, 5}, {5, 4}, {4, 0}, {2, 3}, {3, 7}, {7, 6}, {6, 2}, {0, 2}, {1, 3}, {5, 7}, {4, 6}};
            int count = p->kind == R3D_PLANE ? 4 : 12;
            for (int i = 0; i < count; ++i) push_line(corners[edges[i][0]], corners[edges[i][1]], color, true);
            break;
        }
        default: break;
    }
}
void r3d_draw_box_outline(r3d_vec3 lo, r3d_vec3 hi, r3d_color color) {
    r3d_vec3 c[8];
    for (int i = 0; i < 8; ++i) c[i] = r3d_v3((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
    static const int edges[12][2] = {{0, 1}, {1, 5}, {5, 4}, {4, 0}, {2, 3}, {3, 7}, {7, 6}, {6, 2}, {0, 2}, {1, 3}, {5, 7}, {4, 6}};
    for (int i = 0; i < 12; ++i) push_line(c[edges[i][0]], c[edges[i][1]], color, true);
}
void r3d_draw_grid(float spacing, int half_lines, r3d_color minor, r3d_color major) {
    if (!S.valid || spacing <= 0 || half_lines <= 0) return;
    float extent = spacing * (float)half_lines;
    for (int i = -half_lines; i <= half_lines; ++i) {
        float d = spacing * (float)i;
        r3d_color c = (i % 10 == 0) ? major : minor;
        push_line(r3d_v3(-extent, 0, d), r3d_v3(extent, 0, d), c, false);
        push_line(r3d_v3(d, 0, -extent), r3d_v3(d, 0, extent), c, false);
    }
}
void r3d_draw_axes(r3d_vec3 o, float length) {
    push_line(o, r3d_add(o, r3d_v3(length, 0, 0)), r3d_rgb(230, 80, 80), true);
    push_line(o, r3d_add(o, r3d_v3(0, length, 0)), r3d_rgb(90, 220, 90), true);
    push_line(o, r3d_add(o, r3d_v3(0, 0, length)), r3d_rgb(90, 130, 255), true);
}

/* ---------------------------------------------------------------------------
 * picking
 * ------------------------------------------------------------------------- */
/* The surface normal of a primitive at a point on it. */
static r3d_vec3 primitive_normal(const r3d_primitive *p, r3d_vec3 point, r3d_ray ray) {
    switch (p->kind) {
        case R3D_SPHERE: return r3d_normalize(r3d_sub(point, p->position));
        case R3D_CAPSULE: {
            r3d_vec3 a, b;
            r3d_capsule_segment(p, &a, &b);
            r3d_vec3 ab = r3d_sub(b, a);
            float len2 = r3d_dot(ab, ab);
            float t = len2 > 1e-12f ? r3d_clampf(r3d_dot(r3d_sub(point, a), ab) / len2, 0, 1) : 0;
            return r3d_normalize(r3d_sub(point, r3d_add(a, r3d_scale(ab, t))));
        }
        case R3D_CUBE:
        case R3D_PLANE: {
            r3d_mat4 model = r3d_primitive_matrix(p), inv, rot = r3d_mat4_rotation(p->rotation);
            if (!r3d_mat4_inverse(&model, &inv)) return r3d_neg(r3d_normalize(ray.direction));
            r3d_vec3 l = r3d_mat4_transform_point(&inv, point), n = r3d_v3(0, 1, 0);
            if (p->kind == R3D_CUBE) {
                float ax = fabsf(l.x), ay = fabsf(l.y), az = fabsf(l.z);
                n = ax >= ay && ax >= az ? r3d_v3(l.x > 0 ? 1 : -1, 0, 0) : (ay >= az ? r3d_v3(0, l.y > 0 ? 1 : -1, 0) : r3d_v3(0, 0, l.z > 0 ? 1 : -1));
            }
            r3d_vec3 w = r3d_normalize(r3d_mat4_transform_vector(&rot, n));
            return r3d_dot(w, ray.direction) > 0 ? r3d_neg(w) : w;   /* planes face the viewer */
        }
        default: return r3d_neg(r3d_normalize(ray.direction));
    }
}
bool r3d_pick_filtered(r3d_ray ray, r3d_pick_hit *hit, bool (*accept)(uint64_t id, void *user), void *user) {
    r3d_pick_hit h;
    h.id = R3D_ID_NONE;
    h.t = INFINITY;
    h.point = h.normal = r3d_v3(0, 0, 0);
    const r3d_primitive *best_prim = NULL;
    const pick_box *best_box = NULL;
    for (int i = 0; i < S.item_count; ++i) {
        const r3d_primitive *p = &S.items[i].prim;
        float t;
        if (p->id == R3D_ID_NONE || p->kind == MESH_KIND) continue;
        if (accept && !accept(p->id, user)) continue;
        if (r3d_ray_primitive(ray, p, &t) && t < h.t) { h.t = t; h.id = p->id; best_prim = p; best_box = NULL; }
    }
    int best_plane = -1;
    for (int i = 0; i < S.box_count; ++i) {
        float t;
        int plane;
        if (accept && !accept(S.boxes[i].id, user)) continue;
        bool hit_it = S.boxes[i].plane_count ? r3d_ray_convex(ray, S.boxes[i].planes, S.boxes[i].plane_count, &t, &plane) : r3d_ray_aabb(ray, S.boxes[i].lo, S.boxes[i].hi, &t);
        if (hit_it && t < h.t) { h.t = t; h.id = S.boxes[i].id; best_box = &S.boxes[i]; best_prim = NULL; best_plane = S.boxes[i].plane_count ? plane : -1; }
    }
    if (h.id != R3D_ID_NONE) {
        h.point = r3d_ray_point(ray, h.t);
        if (best_box && best_box->plane_count) {
            h.normal = best_plane >= 0 ? best_box->planes[best_plane].n : r3d_neg(r3d_normalize(ray.direction));
        } else if (best_box) {
            /* the face the hit lies on */
            float best_d = INFINITY;
            for (int axis = 0; axis < 3; ++axis) {
                float p = (&h.point.x)[axis], lo = (&best_box->lo.x)[axis], hi = (&best_box->hi.x)[axis];
                if (fabsf(p - lo) < best_d) { best_d = fabsf(p - lo); h.normal = r3d_v3(0, 0, 0); (&h.normal.x)[axis] = -1; }
                if (fabsf(p - hi) < best_d) { best_d = fabsf(p - hi); h.normal = r3d_v3(0, 0, 0); (&h.normal.x)[axis] = 1; }
            }
        } else if (best_prim) h.normal = primitive_normal(best_prim, h.point, ray);
    }
    if (hit) *hit = h;
    return h.id != R3D_ID_NONE;
}
bool r3d_pick(r3d_ray ray, r3d_pick_hit *hit) { return r3d_pick_filtered(ray, hit, NULL, NULL); }
uint64_t r3d_pick_ray(r3d_ray ray, float *t_out) {
    r3d_pick_hit h;
    r3d_pick(ray, &h);
    if (t_out) *t_out = h.t;
    return h.id;
}
uint64_t r3d_pick_at(float sx, float sy) {
    if (!S.valid || sx < S.vx || sy < S.vy || sx > S.vx + S.vw || sy > S.vy + S.vh) return R3D_ID_NONE;
    return r3d_pick_ray(r3d_screen_ray(sx, sy), NULL);
}

/* ---------------------------------------------------------------------------
 * decals: the frame's opaque triangles clipped to a box and drawn again
 * ------------------------------------------------------------------------- */
static r3d_mat4 decal_matrix(const r3d_decal *d) { return r3d_mat4_trs(d->position, d->rotation, d->size); }
void r3d_decal_corners(const r3d_decal *d, r3d_vec3 corners[8]) {
    if (!d || !corners) return;
    r3d_mat4 m = decal_matrix(d);
    for (int i = 0; i < 8; ++i) corners[i] = r3d_mat4_transform_point(&m, r3d_v3((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f));
}
void r3d_draw_decal(const r3d_decal *decal) {
    if (!S.valid || !decal || S.decal_count >= R3D_MAX_DECALS) return;
    if (!(fabsf(decal->size.x) > 1e-6f) || !(fabsf(decal->size.y) > 1e-6f) || !(fabsf(decal->size.z) > 1e-6f)) return;
    r3d_decal *d = &S.decals[S.decal_count];
    *d = *decal;
    S.decal_materials[S.decal_count] = decal->material ? *decal->material : r3d_material_default();
    d->material = &S.decal_materials[S.decal_count];
    ++S.decal_count;
    if (d->id != R3D_ID_NONE) {   /* the box picks: six planes, outward normals */
        r3d_mat4 rot = r3d_mat4_rotation(d->rotation);
        r3d_plane planes[6];
        for (int a = 0; a < 3; ++a) {
            r3d_vec3 axis = r3d_mat4_transform_vector(&rot, r3d_v3(a == 0, a == 1, a == 2));
            float half = fabsf((&d->size.x)[a]) * 0.5f, c = r3d_dot(axis, d->position);
            planes[a * 2].n = axis; planes[a * 2].d = c + half;
            planes[a * 2 + 1].n = r3d_scale(axis, -1); planes[a * 2 + 1].d = -c + half;
        }
        r3d_pick_convex(d->id, planes, 6);
    }
}
/* Sutherland-Hodgman against one face of the unit box in decal space: keeps sign * p[axis] <= 0.5. */
static int clip_polygon_face(const r3d_vec3 *in, int n, r3d_vec3 *out, int axis, float sign) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        r3d_vec3 a = in[i], b = in[(i + 1) % n];
        float da = sign * (&a.x)[axis] - 0.5f, db = sign * (&b.x)[axis] - 0.5f;
        if (da <= 0) out[m++] = a;
        if ((da <= 0) != (db <= 0)) {
            float t = da / (da - db);
            out[m++] = r3d_add(a, r3d_scale(r3d_sub(b, a), t));
        }
    }
    return m;
}
/* One world-space triangle of a surface against a decal: the clipped polygon
 * becomes decal triangles in the dynamic buffers. Returns the vertices added. */
static int decal_triangle(const r3d_mat4 *m, const r3d_mat4 *inv, r3d_vec3 up, float cos_max, r3d_vec3 a, r3d_vec3 b, r3d_vec3 c) {
    r3d_vec3 n = r3d_cross(r3d_sub(b, a), r3d_sub(c, a));
    float len = r3d_length(n);
    if (len < 1e-12f) return 0;
    n = r3d_scale(n, 1.0f / len);
    if (r3d_dot(n, up) < cos_max) return 0;   /* facing away from the projection, or too steep */
    r3d_vec3 poly[2][12];
    int count = 3;
    poly[0][0] = r3d_mat4_transform_point(inv, a);
    poly[0][1] = r3d_mat4_transform_point(inv, b);
    poly[0][2] = r3d_mat4_transform_point(inv, c);
    int cur = 0;
    for (int axis = 0; axis < 3 && count >= 3; ++axis)
        for (int side = 0; side < 2 && count >= 3; ++side) {
            count = clip_polygon_face(poly[cur], count, poly[cur ^ 1], axis, side ? -1.0f : 1.0f);
            cur ^= 1;
        }
    if (count < 3) return 0;
    if (S.dyn_vertex_count + count > S.desc.max_mesh_vertices || S.dyn_index_count + (count - 2) * 3 > S.desc.max_mesh_vertices * 3) return 0;
    uint32_t base = (uint32_t)S.dyn_vertex_count;
    for (int i = 0; i < count; ++i) {
        r3d_vec3 l = poly[cur][i], w = r3d_mat4_transform_point(m, l);
        r3d_vertex *v = &S.dyn_vertices[S.dyn_vertex_count++];
        v->x = w.x; v->y = w.y; v->z = w.z; v->cap = 0;
        v->nx = n.x; v->ny = n.y; v->nz = n.z;
        v->u = l.x + 0.5f; v->v = l.z + 0.5f;   /* the image's top (v = 0) towards -z */
        v->rgba[0] = v->rgba[1] = v->rgba[2] = v->rgba[3] = 255;
    }
    for (int i = 1; i + 1 < count; ++i) {
        S.dyn_indices[S.dyn_index_count++] = base;
        S.dyn_indices[S.dyn_index_count++] = base + (uint32_t)i;
        S.dyn_indices[S.dyn_index_count++] = base + (uint32_t)i + 1;
    }
    return count;
}
/* Every decal against every opaque item drawn so far: the projections become
 * mesh items of their own (blended, drawn after the opaque shapes). Before
 * the dynamic buffers go up. */
static void build_decals(void) {
    int items = S.item_count;   /* the decals' own items are appended past these */
    for (int k = 0; k < S.decal_count && S.item_count < S.desc.max_primitives; ++k) {
        const r3d_decal *d = &S.decals[k];
        r3d_mat4 m = decal_matrix(d), inv;
        if (!r3d_mat4_inverse(&m, &inv)) continue;
        r3d_vec3 corners[8], lo = r3d_v3(INFINITY, INFINITY, INFINITY), hi = r3d_v3(-INFINITY, -INFINITY, -INFINITY);
        r3d_decal_corners(d, corners);
        for (int i = 0; i < 8; ++i) {
            lo = r3d_v3(r3d_minf(lo.x, corners[i].x), r3d_minf(lo.y, corners[i].y), r3d_minf(lo.z, corners[i].z));
            hi = r3d_v3(r3d_maxf(hi.x, corners[i].x), r3d_maxf(hi.y, corners[i].y), r3d_maxf(hi.z, corners[i].z));
        }
        r3d_vec3 up = r3d_normalize(r3d_mat4_transform_vector(&m, r3d_v3(0, 1, 0)));
        float cos_max = cosf(d->max_angle > 0 ? r3d_clampf(d->max_angle, 0.01f, R3D_PI * 0.5f - 0.01f) : 80.0f * R3D_PI / 180.0f);
        int first_index = S.dyn_index_count;
        for (int n = 0; n < items; ++n) {
            const item *it = &S.items[n];
            if (it->translucent || it->prim.wireframe || it->decal) continue;
            if (it->lo.x > hi.x || it->hi.x < lo.x || it->lo.y > hi.y || it->hi.y < lo.y || it->lo.z > hi.z || it->hi.z < lo.z) continue;
            if (it->prim.kind == MESH_KIND) {
                for (int i = it->index_base; i + 2 < it->index_base + it->index_count; i += 3) {
                    const r3d_vertex *a = &S.dyn_vertices[S.dyn_indices[i]], *b = &S.dyn_vertices[S.dyn_indices[i + 1]], *c = &S.dyn_vertices[S.dyn_indices[i + 2]];
                    decal_triangle(&m, &inv, up, cos_max, r3d_v3(a->x, a->y, a->z), r3d_v3(b->x, b->y, b->z), r3d_v3(c->x, c->y, c->z));
                }
            } else {
                const r3d_mesh *mesh = &S.meshes[it->prim.kind].cpu;
                float radius = fabsf(it->prim.size.x);
                float cap = it->prim.kind == R3D_CAPSULE && radius > 1e-6f ? r3d_maxf(it->prim.size.y, 0) * 0.5f / radius : 0;
                for (size_t i = 0; i + 2 < mesh->index_count; i += 3) {
                    r3d_vec3 w[3];
                    for (int e = 0; e < 3; ++e) {
                        const r3d_vertex *v = &mesh->vertices[mesh->indices[i + e]];
                        w[e] = r3d_mat4_transform_point(&it->model, r3d_v3(v->x, v->y + v->cap * cap, v->z));
                    }
                    decal_triangle(&m, &inv, up, cos_max, w[0], w[1], w[2]);
                }
            }
        }
        if (S.dyn_index_count == first_index) continue;   /* touched nothing */
        item *it = &S.items[S.item_count++];
        memset(it, 0, sizeof(*it));
        it->prim.kind = MESH_KIND;
        it->prim.color = d->color;
        it->prim.id = R3D_ID_NONE;
        it->material = *d->material;
        it->model = r3d_mat4_identity();
        it->translucent = it->decal = true;
        it->depth = r3d_dot(r3d_sub(d->position, S.camera.position), r3d_camera_forward(&S.camera));
        it->index_base = first_index;
        it->index_count = S.dyn_index_count - first_index;
        it->lo = lo; it->hi = hi;
        ++S.pending.decals;
        S.pending.decal_triangles += it->index_count / 3;
    }
}

/* ---------------------------------------------------------------------------
 * rendering
 * ------------------------------------------------------------------------- */
static int compare_items(const void *a, const void *b) {
    const item *x = &S.items[*(const int *)a], *y = &S.items[*(const int *)b];
    if (x->translucent != y->translucent) return x->translucent ? 1 : -1;
    if (x->decal != y->decal) return x->decal ? -1 : 1;   /* decals right after the opaque shapes, in the order drawn */
    if (x->decal) return *(const int *)a - *(const int *)b;
    if (x->translucent) return x->depth > y->depth ? -1 : (x->depth < y->depth);   /* back to front */
    if (x->prim.wireframe != y->prim.wireframe) return x->prim.wireframe ? 1 : -1;
    if (x->prim.kind != y->prim.kind) return (int)x->prim.kind - (int)y->prim.kind;
    return 0;
}
static sg_pipeline pipeline_for(const item *it) {
    if (it->prim.wireframe) return S.pip_wire;
    if (it->decal) return S.pip_decal;
    if (it->translucent) return S.pip_blend;
    return it->prim.kind == R3D_PLANE ? S.pip_solid_twosided : S.pip_solid;
}
/* The frame's lighting as the fragment shader wants it. */
static void fill_lighting(mesh_fs_uniforms *fs) {
    r3d_light rig[R3D_MAX_LIGHTS];
    const r3d_light *lights = S.lights;
    int count = S.light_count;
    float ambient[3] = {S.ambient[0], S.ambient[1], S.ambient[2]};
    if (!count && S.default_lighting) {
        /* a key light from over the camera's shoulder and a soft ambient */
        r3d_vec3 f = r3d_camera_forward(&S.camera), u = r3d_camera_up(&S.camera), r = r3d_camera_right(&S.camera);
        memset(rig, 0, sizeof(rig));
        rig[0].kind = R3D_LIGHT_DIRECTIONAL;
        rig[0].direction = r3d_normalize(r3d_add(r3d_scale(f, 0.8f), r3d_add(r3d_scale(u, -0.7f), r3d_scale(r, 0.35f))));
        rig[0].color = r3d_rgb(255, 250, 240);
        rig[0].intensity = 0.85f;
        lights = rig;
        count = 1;
        ambient[0] = 0.34f; ambient[1] = 0.36f; ambient[2] = 0.42f;
    }
    fs->ambient[0] = ambient[0]; fs->ambient[1] = ambient[1]; fs->ambient[2] = ambient[2]; fs->ambient[3] = (float)count;
    fs->camera[0] = S.camera.position.x; fs->camera[1] = S.camera.position.y; fs->camera[2] = S.camera.position.z; fs->camera[3] = 0;
    memset(fs->shadow_params, 0, sizeof(fs->shadow_params));
    memset(fs->local_params, 0, sizeof(fs->local_params));
    memset(fs->lights, 0, sizeof(fs->lights));
    for (int i = 0; i < count; ++i) {
        const r3d_light *l = &lights[i];
        float *a = fs->lights[i * 4], *b = fs->lights[i * 4 + 1], *c = fs->lights[i * 4 + 2];
        a[0] = l->position.x; a[1] = l->position.y; a[2] = l->position.z; a[3] = l->range > 0 ? l->range : 1e-4f;
        b[0] = l->direction.x; b[1] = l->direction.y; b[2] = l->direction.z;
        b[3] = l->kind == R3D_LIGHT_SPOT ? cosf(r3d_clampf(l->cone, 0.01f, R3D_PI) * 0.5f) : -2.0f;
        c[0] = l->color.r / 255.0f * l->intensity; c[1] = l->color.g / 255.0f * l->intensity; c[2] = l->color.b / 255.0f * l->intensity;
        c[3] = (float)l->kind;
    }
}
/* Renders the opaque geometry from a light into one shadow target. Shapes
 * enclosing `inside` (a point or spot light's bulb) cast no shadow: they
 * would darken everything. */
static void render_shadow_geometry(const r3d_mat4 *vp, const r3d_vec3 *inside) {
    int bound_mesh = -2;
    for (int n = 0; n < S.item_count; ++n) {
        const item *it = &S.items[n];
        if (it->translucent || it->prim.wireframe) continue;
        if (inside && inside->x >= it->lo.x && inside->x <= it->hi.x && inside->y >= it->lo.y && inside->y <= it->hi.y &&
            inside->z >= it->lo.z && inside->z <= it->hi.z) continue;
        const gpu_mesh *g = it->prim.kind == MESH_KIND ? NULL : &S.meshes[it->prim.kind];
        if ((int)it->prim.kind != bound_mesh) {
            sg_bindings bind;
            memset(&bind, 0, sizeof(bind));
            bind.vertex_buffers[0] = g ? g->vertices : S.mesh_vertices;
            bind.index_buffer = g ? g->indices : S.mesh_indices;
            sg_apply_bindings(&bind);
            bound_mesh = (int)it->prim.kind;
        }
        shadow_vs_uniforms vs;
        r3d_mat4 mvp = r3d_mat4_multiply(*vp, it->model);
        memcpy(vs.mvp, mvp.m, sizeof(vs.mvp));
        float r = fabsf(it->prim.size.x);
        vs.shape[0] = it->prim.kind == R3D_CAPSULE && r > 1e-6f ? r3d_maxf(it->prim.size.y, 0) * 0.5f / r : 0;
        vs.shape[1] = vs.shape[2] = vs.shape[3] = 0;
        sg_range vr = {&vs, sizeof(vs)};
        sg_apply_uniforms(0, &vr);
        if (g) sg_draw(0, g->index_count, 1); else sg_draw(it->index_base, it->index_count, 1);
        ++S.pending.draw_calls;
    }
}
/* clip space -> map uv (rows start at the bottom on OpenGL) and depth */
static r3d_mat4 shadow_bias(void) {
    r3d_mat4 bias = r3d_mat4_identity();
    bias.m[0] = 0.5f; bias.m[12] = 0.5f;
    bias.m[5] = S.origin_top_left ? -0.5f : 0.5f; bias.m[13] = 0.5f;
    return bias;
}
/* One spot/point shadow layer: a perspective pass from `eye` along `dir`. */
static void local_shadow_pass(int layer, r3d_vec3 eye, r3d_vec3 dir, r3d_vec3 up, float fov, float range, mesh_fs_uniforms *lighting) {
    r3d_mat4 view = r3d_mat4_look_at(eye, r3d_add(eye, dir), up);
    r3d_mat4 proj = r3d_mat4_perspective(fov, 1, 0.05f, range > 0.1f ? range : 0.1f, true);
    r3d_mat4 vp = r3d_mat4_multiply(proj, view);
    r3d_mat4 m = r3d_mat4_multiply(shadow_bias(), vp);
    memcpy(lighting->local_matrices[layer], m.m, sizeof(m.m));
    sg_pass pass;
    memset(&pass, 0, sizeof(pass));
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value.r = 1.0f;
    pass.action.depth.load_action = SG_LOADACTION_CLEAR;
    pass.action.depth.clear_value = 1.0f;
    pass.attachments.colors[0] = S.local_attachments[layer];
    pass.attachments.depth_stencil = S.local_depth_attachment;
    pass.label = "r3d-shadow-layer";
    sg_begin_pass(&pass);
    sg_apply_pipeline(S.pip_shadow_local);
    render_shadow_geometry(&vp, &eye);
    sg_end_pass();
    ++S.pending.local_shadow_passes;
}
/* The spot and point shadows of the frame: the first shadowing spots and point lights get layers. */
static void local_shadow_passes(mesh_fs_uniforms *lighting) {
    if (!S.shadows || !S.shadow_ok || !S.scene_bounded) return;
    int spots = 0, points = 0;
    lighting->local_params[0] = 1.0f / (float)S.local_size;
    lighting->local_params[1] = 0.002f;
    for (int i = 0; i < S.light_count; ++i) {
        const r3d_light *l = &S.lights[i];
        if (!l->shadows) continue;
        float *sh = lighting->lights[i * 4 + 3];
        if (l->kind == R3D_LIGHT_SPOT && spots < R3D_MAX_SPOT_SHADOWS) {
            int layer = spots++;
            r3d_vec3 up = fabsf(l->direction.y) < 0.95f ? r3d_v3(0, 1, 0) : r3d_v3(1, 0, 0);
            float fov = r3d_clampf(l->cone, 0.05f, 3.0f) + 0.2f;   /* a little beyond the cone's soft edge */
            if (fov > 3.1f) fov = 3.1f;
            local_shadow_pass(layer, l->position, l->direction, up, fov, l->range, lighting);
            sh[0] = 1; sh[1] = (float)layer;
        } else if (l->kind == R3D_LIGHT_POINT && points < R3D_MAX_POINT_SHADOWS) {
            int base = R3D_MAX_SPOT_SHADOWS + points * 6;
            ++points;
            static const r3d_vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            static const r3d_vec3 ups[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}};
            for (int f = 0; f < 6; ++f) local_shadow_pass(base + f, l->position, dirs[f], ups[f], R3D_PI * 0.5f + 0.02f, l->range, lighting);
            sh[0] = 2; sh[1] = (float)base;
        }
    }
}
static void draw_sky(void) {
    if (!S.sky_set || !S.pip_sky.id) return;
    r3d_mat4 inv;
    if (!r3d_mat4_inverse(&S.view_proj, &inv)) return;
    sky_vs_uniforms vs;
    memcpy(vs.inv_view_proj, inv.m, sizeof(vs.inv_view_proj));
    vs.camera[0] = S.camera.position.x; vs.camera[1] = S.camera.position.y; vs.camera[2] = S.camera.position.z; vs.camera[3] = 0;
    sky_fs_uniforms fs;
    memset(&fs, 0, sizeof(fs));
    const r3d_sky *k = &S.sky;
    fs.zenith[0] = k->zenith.r / 255.0f; fs.zenith[1] = k->zenith.g / 255.0f; fs.zenith[2] = k->zenith.b / 255.0f;
    fs.horizon[0] = k->horizon.r / 255.0f; fs.horizon[1] = k->horizon.g / 255.0f; fs.horizon[2] = k->horizon.b / 255.0f;
    fs.ground[0] = k->ground.r / 255.0f; fs.ground[1] = k->ground.g / 255.0f; fs.ground[2] = k->ground.b / 255.0f;
    /* the sun follows the first directional light */
    r3d_vec3 sun_dir = r3d_v3(0, -1, 0);
    r3d_color sun_color = r3d_rgb(255, 250, 235);
    for (int i = 0; i < S.light_count; ++i)
        if (S.lights[i].kind == R3D_LIGHT_DIRECTIONAL) { sun_dir = S.lights[i].direction; sun_color = S.lights[i].color; break; }
    fs.sun[0] = sun_dir.x; fs.sun[1] = sun_dir.y; fs.sun[2] = sun_dir.z; fs.sun[3] = k->sun_size;
    fs.sun_color[0] = sun_color.r / 255.0f * k->sun_intensity; fs.sun_color[1] = sun_color.g / 255.0f * k->sun_intensity; fs.sun_color[2] = sun_color.b / 255.0f * k->sun_intensity;
    fs.sun_color[3] = k->cube.view.id ? 1.0f : 0.0f;
    sg_apply_pipeline(S.pip_sky);
    sg_bindings bind;
    memset(&bind, 0, sizeof(bind));
    bind.vertex_buffers[0] = S.sky_quad;
    bind.views[0] = k->cube.view.id ? k->cube.view : S.black_cube.view;
    bind.samplers[0] = S.samplers[0];
    sg_apply_bindings(&bind);
    sg_range vr = {&vs, sizeof(vs)}, fr = {&fs, sizeof(fs)};
    sg_apply_uniforms(0, &vr);
    sg_apply_uniforms(1, &fr);
    sg_draw(0, 6, 1);
    ++S.pending.draw_calls;
}
/* Draws the sorted items order[from .. to). */
static void draw_items(int from, int to, const mesh_fs_uniforms *lighting) {
    uint32_t bound_pipeline = 0, bound_texture = 0, bound_sampler = 0;
    int bound_mesh = -1;
    bool bound_wire = false;
    for (int n = from; n < to; ++n) {
        const item *it = &S.items[S.order[n]];
        sg_pipeline pip = pipeline_for(it);
        if (pip.id != bound_pipeline) { sg_apply_pipeline(pip); bound_pipeline = pip.id; bound_mesh = -1; }
        const gpu_mesh *g = it->prim.kind == MESH_KIND ? NULL : &S.meshes[it->prim.kind];
        sg_view texture = it->material.texture.view.id ? it->material.texture.view : S.white.view;
        sg_sampler sampler = S.samplers[(it->material.nearest ? 2 : 0) + (it->material.repeat ? 1 : 0)];
        if ((int)it->prim.kind != bound_mesh || it->prim.wireframe != bound_wire || texture.id != bound_texture || sampler.id != bound_sampler) {
            sg_bindings bind;
            memset(&bind, 0, sizeof(bind));
            if (g) {
                bind.vertex_buffers[0] = g->vertices;
                bind.index_buffer = it->prim.wireframe ? g->edges : g->indices;
            } else {
                bind.vertex_buffers[0] = S.mesh_vertices;
                bind.index_buffer = S.mesh_indices;
            }
            bind.views[0] = texture;
            bind.samplers[0] = sampler;
            bind.views[1] = S.shadow_ok ? S.shadow_texture : S.white.view;
            bind.samplers[1] = S.shadow_ok ? S.shadow_sampler : S.samplers[2];
            bind.views[2] = S.shadow_ok ? S.local_texture : S.white.view;
            sg_apply_bindings(&bind);
            bound_mesh = (int)it->prim.kind;
            bound_wire = it->prim.wireframe;
            bound_texture = texture.id;
            bound_sampler = sampler.id;
        }
        mesh_vs_uniforms vs;
        r3d_mat4 mvp = r3d_mat4_multiply(S.view_proj, it->model);
        memcpy(vs.mvp, mvp.m, sizeof(vs.mvp));
        memcpy(vs.model, it->model.m, sizeof(vs.model));
        float radius = fabsf(it->prim.size.x);
        vs.shape[0] = it->prim.kind == R3D_CAPSULE && radius > 1e-6f ? r3d_maxf(it->prim.size.y, 0) * 0.5f / radius : 0;
        vs.shape[1] = vs.shape[2] = vs.shape[3] = 0;
        vs.uvt[0] = it->material.tiling[0]; vs.uvt[1] = it->material.tiling[1]; vs.uvt[2] = it->material.offset[0]; vs.uvt[3] = it->material.offset[1];
        mesh_fs_uniforms fs = *lighting;
        const r3d_material *m = &it->material;
        fs.tint[0] = it->prim.color.r / 255.0f * m->tint[0]; fs.tint[1] = it->prim.color.g / 255.0f * m->tint[1];
        fs.tint[2] = it->prim.color.b / 255.0f * m->tint[2]; fs.tint[3] = it->prim.color.a / 255.0f * m->tint[3];
        fs.specular[0] = m->specular[0]; fs.specular[1] = m->specular[1]; fs.specular[2] = m->specular[2]; fs.specular[3] = m->shininess >= 1 ? m->shininess : 1;
        fs.emissive[0] = m->emissive[0]; fs.emissive[1] = m->emissive[1]; fs.emissive[2] = m->emissive[2]; fs.emissive[3] = m->unlit || it->prim.wireframe ? 1.0f : 0.0f;
        fs.params[0] = it->prim.checker > 0 ? it->prim.checker : 0;
        fs.params[1] = (float)S.view_mode;
        fs.params[2] = m->texture.view.id ? 1.0f : 0.0f;
        fs.params[3] = 0;
        sg_range vr = {&vs, sizeof(vs)}, fr = {&fs, sizeof(fs)};
        sg_apply_uniforms(0, &vr);
        sg_apply_uniforms(1, &fr);
        if (g) sg_draw(0, it->prim.wireframe ? g->edge_count : g->index_count, 1);
        else sg_draw(it->index_base, it->index_count, 1);
        ++S.pending.draw_calls;
    }
}
/* The shadow light of the frame: the first directional light that casts, or -1. */
static int shadow_light_index(void) {
    if (!S.shadows || !S.shadow_ok || !S.scene_bounded) return -1;
    for (int i = 0; i < S.light_count; ++i)
        if (S.lights[i].kind == R3D_LIGHT_DIRECTIONAL && S.lights[i].shadows) return i;
    return -1;
}
/* Renders the opaque geometry into the shadow map from the light; fills the
 * fragment uniforms' shadow fields. Its own pass: before the scene pass. */
static void shadow_pass(int light, mesh_fs_uniforms *lighting) {
    r3d_vec3 center = r3d_scale(r3d_add(S.scene_lo, S.scene_hi), 0.5f);
    float radius = r3d_length(r3d_sub(S.scene_hi, S.scene_lo)) * 0.5f + 0.5f;
    r3d_vec3 dir = r3d_normalize(S.lights[light].direction);
    r3d_vec3 eye = r3d_sub(center, r3d_scale(dir, radius * 2));
    r3d_vec3 up = fabsf(dir.y) < 0.95f ? r3d_v3(0, 1, 0) : r3d_v3(1, 0, 0);
    r3d_mat4 view = r3d_mat4_look_at(eye, center, up);
    r3d_mat4 proj = r3d_mat4_orthographic(-radius, radius, -radius, radius, 0.01f, radius * 4, true);   /* depth 0..1 on every backend */
    r3d_mat4 vp = r3d_mat4_multiply(proj, view);
    r3d_mat4 shadow_matrix = r3d_mat4_multiply(shadow_bias(), vp);
    memcpy(lighting->shadow_matrix, shadow_matrix.m, sizeof(lighting->shadow_matrix));
    lighting->shadow_params[0] = 1;
    lighting->shadow_params[1] = 1.0f / (float)S.shadow_size;
    lighting->shadow_params[2] = 0.0015f;
    lighting->shadow_params[3] = (float)light;

    sg_pass pass;
    memset(&pass, 0, sizeof(pass));
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value.r = 1.0f;   /* far */
    pass.action.depth.load_action = SG_LOADACTION_CLEAR;
    pass.action.depth.clear_value = 1.0f;
    pass.attachments.colors[0] = S.shadow_attachment;
    pass.attachments.depth_stencil = S.shadow_depth_attachment;
    pass.label = "r3d-shadow";
    sg_begin_pass(&pass);
    sg_apply_pipeline(S.pip_shadow);
    render_shadow_geometry(&vp, NULL);
    sg_end_pass();
    S.pending.shadow_pass = true;
}
static void upload_meshes(void) {
    if (!S.dyn_vertex_count) return;
    sg_write_buffer_desc wb;
    memset(&wb, 0, sizeof(wb));
    wb.src.data.ptr = S.dyn_vertices;
    wb.src.data.size = sizeof(r3d_vertex) * (size_t)S.dyn_vertex_count;
    wb.dst.buffer = S.mesh_vertices;
    sg_write_buffer_transient(&wb);
    memset(&wb, 0, sizeof(wb));
    wb.src.data.ptr = S.dyn_indices;
    wb.src.data.size = sizeof(uint32_t) * (size_t)S.dyn_index_count;
    wb.dst.buffer = S.mesh_indices;
    sg_write_buffer_transient(&wb);
}
/* Draws the depth-tested lines (overlay = false) or the overlay lines; the
 * vertex buffer is uploaded once, before the first of the two. */
static void draw_lines(bool overlay) {
    int total = S.line_count + S.overlay_count;
    if (!total) return;
    if (!overlay) {   /* both blocks go up before the buffer is first bound */
        sg_write_buffer_desc wb;
        memset(&wb, 0, sizeof(wb));
        wb.dst.buffer = S.line_buffer;
        if (S.line_count) {
            wb.src.data.ptr = S.lines;
            wb.src.data.size = sizeof(line_vertex) * (size_t)S.line_count;
            sg_write_buffer_transient(&wb);
        }
        if (S.overlay_count) {
            wb.src.data.ptr = S.overlay;
            wb.src.data.size = sizeof(line_vertex) * (size_t)S.overlay_count;
            wb.dst.offset = sizeof(line_vertex) * (size_t)S.line_count;
            sg_write_buffer_transient(&wb);
        }
    }
    int base = overlay ? S.line_count : 0, count = overlay ? S.overlay_count : S.line_count;
    if (!count) return;
    sg_bindings bind;
    memset(&bind, 0, sizeof(bind));
    bind.vertex_buffers[0] = S.line_buffer;
    sg_range mvp = {S.view_proj.m, sizeof(S.view_proj.m)};
    sg_apply_pipeline(overlay ? S.pip_lines_overlay : S.pip_lines);
    sg_apply_bindings(&bind);
    sg_apply_uniforms(0, &mvp);
    sg_draw(base, count, 1);
    ++S.pending.draw_calls;
}
void r3d_render(void) {
    if (!S.valid || S.rendered) return;
    int w = (int)(S.vw * S.dpi + 0.5f), h = (int)(S.vh * S.dpi + 0.5f);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if ((w != S.target_w || h != S.target_h) && !make_target(w, h)) {
        fputs("render3d: cannot create the 3D target\n", stderr);
        return;
    }
    update_matrices();
    mesh_fs_uniforms lighting;
    memset(&lighting, 0, sizeof(lighting));
    fill_lighting(&lighting);
    build_decals();    /* their triangles join the dynamic buffers */
    upload_meshes();   /* before the first pass binds the dynamic buffers */
    int shadow_light = shadow_light_index();
    if (shadow_light >= 0) shadow_pass(shadow_light, &lighting);
    local_shadow_passes(&lighting);
    sg_pass pass;
    memset(&pass, 0, sizeof(pass));
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value.r = S.desc.background.r / 255.0f;
    pass.action.colors[0].clear_value.g = S.desc.background.g / 255.0f;
    pass.action.colors[0].clear_value.b = S.desc.background.b / 255.0f;
    pass.action.colors[0].clear_value.a = S.desc.background.a / 255.0f;
    pass.action.depth.load_action = SG_LOADACTION_CLEAR;
    pass.action.depth.clear_value = 1.0f;
    pass.attachments.colors[0] = S.color_attachment;
    pass.attachments.depth_stencil = S.depth_attachment;
    if (S.samples > 1) {
        pass.attachments.resolves[0] = S.resolve_attachment;
        pass.action.colors[0].store_action = SG_STOREACTION_DONTCARE;
    }
    pass.label = "r3d-scene";
    sg_begin_pass(&pass);
    /* opaque and wireframe shapes, the depth-tested lines, the translucent
     * shapes back to front, then the overlay lines */
    for (int i = 0; i < S.item_count; ++i) S.order[i] = i;
    qsort(S.order, (size_t)S.item_count, sizeof(int), compare_items);
    int split = S.item_count;
    for (int n = 0; n < S.item_count; ++n) if (S.items[S.order[n]].translucent) { split = n; break; }
    draw_items(0, split, &lighting);
    draw_sky();   /* behind the opaque shapes, before the translucent ones blend over it */
    draw_lines(false);
    draw_items(split, S.item_count, &lighting);
    draw_lines(true);
    sg_end_pass();
    S.rendered = true;
    int meshes = 0;
    for (int i = 0; i < S.item_count; ++i) if (S.items[i].prim.kind == MESH_KIND) ++meshes;
    S.pending.meshes = meshes;
    S.pending.mesh_vertices = S.dyn_vertex_count;
    S.pending.lights = (int)lighting.ambient[3];
    S.pending.primitives = S.item_count - meshes;
    S.pending.lines = (S.line_count + S.overlay_count) / 2;
    S.pending.target_width = S.target_w;
    S.pending.target_height = S.target_h;
    S.pending.sample_count = S.samples;
    S.stats = S.pending;
}
static sg_pipeline composite_pipeline_for(sg_pixel_format color, sg_pixel_format depth, int samples) {
    for (int i = 0; i < S.composite_count; ++i) {
        composite_pipeline *c = &S.composites[i];
        if (c->color == color && c->depth == depth && c->samples == samples) return c->pipeline;
    }
    if (S.composite_count == MAX_COMPOSITE_PIPELINES) {
        sg_destroy_pipeline(S.composites[0].pipeline);
        memmove(S.composites, S.composites + 1, sizeof(S.composites[0]) * (MAX_COMPOSITE_PIPELINES - 1));
        --S.composite_count;
    }
    sg_pipeline_desc p;
    memset(&p, 0, sizeof(p));
    p.shader = S.composite_shader;
    p.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    p.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
    p.colors[0].pixel_format = color;
    p.depth.pixel_format = depth;
    p.depth.write_enabled = false;
    p.sample_count = samples > 0 ? samples : 1;
    p.colors[0].blend.enabled = true;
    p.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    p.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    p.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    p.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    p.label = "r3d-composite";
    composite_pipeline *c = &S.composites[S.composite_count++];
    c->color = color; c->depth = depth; c->samples = samples;
    c->pipeline = sg_make_pipeline(&p);
    return c->pipeline;
}
void r3d_composite(sg_pixel_format color_format, sg_pixel_format depth_format, int sample_count, float pass_w, float pass_h) {
    if (!S.valid || !S.rendered || !S.color_texture.id || pass_w <= 0 || pass_h <= 0) return;
    if (!S.quad_written) {
        float x0 = S.vx / pass_w * 2 - 1, x1 = (S.vx + S.vw) / pass_w * 2 - 1;
        float y0 = 1 - S.vy / pass_h * 2, y1 = 1 - (S.vy + S.vh) / pass_h * 2;   /* y0 is the top edge */
        float vt = S.origin_top_left ? 0.0f : 1.0f, vb = 1.0f - vt;
        quad_vertex q[6] = {{x0, y0, 0, vt}, {x1, y0, 1, vt}, {x1, y1, 1, vb}, {x0, y0, 0, vt}, {x1, y1, 1, vb}, {x0, y1, 0, vb}};
        sg_write_buffer_desc wb;
        memset(&wb, 0, sizeof(wb));
        wb.src.data.ptr = q;
        wb.src.data.size = sizeof(q);
        wb.dst.buffer = S.quad_buffer;
        sg_write_buffer_transient(&wb);
        S.quad_written = true;
    }
    sg_apply_pipeline(composite_pipeline_for(color_format, depth_format, sample_count));
    sg_bindings bind;
    memset(&bind, 0, sizeof(bind));
    bind.vertex_buffers[0] = S.quad_buffer;
    bind.views[0] = S.color_texture;
    bind.samplers[0] = S.sampler;
    sg_apply_bindings(&bind);
    sg_draw(0, 6, 1);
}
void r3d_present(void) {
    if (!S.valid) return;
    if (!S.rendered) r3d_render();
    sg_environment env = sglue_environment();
    sg_pass pass;
    memset(&pass, 0, sizeof(pass));
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value.r = S.desc.background.r / 255.0f;
    pass.action.colors[0].clear_value.g = S.desc.background.g / 255.0f;
    pass.action.colors[0].clear_value.b = S.desc.background.b / 255.0f;
    pass.action.colors[0].clear_value.a = 1.0f;
    pass.swapchain = sglue_swapchain();
    pass.label = "r3d-present";
    sg_begin_pass(&pass);
    r3d_composite(env.defaults.color_format, env.defaults.depth_format, sapp_sample_count(), sapp_widthf() / S.dpi, sapp_heightf() / S.dpi);
    sg_end_pass();
    sg_commit();
}
r3d_stats r3d_get_stats(void) { return S.stats; }
