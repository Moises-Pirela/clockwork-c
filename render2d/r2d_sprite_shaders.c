#define _CRT_SECURE_NO_WARNINGS 1
#include "r2d_sprite_internal.h"
#include <stdlib.h>
#include <string.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
/* The sprite shader, one vertex program per backend and one fragment
 * program assembled from a per-backend preamble, a shared effect body
 * (written in the HLSL dialect; the preambles define the few macros that
 * differ: float2 vs vec2, lerp vs mix, how a texture is sampled) and a
 * per-backend entry point. Custom GLSL shaders are the GLSL preamble, the
 * shared body, the user's material() and a custom entry point.
 *
 * Fragment uniforms: float4 params[9]:
 *   params[0] = (effect, time, texel.x, texel.y)
 *   params[1..4] = p0..p3 (the material's parameters)
 *   params[5] = (tiling.x, tiling.y, offset.x, offset.y)
 *   params[6] = (size.x, size.y, hover, press)   the quad's size in pixels and the element's interaction
 *   params[7] = (fill, since, 0, 0)              the element's fill amount and seconds since its state changed
 *   params[8] = elem                             per-element parameters (UiImage.param_*)
 * Effects: 0 sprite, 1 flash, 2 outline, 3 dissolve, 4 hue, 5 wave,
 * 6 pixelate, 7 glow, 8 rounded, 9 gradient, 10 sweep, 11 ring, 12 pulse,
 * 13 scanlines. */

const char *const r2d_sprite_vs_glsl =
    "#version 410\n"
    "uniform mat4 mvp;\n"
    "in vec2 pos;\n"
    "in vec2 uv_in;\n"
    "in vec4 color_in;\n"
    "out vec2 uv;\n"
    "out vec4 color;\n"
    "void main() { gl_Position = mvp * vec4(pos, 0.0, 1.0); uv = uv_in; color = color_in; }\n";

const char *const r2d_sprite_vs_hlsl =
    "cbuffer vs_params : register(b0) { float4x4 mvp; };\n"
    "struct vs_in { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };\n"
    "struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; };\n"
    "vs_out vs_main(vs_in i) { vs_out o; o.pos = mul(mvp, float4(i.pos, 0.0, 1.0)); o.uv = i.uv; o.color = i.color; return o; }\n";

const char *const r2d_sprite_vs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct vs_params { float4x4 mvp; };\n"
    "struct vs_in { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 color [[attribute(2)]]; };\n"
    "struct vs_out { float4 pos [[position]]; float2 uv; float4 color; };\n"
    "vertex vs_out vs_main(vs_in i [[stage_in]], constant vs_params& u [[buffer(0)]]) {\n"
    "  vs_out o; o.pos = u.mvp * float4(i.pos, 0.0, 1.0); o.uv = i.uv; o.color = i.color; return o;\n"
    "}\n";

/* --- fragment preambles: uniforms, the texture, the dialect macros --- */
const char *const r2d_sprite_fs_glsl_preamble =
    "#version 410\n"
    "#define float2 vec2\n"
    "#define float3 vec3\n"
    "#define float4 vec4\n"
    "#define lerp mix\n"
    "#define frac fract\n"
    "#define saturate(x) clamp(x, 0.0, 1.0)\n"
    "#define atan2(y, x) atan(y, x)\n"
    "#define TEXPARAMS\n"
    "#define TEXARGS\n"
    "#define SAMPLE(st) texture(tex, st)\n"
    "uniform vec4 params[9];\n"
    "uniform sampler2D tex;\n"
    "in vec2 uv;\n"
    "in vec4 color;\n"
    "out vec4 frag_color;\n"
    "vec2 texel;\n"
    "vec2 size;\n"     /* the quad in pixels */
    "vec4 state;\n"    /* hover, press, fill, seconds since the state changed */
    "vec4 elem;\n";    /* per-element parameters */

