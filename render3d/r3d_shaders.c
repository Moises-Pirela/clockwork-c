#include "r3d_shaders.h"

/* --- mesh: lit surfaces ------------------------------------------------
 * Vertex uniforms: mvp, model, shape (x: capsule cap offset in local units),
 * uvt (tiling.xy, offset.xy). Fragment uniforms: tint (rgba), specular (rgb,
 * shininess), emissive (rgb, unlit flag), params (checker, view mode,
 * textured, unused), ambient (rgb, light count), camera (xyz), shadow_params
 * (enabled, texel size, bias, the light that casts), shadow_matrix (world
 * -> shadow map uv and depth), local_params (texel size and bias of the
 * spot/point shadow layers), local_matrices (one per layer), and per light
 * four vec4: position + range, direction + cos(half cone) (-2: none),
 * colour * intensity + kind (0 directional, 1 point, 2 spot), shadow (kind
 * 0 none / 1 spot / 2 point, first layer). Shadow tests are 3x3 PCFs
 * against R32F depth maps: one 2D map for the directional light, a 2D array
 * of layers for spots (one each) and points (six cube faces each). */
const char *const r3d_mesh_vs_glsl =
    "#version 410\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 model;\n"
    "uniform vec4 shape;\n"
    "uniform vec4 uvt;\n"
    "in vec4 pos;\n"
    "in vec3 normal;\n"
    "in vec2 uv;\n"
    "in vec4 color_in;\n"
    "out vec3 world_pos;\n"
    "out vec3 world_normal;\n"
    "out vec2 frag_uv;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "  vec3 local = pos.xyz + vec3(0.0, pos.w * shape.x, 0.0);\n"
    "  gl_Position = mvp * vec4(local, 1.0);\n"
    "  world_pos = (model * vec4(local, 1.0)).xyz;\n"
    "  world_normal = mat3(model) * normal;\n"
    "  frag_uv = uv * uvt.xy + uvt.zw;\n"
    "  frag_color = color_in;\n"
    "}\n";
