/* Fullscreen post-process shaders for r2d_post, one per Sokol backend.
 *
 * All three implement the same program: the vertex stage turns a clip-space
 * triangle pair into uv (flipped on the top-left-origin backends D3D11 and
 * Metal so that sampling a render target always reads the right rows), the
 * fragment stage runs one pass kind
 * selected by params[0].x:
 *   0 blit              1 bloom threshold     2 gaussian blur (p1.xy = dir * radius texels)
 *   3 bloom combine     4 dof combine         5 motion blur
 *   6 chromatic         7 tonemap             8 vignette          9 shake
 *   10 lighting composite (tex1 = direct lightmap, tex2 = volumetric lightmap;
 *      params[1] camera, [2] viewport/time/fog scale, [3] fog density/contrast/wind,
 *      [4] fog color + ambient, [5] octaves/direct/haze/enabled)
 * Uniforms: float4 params[8]. params[0] = (kind, texel.x, texel.y, flip)
 * where flip = 1 when the backend's texture origin is top-left; `suv` is
 * the screen position with y growing downwards on every backend so that
 * config coordinates mean the same thing everywhere.
 * Textures: tex0 = pass input, tex1 = auxiliary (bloom / blurred copy / direct
 * light), tex2 = second auxiliary (volumetric light).
 */
#ifndef R2D_POST_SHADERS_H
#define R2D_POST_SHADERS_H

extern const char *const r2d_post_vs_glsl;
extern const char *const r2d_post_fs_glsl;
extern const char *const r2d_post_vs_hlsl;
extern const char *const r2d_post_fs_hlsl;
extern const char *const r2d_post_vs_msl;
extern const char *const r2d_post_fs_msl;
extern const char *const r2d_light_vs_glsl;
extern const char *const r2d_light_fs_glsl;
extern const char *const r2d_light_vs_hlsl;
extern const char *const r2d_light_fs_hlsl;
extern const char *const r2d_light_vs_msl;
extern const char *const r2d_light_fs_msl;

#endif