static const char *const fs_hlsl_preamble =
    "#define TEXPARAMS\n"
    "#define TEXARGS\n"
    "#define SAMPLE(st) tex.Sample(smp, st)\n"
    "cbuffer fs_params : register(b0) { float4 params[9]; };\n"
    "Texture2D<float4> tex : register(t0);\n"
    "SamplerState smp : register(s0);\n";

static const char *const fs_msl_preamble =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "#define lerp mix\n"
    "#define frac fract\n"
    "#define radians(x) ((x) * 0.017453292)\n"
    "#define TEXPARAMS , texture2d<float> tex, sampler smp\n"
    "#define TEXARGS , tex, smp\n"
    "#define SAMPLE(st) tex.sample(smp, st)\n"
    "struct fs_params { float4 params[9]; };\n"
    "struct fs_in { float2 uv; float4 color; };\n";

/* --- the shared body: helpers and the built-in effects --- */
const char *const r2d_sprite_fs_body =
    "float hash2(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }\n"
    "float vnoise(float2 p) {\n"
    "  float2 i = floor(p), f = frac(p); f = f * f * (3.0 - 2.0 * f);\n"
    "  float a = hash2(i), b = hash2(i + float2(1.0, 0.0)), c = hash2(i + float2(0.0, 1.0)), d = hash2(i + float2(1.0, 1.0));\n"
    "  return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);\n"
    "}\n"
    "float3 hue_shift(float3 c, float degrees) {\n"
    "  float a = radians(degrees); float3 k = float3(0.57735, 0.57735, 0.57735);\n"
    "  return c * cos(a) + cross(k, c) * sin(a) + k * dot(k, c) * (1.0 - cos(a));\n"
    "}\n"
    /* signed distance to a rounded box of half size b and corner radius r, p relative to its centre */
    "float rounded_box(float2 p, float2 b, float r) {\n"
    "  float2 q = abs(p) - b + float2(r, r);\n"
    "  return length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    /* the angle around the centre as 0..1, 0 at the top, clockwise */
    "float angle01(float2 p) { return frac(atan2(p.x, -p.y) / 6.2831853 + 0.5); }\n"
    "float4 apply_effect(int effect, float2 st, float2 uv, float4 color, float4 p0, float4 p1, float4 p2, float4 p3, float time,\n"
    "                    float2 texel, float2 size, float4 state, float4 elem TEXPARAMS) {\n"
    "  if (effect == 5) { float w = sin((p0.w > 0.5 ? st.x : st.y) * p0.x + time * p0.y) * p0.z; if (p0.w > 0.5) st.y += w; else st.x += w; }\n"
    "  if (effect == 6) st = (floor(st * p0.x) + 0.5) / p0.x;\n"
    "  float4 base = SAMPLE(st) * color;\n"
    "  float4 o = base;\n"
    "  float2 px = (uv - float2(0.5, 0.5)) * size;\n"   /* pixels from the quad's centre */
    "  float hover = state.x, press = state.y, fill = state.z;\n"
    "  if (effect == 1) o.rgb = lerp(base.rgb, p0.rgb, p0.a);\n"
    "  else if (effect == 2) {\n"
    "    float t = p1.x;\n"
    "    float n = max(max(SAMPLE(st + float2(texel.x * t, 0.0)).a, SAMPLE(st - float2(texel.x * t, 0.0)).a),\n"
    "                  max(SAMPLE(st + float2(0.0, texel.y * t)).a, SAMPLE(st - float2(0.0, texel.y * t)).a));\n"
    "    if (base.a < 0.5 && n > 0.5) o = float4(p0.rgb, p0.a * color.a);\n"
    "  } else if (effect == 3) {\n"
    "    float nz = vnoise(st * p0.y); float th = p0.x;\n"
    "    if (nz < th) o.a = 0.0; else if (nz < th + p0.z) o.rgb = lerp(p1.rgb, o.rgb, (nz - th) / max(p0.z, 1e-4));\n"
    "  } else if (effect == 4) {\n"
    "    o.rgb = hue_shift(o.rgb, p0.x); float l = dot(o.rgb, float3(0.299, 0.587, 0.114)); o.rgb = lerp(float3(l, l, l), o.rgb, p0.y) * p0.z;\n"
    "  } else if (effect == 7) o.rgb += p0.rgb * p0.w * base.a;\n"
    /* 8 rounded: corners cut by a signed distance, a border in its own colour, hover brightens, press darkens */
    "  else if (effect == 8) {\n"
    "    float r = min(p0.x, min(size.x, size.y) * 0.5), aa = max(p0.w, 0.5);\n"
    "    float d = rounded_box(px, size * 0.5, r);\n"
    "    float cover = 1.0 - smoothstep(-aa, aa, d);\n"
    "    float inner = 1.0 - smoothstep(-aa, aa, d + p0.y);\n"
    "    o = lerp(float4(p1.rgb, p1.a * color.a), base, inner);\n"
    "    o.rgb += float3(p0.z, p0.z, p0.z) * hover;\n"
    "    o.rgb *= 1.0 - 0.25 * press;\n"
    "    o.a *= cover;\n"
    /* 9 gradient: between two tints along an angle, optionally scrolling */
    "  } else if (effect == 9) {\n"
    "    float a = radians(p2.x); float t = dot(uv - float2(0.5, 0.5), float2(cos(a), sin(a))) + 0.5;\n"
    "    t = p2.y != 0.0 ? frac(t + time * p2.y) : saturate(t);\n"
    "    if (p2.z > 0.5) t = 1.0 - abs(t * 2.0 - 1.0);\n"   /* mirrored: A B A */
    "    o.rgb = base.rgb * lerp(p0.rgb, p1.rgb, t);\n"
    /* 10 sweep: a band of light crossing the element, brighter while hovered */
    "  } else if (effect == 10) {\n"
    "    float a = radians(p0.w); float proj = dot(uv - float2(0.5, 0.5), float2(cos(a), sin(a))) + 0.5;\n"
    "    float w = max(p0.y, 0.01); float phase = frac(time * p0.x) * (1.0 + 2.0 * w) - w;\n"
    "    float band = 1.0 - smoothstep(0.0, w, abs(proj - phase));\n"
    "    o.rgb += p1.rgb * band * p0.z * (1.0 + p1.w * hover) * base.a;\n"
    /* 11 ring: a radial progress arc (the fill) in the base colour, the rest in p1 */
    "  } else if (effect == 11) {\n"
    "    float R = min(size.x, size.y) * 0.5 - 1.0, r = length(px), aa = max(p0.z, 0.5);\n"
    "    float th = min(p0.x, R);\n"
    "    float ring = smoothstep(R - th - aa, R - th + aa, r) * (1.0 - smoothstep(R - aa, R + aa, r));\n"
    "    float ang = frac(angle01(px) - p0.y / 360.0);\n"
    "    float soft = max(0.5 / (6.2831853 * R), 0.002);\n"
    "    float on = 1.0 - smoothstep(fill - soft, fill + soft, ang);\n"
    "    if (fill >= 1.0) on = 1.0;\n"
    "    o = lerp(float4(p1.rgb, p1.a * color.a), base, on);\n"
    "    o.a *= ring;\n"
    /* 12 pulse: a glowing border that breathes, and flares on hover */
    "  } else if (effect == 12) {\n"
    "    float d = rounded_box(px, size * 0.5, min(p1.x, min(size.x, size.y) * 0.5));\n"
    "    float edge = 1.0 - smoothstep(0.0, max(p1.y, 0.5), abs(d + p1.y * 0.5));\n"
    "    float breath = 0.5 + 0.5 * sin(time * p0.w * 6.2831853);\n"
    "    float strength = p1.w * (0.4 + 0.6 * breath) + p1.z * hover;\n"
    "    o.rgb += p0.rgb * edge * strength;\n"
    "    o.a = max(o.a, edge * min(strength, 1.0) * color.a);\n"
    "    o.rgb *= 1.0 - 0.2 * press;\n"
    /* 13 scanlines: a CRT look with scrolling lines and flicker */
    "  } else if (effect == 13) {\n"
    "    float l = 0.5 + 0.5 * sin((px.y + time * p0.z) * p0.x);\n"
    "    o.rgb *= 1.0 - p0.y * l;\n"
    "    o.rgb *= 1.0 - p0.w * 0.5 * hash2(float2(floor(time * 60.0), 1.0));\n"
    "    o.rgb += p1.rgb * p1.w * hover;\n"
    "  }\n"
    "  return o;\n"
    "}\n";