#define R3D_LIGHTING_GLSL \
    "  vec4 albedo = tint * frag_color;\n" \
    "  if (params.z > 0.5) albedo *= texture(tex, frag_uv);\n" \
    "  if (params.x > 0.0) {\n" \
    "    vec3 q = floor(world_pos * params.x + 0.001);\n" \
    "    float k = fract((q.x + q.y + q.z) * 0.5) * 2.0;\n" \
    "    albedo.rgb *= mix(1.0, 0.78, k);\n" \
    "  }\n" \
    "  vec3 N = normalize(world_normal);\n" \
    "  if (!gl_FrontFacing) N = -N;\n" \
    "  vec3 V = normalize(camera.xyz - world_pos);\n" \
    "  vec3 lit = albedo.rgb + emissive.rgb;\n" \
    "  if (emissive.w < 0.5) {\n" \
    "    lit = ambient.rgb * albedo.rgb + emissive.rgb;\n" \
    "    int count = int(ambient.w + 0.5);\n" \
    "    for (int i = 0; i < 8; ++i) {\n" \
    "      if (i >= count) break;\n" \
    "      vec4 a = lights[i * 4], b = lights[i * 4 + 1], c = lights[i * 4 + 2], sh = lights[i * 4 + 3];\n" \
    "      int kind = int(c.w + 0.5);\n" \
    "      vec3 L; float att = 1.0;\n" \
    "      if (kind == 0) L = -b.xyz;\n" \
    "      else {\n" \
    "        vec3 d = a.xyz - world_pos; float dist = length(d); L = d / max(dist, 1e-4);\n" \
    "        att = clamp(1.0 - dist / max(a.w, 1e-4), 0.0, 1.0); att *= att;\n" \
    "        if (kind == 2) att *= smoothstep(b.w, min(b.w + 0.08, 1.0), dot(-L, b.xyz));\n" \
    "      }\n" \
    "      float diff = max(dot(N, L), 0.0);\n" \
    "      if (shadow_params.x > 0.5 && i == int(shadow_params.w + 0.5) && diff > 0.0) {\n" \
    "        vec4 sp = shadow_matrix * vec4(world_pos, 1.0);\n" \
    "        vec3 s = sp.xyz / sp.w;\n" \
    "        if (s.x > 0.0 && s.x < 1.0 && s.y > 0.0 && s.y < 1.0 && s.z < 1.0) {\n" \
    "          float bias = shadow_params.z * (1.0 + 2.0 * (1.0 - diff));\n" \
    "          float lit_taps = 0.0;\n" \
    "          for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)\n" \
    "            lit_taps += texture(shadow_map, s.xy + vec2(float(x), float(y)) * shadow_params.y).r + bias >= s.z ? 1.0 : 0.0;\n" \
    "          att *= lit_taps / 9.0;\n" \
    "        }\n" \
    "      }\n" \
    "      if (sh.x > 0.5 && diff > 0.0 && att > 0.0) {\n" \
    "        int layer = int(sh.y + 0.5);\n" \
    "        if (sh.x > 1.5) {\n" \
    "          vec3 dv = world_pos - a.xyz; vec3 ad = abs(dv);\n" \
    "          if (ad.x >= ad.y && ad.x >= ad.z) layer += dv.x > 0.0 ? 0 : 1;\n" \
    "          else if (ad.y >= ad.z) layer += dv.y > 0.0 ? 2 : 3;\n" \
    "          else layer += dv.z > 0.0 ? 4 : 5;\n" \
    "        }\n" \
    "        vec4 lp = local_matrices[layer] * vec4(world_pos, 1.0);\n" \
    "        if (lp.w > 0.0) {\n" \
    "          vec3 s = lp.xyz / lp.w;\n" \
    "          if (s.x > 0.0 && s.x < 1.0 && s.y > 0.0 && s.y < 1.0 && s.z < 1.0) {\n" \
    "            float bias = local_params.y * (1.0 + 2.0 * (1.0 - diff));\n" \
    "            float lit_taps = 0.0;\n" \
    "            for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)\n" \
    "              lit_taps += texture(shadow_layers, vec3(s.xy + vec2(float(x), float(y)) * local_params.x, float(layer))).r + bias >= s.z ? 1.0 : 0.0;\n" \
    "            att *= lit_taps / 9.0;\n" \
    "          }\n" \
    "        }\n" \
    "      }\n" \
    "      float spec = diff > 0.0 ? pow(max(dot(reflect(-L, N), V), 0.0), specular.w) : 0.0;\n" \
    "      lit += (albedo.rgb * diff + specular.rgb * spec) * c.rgb * att;\n" \
    "    }\n" \
    "  }\n" \
    "  vec4 c_out = vec4(lit, albedo.a);\n" \
    "  if (params.y > 1.5) c_out.rgb = vec3(fract(frag_uv), 0.0);\n" \
    "  else if (params.y > 0.5) c_out.rgb = N * 0.5 + 0.5;\n"
const char *const r3d_mesh_fs_glsl =
    "#version 410\n"
    "uniform vec4 tint;\n"
    "uniform vec4 specular;\n"
    "uniform vec4 emissive;\n"
    "uniform vec4 params;\n"
    "uniform vec4 ambient;\n"
    "uniform vec4 camera;\n"
    "uniform vec4 shadow_params;\n"
    "uniform mat4 shadow_matrix;\n"
    "uniform vec4 local_params;\n"
    "uniform mat4 local_matrices[8];\n"
    "uniform vec4 lights[32];\n"
    "uniform sampler2D tex;\n"
    "uniform sampler2D shadow_map;\n"
    "uniform sampler2DArray shadow_layers;\n"
    "in vec3 world_pos;\n"
    "in vec3 world_normal;\n"
    "in vec2 frag_uv;\n"
    "in vec4 frag_color;\n"
    "out vec4 frag_out;\n"
    "void main() {\n"
    R3D_LIGHTING_GLSL
    "  frag_out = c_out;\n"
    "}\n";
const char *const r3d_mesh_vs_hlsl =
    "cbuffer vs_params : register(b0) { float4x4 mvp; float4x4 model; float4 shape; float4 uvt; };\n"
    "struct vs_in { float4 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; float4 color : COLOR0; };\n"
    "struct vs_out { float4 pos : SV_Position; float3 world_pos : TEXCOORD0; float3 world_normal : TEXCOORD1; float2 uv : TEXCOORD2; float4 color : COLOR0; };\n"
    "vs_out vs_main(vs_in inp) {\n"
    "  vs_out o;\n"
    "  float3 local = inp.pos.xyz + float3(0.0, inp.pos.w * shape.x, 0.0);\n"
    "  o.pos = mul(mvp, float4(local, 1.0));\n"
    "  o.world_pos = mul(model, float4(local, 1.0)).xyz;\n"
    "  o.world_normal = mul((float3x3)model, inp.normal);\n"
    "  o.uv = inp.uv * uvt.xy + uvt.zw;\n"
    "  o.color = inp.color;\n"
    "  return o;\n"
    "}\n";
