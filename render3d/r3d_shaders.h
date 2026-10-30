/* Shader sources of render3d, one set per Sokol backend (GLSL 4.10, HLSL 5,
 * MSL). Three programs:
 *   mesh       lit surfaces (Phong): position (xyz + capsule cap sign),
 *              normal, uv, colour, one texture; the material, the ambient
 *              term and up to eight lights as uniforms (r3d_shaders.c)
 *   shadow     the shadow pass: position only, writes the light-space depth
 *              (0..1) into an R32F target; vertex uniforms mvp, shape
 *   sky        a gradient or cube-map sky on a fullscreen quad at the far plane
 *   line       coloured lines: position, colour; vertex uniform mvp
 *   composite  a textured quad in clip space: position, uv; one texture
 */
#ifndef R3D_SHADERS_H
#define R3D_SHADERS_H

extern const char *const r3d_mesh_vs_glsl, *const r3d_mesh_fs_glsl;
extern const char *const r3d_mesh_vs_hlsl, *const r3d_mesh_fs_hlsl;
extern const char *const r3d_mesh_vs_msl, *const r3d_mesh_fs_msl;

extern const char *const r3d_line_vs_glsl, *const r3d_line_fs_glsl;
extern const char *const r3d_line_vs_hlsl, *const r3d_line_fs_hlsl;
extern const char *const r3d_line_vs_msl, *const r3d_line_fs_msl;

extern const char *const r3d_shadow_vs_glsl, *const r3d_shadow_fs_glsl;
extern const char *const r3d_shadow_vs_hlsl, *const r3d_shadow_fs_hlsl;
extern const char *const r3d_shadow_vs_msl, *const r3d_shadow_fs_msl;

extern const char *const r3d_sky_vs_glsl, *const r3d_sky_fs_glsl;
extern const char *const r3d_sky_vs_hlsl, *const r3d_sky_fs_hlsl;
extern const char *const r3d_sky_vs_msl, *const r3d_sky_fs_msl;

extern const char *const r3d_composite_vs_glsl, *const r3d_composite_fs_glsl;
extern const char *const r3d_composite_vs_hlsl, *const r3d_composite_fs_hlsl;
extern const char *const r3d_composite_vs_msl, *const r3d_composite_fs_msl;

#endif