/* --- entry points --- */
const char *const r2d_sprite_fs_glsl_main =
    "void main() {\n"
    "  int effect = int(params[0].x + 0.5); float time = params[0].y; texel = params[0].zw;\n"
    "  size = params[6].xy; state = vec4(params[6].zw, params[7].xy); elem = params[8];\n"
    "  vec2 st = uv * params[5].xy + params[5].zw;\n"
    "  frag_color = apply_effect(effect, st, uv, color, params[1], params[2], params[3], params[4], time, texel, size, state, elem);\n"
    "}\n";

/* a custom program: the user's material() plus this entry point */
const char *const r2d_sprite_fs_glsl_custom_main =
    "void main() {\n"
    "  float time = params[0].y; texel = params[0].zw;\n"
    "  size = params[6].xy; state = vec4(params[6].zw, params[7].xy); elem = params[8];\n"
    "  vec2 st = uv * params[5].xy + params[5].zw;\n"
    "  frag_color = material(texture(tex, st) * color, st, params[1], params[2], params[3], params[4], time);\n"
    "}\n";

static const char *const fs_hlsl_main =
    "float4 fs_main(float4 pos : SV_Position, float2 uv : TEXCOORD0, float4 color : COLOR0) : SV_Target0 {\n"
    "  int effect = int(params[0].x + 0.5); float time = params[0].y; float2 texel = params[0].zw;\n"
    "  float2 size = params[6].xy; float4 state = float4(params[6].zw, params[7].xy); float4 elem = params[8];\n"
    "  float2 st = uv * params[5].xy + params[5].zw;\n"
    "  return apply_effect(effect, st, uv, color, params[1], params[2], params[3], params[4], time, texel, size, state, elem);\n"
    "}\n";