const char *const r3d_mesh_fs_hlsl =
    "cbuffer fs_params : register(b0) { float4 tint; float4 specular; float4 emissive; float4 params; float4 ambient; float4 camera; float4 shadow_params; float4x4 shadow_matrix; float4 local_params; float4x4 local_matrices[8]; float4 lights[32]; };\n"
    "Texture2D<float4> tex : register(t0);\n"
    "Texture2D<float4> shadow_map : register(t1);\n"
    "Texture2DArray<float4> shadow_layers : register(t2);\n"
    "SamplerState smp : register(s0);\n"
    "SamplerState shadow_smp : register(s1);\n"
    "struct fs_in { float4 pos : SV_Position; float3 world_pos : TEXCOORD0; float3 world_normal : TEXCOORD1; float2 uv : TEXCOORD2; float4 color : COLOR0; };\n"
    "float4 fs_main(fs_in inp, bool front : SV_IsFrontFace) : SV_Target0 {\n"
    "  float4 albedo = tint * inp.color;\n"
    "  if (params.z > 0.5) albedo *= tex.Sample(smp, inp.uv);\n"
    "  if (params.x > 0.0) {\n"
    "    float3 q = floor(inp.world_pos * params.x + 0.001);\n"
    "    float k = frac((q.x + q.y + q.z) * 0.5) * 2.0;\n"
    "    albedo.rgb *= lerp(1.0, 0.78, k);\n"
    "  }\n"
    "  float3 N = normalize(inp.world_normal);\n"
    "  if (!front) N = -N;\n"
    "  float3 V = normalize(camera.xyz - inp.world_pos);\n"
    "  float3 lit = albedo.rgb + emissive.rgb;\n"
    "  if (emissive.w < 0.5) {\n"
    "    lit = ambient.rgb * albedo.rgb + emissive.rgb;\n"
    "    int count = int(ambient.w + 0.5);\n"
    "    for (int i = 0; i < 8; ++i) {\n"
    "      if (i >= count) break;\n"
    "      float4 a = lights[i * 4], b = lights[i * 4 + 1], c = lights[i * 4 + 2], sh = lights[i * 4 + 3];\n"
    "      int kind = int(c.w + 0.5);\n"
    "      float3 L; float att = 1.0;\n"
    "      if (kind == 0) L = -b.xyz;\n"
    "      else {\n"
    "        float3 d = a.xyz - inp.world_pos; float dist = length(d); L = d / max(dist, 1e-4);\n"
    "        att = saturate(1.0 - dist / max(a.w, 1e-4)); att *= att;\n"
    "        if (kind == 2) att *= smoothstep(b.w, min(b.w + 0.08, 1.0), dot(-L, b.xyz));\n"
    "      }\n"
    "      float diff = max(dot(N, L), 0.0);\n"
    "      if (shadow_params.x > 0.5 && i == int(shadow_params.w + 0.5) && diff > 0.0) {\n"
    "        float4 sp = mul(shadow_matrix, float4(inp.world_pos, 1.0));\n"
    "        float3 s = sp.xyz / sp.w;\n"
    "        if (s.x > 0.0 && s.x < 1.0 && s.y > 0.0 && s.y < 1.0 && s.z < 1.0) {\n"
    "          float bias = shadow_params.z * (1.0 + 2.0 * (1.0 - diff));\n"
    "          float lit_taps = 0.0;\n"
    "          for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)\n"
    "            lit_taps += shadow_map.Sample(shadow_smp, s.xy + float2(x, y) * shadow_params.y).r + bias >= s.z ? 1.0 : 0.0;\n"
    "          att *= lit_taps / 9.0;\n"
    "        }\n"
    "      }\n"
    "      if (sh.x > 0.5 && diff > 0.0 && att > 0.0) {\n"
    "        int layer = int(sh.y + 0.5);\n"
    "        if (sh.x > 1.5) {\n"
    "          float3 dv = inp.world_pos - a.xyz; float3 ad = abs(dv);\n"
    "          if (ad.x >= ad.y && ad.x >= ad.z) layer += dv.x > 0.0 ? 0 : 1;\n"
    "          else if (ad.y >= ad.z) layer += dv.y > 0.0 ? 2 : 3;\n"
    "          else layer += dv.z > 0.0 ? 4 : 5;\n"
    "        }\n"
    "        float4 lp = mul(local_matrices[layer], float4(inp.world_pos, 1.0));\n"
    "        if (lp.w > 0.0) {\n"
    "          float3 s = lp.xyz / lp.w;\n"
    "          if (s.x > 0.0 && s.x < 1.0 && s.y > 0.0 && s.y < 1.0 && s.z < 1.0) {\n"
    "            float bias = local_params.y * (1.0 + 2.0 * (1.0 - diff));\n"
    "            float lit_taps = 0.0;\n"
    "            for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)\n"
    "              lit_taps += shadow_layers.Sample(shadow_smp, float3(s.xy + float2(x, y) * local_params.x, layer)).r + bias >= s.z ? 1.0 : 0.0;\n"
    "            att *= lit_taps / 9.0;\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "      float spec = diff > 0.0 ? pow(max(dot(reflect(-L, N), V), 0.0), specular.w) : 0.0;\n"
    "      lit += (albedo.rgb * diff + specular.rgb * spec) * c.rgb * att;\n"
    "    }\n"
    "  }\n"
    "  float4 c_out = float4(lit, albedo.a);\n"
    "  if (params.y > 1.5) c_out.rgb = float3(frac(inp.uv), 0.0);\n"
    "  else if (params.y > 0.5) c_out.rgb = N * 0.5 + 0.5;\n"
    "  return c_out;\n"
    "}\n";