static const char *const fs_msl_main =
    "fragment float4 fs_main(fs_in in [[stage_in]], constant fs_params& u [[buffer(0)]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {\n"
    "  int effect = int(u.params[0].x + 0.5); float time = u.params[0].y; float2 texel = u.params[0].zw;\n"
    "  float2 size = u.params[6].xy; float4 state = float4(u.params[6].zw, u.params[7].xy); float4 elem = u.params[8];\n"
    "  float2 st = in.uv * u.params[5].xy + u.params[5].zw;\n"
    "  return apply_effect(effect, st, in.uv, in.color, u.params[1], u.params[2], u.params[3], u.params[4], time, texel, size, state, elem, tex, smp);\n"
    "}\n";

static char *join(const char *a, const char *b, const char *c, const char *d, const char *e) {
    size_t n = strlen(a) + strlen(b) + strlen(c) + (d ? strlen(d) : 0) + (e ? strlen(e) : 0) + 1;
    char *s = malloc(n);
    if (!s) return NULL;
    strcpy(s, a);
    strcat(s, b);
    strcat(s, c);
    if (d) strcat(s, d);
    if (e) strcat(s, e);
    return s;
}
char *r2d_sprite__fragment_source(sg_backend backend, const char *custom) {
    switch (backend) {
        case SG_BACKEND_GLCORE:
            if (custom) return join(r2d_sprite_fs_glsl_preamble, r2d_sprite_fs_body, "\n#line 1\n", custom, r2d_sprite_fs_glsl_custom_main);
            return join(r2d_sprite_fs_glsl_preamble, r2d_sprite_fs_body, r2d_sprite_fs_glsl_main, NULL, NULL);
        case SG_BACKEND_D3D11: return join(fs_hlsl_preamble, r2d_sprite_fs_body, fs_hlsl_main, NULL, NULL);
        default: return join(fs_msl_preamble, r2d_sprite_fs_body, fs_msl_main, NULL, NULL);
    }
}