const char *const r3d_mesh_vs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct vs_params { float4x4 mvp; float4x4 model; float4 shape; float4 uvt; };\n"
    "struct vs_in { float4 pos [[attribute(0)]]; float3 normal [[attribute(1)]]; float2 uv [[attribute(2)]]; float4 color [[attribute(3)]]; };\n"
    "struct vs_out { float4 pos [[position]]; float3 world_pos; float3 world_normal; float2 uv; float4 color; };\n"
    "vertex vs_out vs_main(vs_in inp [[stage_in]], constant vs_params& u [[buffer(0)]]) {\n"
    "  vs_out o;\n"
    "  float3 local = inp.pos.xyz + float3(0.0, inp.pos.w * u.shape.x, 0.0);\n"
    "  o.pos = u.mvp * float4(local, 1.0);\n"
    "  o.world_pos = (u.model * float4(local, 1.0)).xyz;\n"
    "  o.world_normal = float3x3(u.model[0].xyz, u.model[1].xyz, u.model[2].xyz) * inp.normal;\n"
    "  o.uv = inp.uv * u.uvt.xy + u.uvt.zw;\n"
    "  o.color = inp.color;\n"
    "  return o;\n"
    "}\n";
const char *const r3d_mesh_fs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct fs_params { float4 tint; float4 specular; float4 emissive; float4 params; float4 ambient; float4 camera; float4 shadow_params; float4x4 shadow_matrix; float4 local_params; float4x4 local_matrices[8]; float4 lights[32]; };\n"
    "struct fs_in { float4 pos [[position]]; float3 world_pos; float3 world_normal; float2 uv; float4 color; };\n"
    "fragment float4 fs_main(fs_in inp [[stage_in]], bool front [[front_facing]], constant fs_params& u [[buffer(0)]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]], texture2d<float> shadow_map [[texture(1)]], sampler shadow_smp [[sampler(1)]], texture2d_array<float> shadow_layers [[texture(2)]]) {\n"
    "  float4 albedo = u.tint * inp.color;\n"
    "  if (u.params.z > 0.5) albedo *= tex.sample(smp, inp.uv);\n"
    "  if (u.params.x > 0.0) {\n"
    "    float3 q = floor(inp.world_pos * u.params.x + 0.001);\n"
    "    float k = fract((q.x + q.y + q.z) * 0.5) * 2.0;\n"
    "    albedo.rgb *= mix(1.0, 0.78, k);\n"
    "  }\n"
    "  float3 N = normalize(inp.world_normal);\n"
    "  if (!front) N = -N;\n"
    "  float3 V = normalize(u.camera.xyz - inp.world_pos);\n"
    "  float3 lit = albedo.rgb + u.emissive.rgb;\n"
    "  if (u.emissive.w < 0.5) {\n"
    "    lit = u.ambient.rgb * albedo.rgb + u.emissive.rgb;\n"
    "    int count = int(u.ambient.w + 0.5);\n"
    "    for (int i = 0; i < 8; ++i) {\n"
    "      if (i >= count) break;\n"
    "      float4 a = u.lights[i * 4], b = u.lights[i * 4 + 1], c = u.lights[i * 4 + 2], sh = u.lights[i * 4 + 3];\n"
    "      int kind = int(c.w + 0.5);\n"
    "      float3 L; float att = 1.0;\n"
    "      if (kind == 0) L = -b.xyz;\n"
    "      else {\n"
    "        float3 d = a.xyz - inp.world_pos; float dist = length(d); L = d / max(dist, 1e-4);\n"
    "        att = saturate(1.0 - dist / max(a.w, 1e-4)); att *= att;\n"
    "        if (kind == 2) att *= smoothstep(b.w, min(b.w + 0.08, 1.0), dot(-L, b.xyz));\n"
    "      }\n"
    "      float diff = max(dot(N, L), 0.0);\n"
    "      if (u.shadow_params.x > 0.5 && i == int(u.shadow_params.w + 0.5) && diff > 0.0) {\n"
    "        float4 sp = u.shadow_matrix * float4(inp.world_pos, 1.0);\n"
    "        float3 s = sp.xyz / sp.w;\n"
    "        if (s.x > 0.0 && s.x < 1.0 && s.y > 0.0 && s.y < 1.0 && s.z < 1.0) {\n"
    "          float bias = u.shadow_params.z * (1.0 + 2.0 * (1.0 - diff));\n"
    "          float lit_taps = 0.0;\n"
    "          for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)\n"
    "            lit_taps += shadow_map.sample(shadow_smp, s.xy + float2(x, y) * u.shadow_params.y).r + bias >= s.z ? 1.0 : 0.0;\n"
    "          att *= lit_taps / 9.0;\n"
    "        }\n"
    "      }\n"
    "      if (sh.x > 0.5 && diff > 0.0 && att > 0.0) {\n"
    "        int layer = int(sh.y + 0.5);\n"
    "        if (sh.x > 1.5) {\n"
    "          float3 dv = inp.world_pos - a.xyz; float3 ad = abs(dv);\n"
    "          if (ad.x >= ad.y && ad.x >= ad.z) layer += dv.x > 0.0 ? 0 : 1;\n"
    "          else if (ad.y >= ad.z) layer += dv.y > 0.0 ? 2 : 3;\n"
    "          else layer += dv.z > 0.0 ? 4 : 5;\n"
    "        }\n"
    "        float4 lp = u.local_matrices[layer] * float4(inp.world_pos, 1.0);\n"
    "        if (lp.w > 0.0) {\n"
    "          float3 s = lp.xyz / lp.w;\n"
    "          if (s.x > 0.0 && s.x < 1.0 && s.y > 0.0 && s.y < 1.0 && s.z < 1.0) {\n"
    "            float bias = u.local_params.y * (1.0 + 2.0 * (1.0 - diff));\n"
    "            float lit_taps = 0.0;\n"
    "            for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)\n"
    "              lit_taps += shadow_layers.sample(shadow_smp, s.xy + float2(x, y) * u.local_params.x, layer).r + bias >= s.z ? 1.0 : 0.0;\n"
    "            att *= lit_taps / 9.0;\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "      float spec = diff > 0.0 ? pow(max(dot(reflect(-L, N), V), 0.0), u.specular.w) : 0.0;\n"
    "      lit += (albedo.rgb * diff + u.specular.rgb * spec) * c.rgb * att;\n"
    "    }\n"
    "  }\n"
    "  float4 c_out = float4(lit, albedo.a);\n"
    "  if (u.params.y > 1.5) c_out.rgb = float3(fract(inp.uv), 0.0);\n"
    "  else if (u.params.y > 0.5) c_out.rgb = N * 0.5 + 0.5;\n"
    "  return c_out;\n"
    "}\n";

/* --- shadow pass: light-space depth into an R32F target ----------------
 * Clip z and w travel separately and divide per fragment: both are affine in
 * world space, so they interpolate exactly under a perspective (spot/point)
 * projection, where an interpolated z/w would not. */
const char *const r3d_shadow_vs_glsl =
    "#version 410\n"
    "uniform mat4 mvp;\n"
    "uniform vec4 shape;\n"
    "in vec4 pos;\n"
    "out vec2 clip_zw;\n"
    "void main() { vec3 local = pos.xyz + vec3(0.0, pos.w * shape.x, 0.0); gl_Position = mvp * vec4(local, 1.0); clip_zw = gl_Position.zw; }\n";
const char *const r3d_shadow_fs_glsl =
    "#version 410\n"
    "in vec2 clip_zw;\n"
    "out vec4 frag_color;\n"
    "void main() { frag_color = vec4(clip_zw.x / clip_zw.y, 0.0, 0.0, 1.0); }\n";
const char *const r3d_shadow_vs_hlsl =
    "cbuffer vs_params : register(b0) { float4x4 mvp; float4 shape; };\n"
    "struct vs_in { float4 pos : POSITION; };\n"
    "struct vs_out { float4 pos : SV_Position; float2 clip_zw : TEXCOORD0; };\n"
    "vs_out vs_main(vs_in inp) { vs_out o; float3 local = inp.pos.xyz + float3(0.0, inp.pos.w * shape.x, 0.0); o.pos = mul(mvp, float4(local, 1.0)); o.clip_zw = o.pos.zw; return o; }\n";