/* --- starter shaders --- */
static const char *const example_rim =
    "// A custom sprite shader. `base` is the texture sample times the vertex\n"
    "// colour, `uv` the texture coordinate, p0..p3 the material parameters,\n"
    "// `time` seconds. Also in scope: `tex` (sampler2D), `texel` (vec2),\n"
    "// `size` (the quad in pixels), `state` (hover, press, fill, seconds since\n"
    "// the element's state changed; all 0 on plain sprites) and `elem` (the\n"
    "// element's own parameters, UiImage.param_x..w), plus the helpers\n"
    "// hash2, vnoise, hue_shift, rounded_box and angle01.\n"
    "vec4 material(vec4 base, vec2 uv, vec4 p0, vec4 p1, vec4 p2, vec4 p3, float time) {\n"
    "  float rim = smoothstep(0.2, 0.5, distance(uv, vec2(0.5)));\n"
    "  vec3 tint = vec3(0.5 + 0.5 * sin(time + p0.x), 0.8, 1.0);\n"
    "  return vec4(mix(base.rgb, tint, rim * p0.y), base.a);\n"
    "}\n";
static const char *const example_button =
    "// A button: rounded corners from a signed distance (so they stay crisp at\n"
    "// any size), a vertical gradient, a light rim, and the element's state:\n"
    "// it brightens as the mouse comes over it and sinks while pressed.\n"
    "// p0.x corner radius, p0.y rim strength, p0.z hover glow.\n"
    "vec4 material(vec4 base, vec2 uv, vec4 p0, vec4 p1, vec4 p2, vec4 p3, float time) {\n"
    "  vec2 px = (uv - 0.5) * size;\n"
    "  float d = rounded_box(px, size * 0.5, 8.0 + p0.x);\n"
    "  float cover = 1.0 - smoothstep(-1.0, 1.0, d);\n"
    "  vec3 col = base.rgb * mix(1.2, 0.8, uv.y);\n"
    "  float rim = 1.0 - smoothstep(0.0, 2.5, abs(d + 1.5));\n"
    "  col = mix(col, vec3(1.0), rim * (0.25 + p0.y));\n"
    "  col += vec3(0.12, 0.14, 0.2) * (0.5 + p0.z) * state.x;\n"
    "  col *= 1.0 - 0.25 * state.y;\n"
    "  return vec4(col, base.a * cover);\n"
    "}\n";
static const char *const example_ring =
    "// A radial progress ring driven by the element's fill (UiImage.fill, or\n"
    "// whatever the game writes there): the arc in the base colour, the rest\n"
    "// dimmed, with a bright tip. p0.x thickness in pixels.\n"
    "vec4 material(vec4 base, vec2 uv, vec4 p0, vec4 p1, vec4 p2, vec4 p3, float time) {\n"
    "  vec2 px = (uv - 0.5) * size;\n"
    "  float R = min(size.x, size.y) * 0.5 - 1.0, th = max(p0.x, 2.0), r = length(px);\n"
    "  float ring = smoothstep(R - th - 1.0, R - th + 1.0, r) * (1.0 - smoothstep(R - 1.0, R + 1.0, r));\n"
    "  float ang = angle01(px), fill = state.z;\n"
    "  float on = 1.0 - smoothstep(fill - 0.003, fill + 0.003, ang);\n"
    "  float tip = 1.0 - smoothstep(0.0, 0.02, abs(ang - fill));\n"
    "  vec3 col = mix(base.rgb * 0.25, base.rgb, on) + vec3(tip * on);\n"
    "  return vec4(col, base.a * ring);\n"
    "}\n";