const char *const r3d_shadow_fs_hlsl =
    "struct fs_in { float4 pos : SV_Position; float2 clip_zw : TEXCOORD0; };\n"
    "float4 fs_main(fs_in inp) : SV_Target0 { return float4(inp.clip_zw.x / inp.clip_zw.y, 0.0, 0.0, 1.0); }\n";
const char *const r3d_shadow_vs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct vs_params { float4x4 mvp; float4 shape; };\n"
    "struct vs_in { float4 pos [[attribute(0)]]; };\n"
    "struct vs_out { float4 pos [[position]]; float2 clip_zw; };\n"
    "vertex vs_out vs_main(vs_in inp [[stage_in]], constant vs_params& u [[buffer(0)]]) {\n"
    "  vs_out o; float3 local = inp.pos.xyz + float3(0.0, inp.pos.w * u.shape.x, 0.0); o.pos = u.mvp * float4(local, 1.0); o.clip_zw = o.pos.zw; return o;\n"
    "}\n";
const char *const r3d_shadow_fs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct fs_in { float4 pos [[position]]; float2 clip_zw; };\n"
    "fragment float4 fs_main(fs_in inp [[stage_in]]) { return float4(inp.clip_zw.x / inp.clip_zw.y, 0.0, 0.0, 1.0); }\n";

/* --- sky: a fullscreen quad at the far plane ---------------------------
 * Vertex uniforms: inv_view_proj, camera (xyz). Fragment uniforms: zenith,
 * horizon, ground (rgb), sun (direction xyz, angular size), sun_color (rgb *
 * intensity, use-cube flag); a cube map when the flag is set. */
const char *const r3d_sky_vs_glsl =
    "#version 410\n"
    "uniform mat4 inv_view_proj;\n"
    "uniform vec4 camera;\n"
    "in vec2 pos;\n"
    "out vec3 world_dir;\n"
    "void main() { gl_Position = vec4(pos, 1.0, 1.0); vec4 far = inv_view_proj * vec4(pos, 1.0, 1.0); world_dir = far.xyz / far.w - camera.xyz; }\n";
#define R3D_SKY_BODY_GLSL \
    "  vec3 d = normalize(world_dir);\n" \
    "  vec3 c;\n" \
    "  if (sun_color.w > 0.5) c = texture(sky_cube, d).rgb;\n" \
    "  else {\n" \
    "    float t = d.y;\n" \
    "    c = t >= 0.0 ? mix(horizon.rgb, zenith.rgb, pow(clamp(t, 0.0, 1.0), 0.6)) : mix(horizon.rgb, ground.rgb, clamp(-t * 4.0, 0.0, 1.0));\n" \
    "    if (sun.w > 0.0) {\n" \
    "      float cosang = dot(d, -sun.xyz);\n" \
    "      float disc = smoothstep(cos(sun.w * 0.5 + 0.01), cos(sun.w * 0.5), cosang);\n" \
    "      float glow = pow(max(cosang, 0.0), 64.0) * 0.25;\n" \
    "      c += sun_color.rgb * (disc + glow);\n" \
    "    }\n" \
    "  }\n"
const char *const r3d_sky_fs_glsl =
    "#version 410\n"
    "uniform vec4 zenith;\n"
    "uniform vec4 horizon;\n"
    "uniform vec4 ground;\n"
    "uniform vec4 sun;\n"
    "uniform vec4 sun_color;\n"
    "uniform samplerCube sky_cube;\n"
    "in vec3 world_dir;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    R3D_SKY_BODY_GLSL
    "  frag_color = vec4(c, 1.0);\n"
    "}\n";
const char *const r3d_sky_vs_hlsl =
    "cbuffer vs_params : register(b0) { float4x4 inv_view_proj; float4 camera; };\n"
    "struct vs_in { float2 pos : POSITION; };\n"
    "struct vs_out { float4 pos : SV_Position; float3 world_dir : TEXCOORD0; };\n"
    "vs_out vs_main(vs_in inp) { vs_out o; o.pos = float4(inp.pos, 1.0, 1.0); float4 far = mul(inv_view_proj, float4(inp.pos, 1.0, 1.0)); o.world_dir = far.xyz / far.w - camera.xyz; return o; }\n";
const char *const r3d_sky_fs_hlsl =
    "cbuffer fs_params : register(b0) { float4 zenith; float4 horizon; float4 ground; float4 sun; float4 sun_color; };\n"
    "TextureCube<float4> sky_cube : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct fs_in { float4 pos : SV_Position; float3 world_dir : TEXCOORD0; };\n"
    "float4 fs_main(fs_in inp) : SV_Target0 {\n"
    "  float3 d = normalize(inp.world_dir);\n"
    "  float3 c;\n"
    "  if (sun_color.w > 0.5) c = sky_cube.Sample(smp, d).rgb;\n"
    "  else {\n"
    "    float t = d.y;\n"
    "    c = t >= 0.0 ? lerp(horizon.rgb, zenith.rgb, pow(saturate(t), 0.6)) : lerp(horizon.rgb, ground.rgb, saturate(-t * 4.0));\n"
    "    if (sun.w > 0.0) {\n"
    "      float cosang = dot(d, -sun.xyz);\n"
    "      float disc = smoothstep(cos(sun.w * 0.5 + 0.01), cos(sun.w * 0.5), cosang);\n"
    "      float glow = pow(max(cosang, 0.0), 64.0) * 0.25;\n"
    "      c += sun_color.rgb * (disc + glow);\n"
    "    }\n"
    "  }\n"
    "  return float4(c, 1.0);\n"
    "}\n";
const char *const r3d_sky_vs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct vs_params { float4x4 inv_view_proj; float4 camera; };\n"
    "struct vs_in { float2 pos [[attribute(0)]]; };\n"
    "struct vs_out { float4 pos [[position]]; float3 world_dir; };\n"
    "vertex vs_out vs_main(vs_in inp [[stage_in]], constant vs_params& u [[buffer(0)]]) {\n"
    "  vs_out o; o.pos = float4(inp.pos, 1.0, 1.0); float4 far = u.inv_view_proj * float4(inp.pos, 1.0, 1.0); o.world_dir = far.xyz / far.w - u.camera.xyz; return o;\n"
    "}\n";
const char *const r3d_sky_fs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct fs_params { float4 zenith; float4 horizon; float4 ground; float4 sun; float4 sun_color; };\n"
    "struct fs_in { float4 pos [[position]]; float3 world_dir; };\n"
    "fragment float4 fs_main(fs_in inp [[stage_in]], constant fs_params& u [[buffer(0)]], texturecube<float> sky_cube [[texture(0)]], sampler smp [[sampler(0)]]) {\n"
    "  float3 d = normalize(inp.world_dir);\n"
    "  float3 c;\n"
    "  if (u.sun_color.w > 0.5) c = sky_cube.sample(smp, d).rgb;\n"
    "  else {\n"
    "    float t = d.y;\n"
    "    c = t >= 0.0 ? mix(u.horizon.rgb, u.zenith.rgb, pow(saturate(t), 0.6)) : mix(u.horizon.rgb, u.ground.rgb, saturate(-t * 4.0));\n"
    "    if (u.sun.w > 0.0) {\n"
    "      float cosang = dot(d, -u.sun.xyz);\n"
    "      float disc = smoothstep(cos(u.sun.w * 0.5 + 0.01), cos(u.sun.w * 0.5), cosang);\n"
    "      float glow = pow(max(cosang, 0.0), 64.0) * 0.25;\n"
    "      c += u.sun_color.rgb * (disc + glow);\n"
    "    }\n"
    "  }\n"
    "  return float4(c, 1.0);\n"
    "}\n";