static const char *const example_shine =
    "// A sweeping highlight: a diagonal band of light crossing the element,\n"
    "// faster and brighter while hovered. elem.x (UiImage.param_x) adds speed.\n"
    "vec4 material(vec4 base, vec2 uv, vec4 p0, vec4 p1, vec4 p2, vec4 p3, float time) {\n"
    "  float speed = 0.4 + elem.x + 0.6 * state.x;\n"
    "  float proj = (uv.x + uv.y) * 0.5;\n"
    "  float phase = fract(time * speed) * 1.4 - 0.2;\n"
    "  float band = 1.0 - smoothstep(0.0, 0.12, abs(proj - phase));\n"
    "  vec3 col = base.rgb + vec3(1.0, 0.95, 0.8) * band * (0.35 + 0.35 * state.x);\n"
    "  return vec4(col, base.a);\n"
    "}\n";
static const char *const example_hologram =
    "// A hologram: scrolling scanlines, a little flicker, cyan tint, and an\n"
    "// edge glow that breathes. Works on flat panels and on textures.\n"
    "vec4 material(vec4 base, vec2 uv, vec4 p0, vec4 p1, vec4 p2, vec4 p3, float time) {\n"
    "  vec2 px = (uv - 0.5) * size;\n"
    "  float lines = 0.5 + 0.5 * sin((px.y - time * 30.0) * 0.9);\n"
    "  float flicker = 0.92 + 0.08 * hash2(vec2(floor(time * 24.0), 3.0));\n"
    "  vec3 col = base.rgb * vec3(0.6, 1.0, 1.1) * (1.0 - 0.35 * lines) * flicker;\n"
    "  float d = rounded_box(px, size * 0.5, 4.0);\n"
    "  float edge = 1.0 - smoothstep(0.0, 3.0, abs(d + 1.5));\n"
    "  col += vec3(0.3, 0.9, 1.0) * edge * (0.5 + 0.5 * sin(time * 2.0)) * 0.8;\n"
    "  return vec4(col, base.a * (0.85 + 0.15 * lines));\n"
    "}\n";
static const char *const example_bar =
    "// A health bar: the fill (UiImage.fill) from the left with a soft edge,\n"
    "// the colour sliding from green to red as it empties, an animated stripe\n"
    "// pattern inside, and a dark track behind. Set fill_mode to none.\n"
    "vec4 material(vec4 base, vec2 uv, vec4 p0, vec4 p1, vec4 p2, vec4 p3, float time) {\n"
    "  float fill = state.z;\n"
    "  float on = 1.0 - smoothstep(fill - 0.004, fill + 0.004, uv.x);\n"
    "  vec3 healthy = vec3(0.35, 0.85, 0.4), hurt = vec3(0.9, 0.25, 0.2);\n"
    "  vec3 col = mix(hurt, healthy, smoothstep(0.2, 0.7, fill));\n"
    "  float stripe = 0.5 + 0.5 * sin((uv.x * size.x - uv.y * size.y + time * 40.0) * 0.5);\n"
    "  col *= 0.85 + 0.15 * stripe;\n"
    "  vec3 track = vec3(0.08, 0.09, 0.12);\n"
    "  return vec4(mix(track, col, on) * base.rgb, base.a);\n"
    "}\n";
static const char *const example_names[] = {"rim", "button", "ring", "shine", "hologram", "bar"};
size_t r2d_shader_example_count(void) { return sizeof(example_names) / sizeof(example_names[0]); }
const char *r2d_shader_example_name(size_t index) { return index < r2d_shader_example_count() ? example_names[index] : NULL; }
const char *r2d_shader_example_source(size_t index) {
    switch (index) {
        case 0: return example_rim;
        case 1: return example_button;
        case 2: return example_ring;
        case 3: return example_shine;
        case 4: return example_hologram;
        case 5: return example_bar;
        default: return NULL;
    }
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