/* --- lines ------------------------------------------------------------- */
const char *const r3d_line_vs_glsl =
    "#version 410\n"
    "uniform mat4 mvp;\n"
    "in vec3 pos;\n"
    "in vec4 color_in;\n"
    "out vec4 color;\n"
    "void main() { gl_Position = mvp * vec4(pos, 1.0); color = color_in; }\n";
const char *const r3d_line_fs_glsl =
    "#version 410\n"
    "in vec4 color;\n"
    "out vec4 frag_color;\n"
    "void main() { frag_color = color; }\n";
const char *const r3d_line_vs_hlsl =
    "cbuffer vs_params : register(b0) { float4x4 mvp; };\n"
    "struct vs_in { float3 pos : POSITION; float4 color : COLOR0; };\n"
    "struct vs_out { float4 pos : SV_Position; float4 color : COLOR0; };\n"
    "vs_out vs_main(vs_in inp) { vs_out o; o.pos = mul(mvp, float4(inp.pos, 1.0)); o.color = inp.color; return o; }\n";
const char *const r3d_line_fs_hlsl =
    "struct fs_in { float4 pos : SV_Position; float4 color : COLOR0; };\n"
    "float4 fs_main(fs_in inp) : SV_Target0 { return inp.color; }\n";
const char *const r3d_line_vs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct vs_params { float4x4 mvp; };\n"
    "struct vs_in { float3 pos [[attribute(0)]]; float4 color [[attribute(1)]]; };\n"
    "struct vs_out { float4 pos [[position]]; float4 color; };\n"
    "vertex vs_out vs_main(vs_in inp [[stage_in]], constant vs_params& u [[buffer(0)]]) {\n"
    "  vs_out o; o.pos = u.mvp * float4(inp.pos, 1.0); o.color = inp.color; return o;\n"
    "}\n";
const char *const r3d_line_fs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct fs_in { float4 pos [[position]]; float4 color; };\n"
    "fragment float4 fs_main(fs_in inp [[stage_in]]) { return inp.color; }\n";

/* --- composite: the 3D target into the host's pass --------------------- */
const char *const r3d_composite_vs_glsl =
    "#version 410\n"
    "in vec2 pos;\n"
    "in vec2 uv_in;\n"
    "out vec2 uv;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); uv = uv_in; }\n";
const char *const r3d_composite_fs_glsl =
    "#version 410\n"
    "uniform sampler2D tex;\n"
    "in vec2 uv;\n"
    "out vec4 frag_color;\n"
    "void main() { frag_color = texture(tex, uv); }\n";
const char *const r3d_composite_vs_hlsl =
    "struct vs_in { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "vs_out vs_main(vs_in inp) { vs_out o; o.pos = float4(inp.pos, 0.0, 1.0); o.uv = inp.uv; return o; }\n";
const char *const r3d_composite_fs_hlsl =
    "Texture2D<float4> tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct fs_in { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 fs_main(fs_in inp) : SV_Target0 { return tex.Sample(smp, inp.uv); }\n";
const char *const r3d_composite_vs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct vs_in { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };\n"
    "struct vs_out { float4 pos [[position]]; float2 uv; };\n"
    "vertex vs_out vs_main(vs_in inp [[stage_in]]) { vs_out o; o.pos = float4(inp.pos, 0.0, 1.0); o.uv = inp.uv; return o; }\n";
const char *const r3d_composite_fs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct fs_in { float4 pos [[position]]; float2 uv; };\n"
    "fragment float4 fs_main(fs_in inp [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {\n"
    "  return tex.sample(smp, inp.uv);\n"
    "}\n";
