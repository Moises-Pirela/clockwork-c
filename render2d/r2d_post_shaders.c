#include "r2d_post_shaders.h"
/* The shader sources exceed the 4095-character literal length ISO C
 * guarantees; every real compiler accepts far longer strings. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
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

const char *const r2d_post_vs_glsl =
    "#version 410\n"
    "in vec2 pos;\n"
    "out vec2 uv;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); uv = pos * 0.5 + 0.5; }\n";

const char *const r2d_post_fs_glsl =
    "#version 410\n"
    "uniform vec4 params[8];\n"
    "uniform sampler2D tex0;\n"
    "uniform sampler2D tex1;\n"
    "uniform sampler2D tex2;\n"
    "in vec2 uv;\n"
    "out vec4 frag_color;\n"
    "vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }\n"
    "vec3 u2(vec3 x) { return ((x * (0.15 * x + 0.05) + 0.004) / (x * (0.15 * x + 0.50) + 0.06)) - 0.0667; }\n"
    "vec3 filmic(vec3 x) { return u2(x * 2.0) / u2(vec3(11.2)); }\n"
    "float hash2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
    "float vnoise(vec2 p) {\n"
    "  vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);\n"
    "  float a = hash2(i), b = hash2(i + vec2(1.0, 0.0)), c = hash2(i + vec2(0.0, 1.0)), d = hash2(i + vec2(1.0, 1.0));\n"
    "  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n"
    "}\n"
    "float fbm(vec2 p, int octaves) {\n"
    "  float v = 0.0, amp = 0.5, total = 0.0;\n"
    "  for (int i = 0; i < 6; ++i) { if (i >= octaves) break; v += amp * vnoise(p); total += amp; p = p * 2.03 + vec2(17.3, 9.1); amp *= 0.5; }\n"
    "  return v / max(total, 1e-4);\n"
    "}\n"
    "void main() {\n"
    "  int kind = int(params[0].x + 0.5);\n"
    "  vec2 texel = params[0].yz;\n"
    "  bool flip = params[0].w > 0.5;\n"
    "  vec2 suv = vec2(uv.x, flip ? uv.y : 1.0 - uv.y);\n"
    "  vec4 p1 = params[1], p2 = params[2];\n"
    "  vec4 color = texture(tex0, uv);\n"
    "  if (kind == 1) {\n"
    "    float br = max(color.r, max(color.g, color.b));\n"
    "    float knee = p1.x * p1.y + 1e-5;\n"
    "    float soft = clamp(br - p1.x + knee, 0.0, 2.0 * knee);\n"
    "    soft = soft * soft / (4.0 * knee);\n"
    "    float contrib = max(soft, br - p1.x) / max(br, 1e-4);\n"
    "    color = vec4(color.rgb * max(contrib, 0.0), 1.0);\n"
    "  } else if (kind == 2) {\n"
    "    vec2 d = p1.xy * texel;\n"
    "    vec3 s = color.rgb * 0.2270270270;\n"
    "    s += (texture(tex0, uv + d * 1.3846153846).rgb + texture(tex0, uv - d * 1.3846153846).rgb) * 0.3162162162;\n"
    "    s += (texture(tex0, uv + d * 3.2307692308).rgb + texture(tex0, uv - d * 3.2307692308).rgb) * 0.0702702703;\n"
    "    color = vec4(s, 1.0);\n"
    "  } else if (kind == 3) {\n"
    "    color.rgb += texture(tex1, uv).rgb * p1.x;\n"
    "  } else if (kind == 4) {\n"
    "    vec2 d = suv - p1.xy; d.x *= p2.z;\n"
    "    float dist = p2.y < 0.5 ? length(d) : (p2.y < 1.5 ? abs(d.y) : abs(d.x));\n"
    "    float coc = smoothstep(p1.z, p1.z + max(p1.w, 1e-4), dist) * p2.x;\n"
    "    color = mix(color, texture(tex1, uv), coc);\n"
    "  } else if (kind == 5) {\n"
    "    int n = int(p1.z + 0.5);\n"
    "    vec2 c = uv - p2.xy;\n"
    "    vec4 acc = vec4(0.0);\n"
    "    for (int i = 0; i < 32; ++i) {\n"
    "      if (i >= n) break;\n"
    "      float t = float(i) / float(max(n - 1, 1)) - 0.5;\n"
    "      acc += texture(tex0, uv + p1.xy * t + c * p1.w * t);\n"
    "    }\n"
    "    color = acc / float(max(n, 1));\n"
    "  } else if (kind == 6) {\n"
    "    vec2 d = uv - p1.yz;\n"
    "    float f = pow(max(length(d) * 2.0, 1e-4), p1.w);\n"
    "    vec2 o = d * p1.x * f;\n"
    "    color = vec4(texture(tex0, uv + o).r, color.g, texture(tex0, uv - o).b, color.a);\n"
    "  } else if (kind == 7) {\n"
    "    vec3 c = color.rgb * p1.y;\n"
    "    int op = int(p1.x + 0.5);\n"
    "    if (op == 1) c = c / (1.0 + c); else if (op == 2) c = aces(c); else if (op == 3) c = filmic(c);\n"
    "    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "    c = mix(vec3(l), c, p1.w);\n"
    "    c = (c - 0.5) * p2.x + 0.5 + p2.y;\n"
    "    c = pow(max(c, vec3(0.0)), vec3(1.0 / max(p1.z, 1e-3)));\n"
    "    color = vec4(c, color.a);\n"
    "  } else if (kind == 8) {\n"
    "    vec2 d = (suv - 0.5) * 2.0; d.x *= mix(1.0, p2.w, p1.w);\n"
    "    float v = 1.0 - smoothstep(p1.x - max(p1.y, 1e-4), p1.x, length(d));\n"
    "    color.rgb = mix(color.rgb, p2.xyz, (1.0 - v) * p1.z);\n"
    "  } else if (kind == 9) {\n"
    "    vec2 c = (uv - 0.5) / p1.w;\n"
    "    float s = sin(p1.z), co = cos(p1.z);\n"
    "    c = vec2(c.x * co - c.y * s, c.x * s + c.y * co);\n"
    "    color = texture(tex0, clamp(c + 0.5 + p1.xy, vec2(0.0), vec2(1.0)));\n"
    "  } else if (kind == 10) {\n"
    "    vec4 p3 = params[3], p4 = params[4], p5 = params[5];\n"
    "    vec3 direct = texture(tex1, uv).rgb, volume = texture(tex2, uv).rgb;\n"
    "    vec3 lit = color.rgb * direct;\n"
    "    float fogamt = 0.0;\n"
    "    if (p5.w > 0.5) {\n"
    "      vec2 screen = suv * p2.xy;\n"
    "      vec2 world = p1.xy + (screen - p2.xy * 0.5) / p1.z;\n"
    "      if (p1.w > 0.5) world.y = p1.y - (screen.y - p2.y * 0.5) / p1.z;\n"
    "      vec2 q = (world + p3.zw * p2.z) / max(p2.w, 1.0);\n"
    "      float n = fbm(q, int(p5.x + 0.5));\n"
    "      n = clamp((n - 0.5) * p3.y + 0.5, 0.0, 1.0);\n"
    "      fogamt = clamp(p3.x * n, 0.0, 1.0);\n"
    "    }\n"
    "    vec3 fogc = p4.rgb * (p4.w + direct * p5.y + volume);\n"
    "    color = vec4(mix(lit, fogc, fogamt) + volume * p5.z, color.a);\n"
    "  }\n"
    "  frag_color = color;\n"
    "}\n";

const char *const r2d_post_vs_hlsl =
    "struct vs_in { float2 pos : POSITION; };\n"
    "struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "vs_out vs_main(vs_in inp) {\n"
    "  vs_out outp; outp.pos = float4(inp.pos, 0.0, 1.0); outp.uv = float2(inp.pos.x * 0.5 + 0.5, 0.5 - inp.pos.y * 0.5); return outp;\n"
    "}\n";

const char *const r2d_post_fs_hlsl =
    "cbuffer params_cb : register(b0) { float4 params[8]; };\n"
    "Texture2D<float4> tex0 : register(t0);\n"
    "Texture2D<float4> tex1 : register(t1);\n"
    "Texture2D<float4> tex2 : register(t2);\n"
    "SamplerState smp0 : register(s0);\n"
    "struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float3 aces(float3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }\n"
    "float3 u2(float3 x) { return ((x * (0.15 * x + 0.05) + 0.004) / (x * (0.15 * x + 0.50) + 0.06)) - 0.0667; }\n"
    "float3 filmic(float3 x) { return u2(x * 2.0) / u2(float3(11.2, 11.2, 11.2)); }\n"
    "float hash2(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }\n"
    "float vnoise(float2 p) {\n"
    "  float2 i = floor(p), f = frac(p); f = f * f * (3.0 - 2.0 * f);\n"
    "  float a = hash2(i), b = hash2(i + float2(1.0, 0.0)), c = hash2(i + float2(0.0, 1.0)), d = hash2(i + float2(1.0, 1.0));\n"
    "  return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);\n"
    "}\n"
    "float fbm(float2 p, int octaves) {\n"
    "  float v = 0.0, amp = 0.5, total = 0.0;\n"
    "  [loop] for (int i = 0; i < 6; ++i) { if (i >= octaves) break; v += amp * vnoise(p); total += amp; p = p * 2.03 + float2(17.3, 9.1); amp *= 0.5; }\n"
    "  return v / max(total, 1e-4);\n"
    "}\n"
    "float4 fs_main(vs_out inp) : SV_Target0 {\n"
    "  float2 uv = inp.uv;\n"
    "  int kind = (int)(params[0].x + 0.5);\n"
    "  float2 texel = params[0].yz;\n"
    "  bool flip = params[0].w > 0.5;\n"
    "  float2 suv = float2(uv.x, flip ? uv.y : 1.0 - uv.y);\n"
    "  float4 p1 = params[1], p2 = params[2];\n"
    "  float4 color = tex0.Sample(smp0, uv);\n"
    "  if (kind == 1) {\n"
    "    float br = max(color.r, max(color.g, color.b));\n"
    "    float knee = p1.x * p1.y + 1e-5;\n"
    "    float soft = clamp(br - p1.x + knee, 0.0, 2.0 * knee);\n"
    "    soft = soft * soft / (4.0 * knee);\n"
    "    float contrib = max(soft, br - p1.x) / max(br, 1e-4);\n"
    "    color = float4(color.rgb * max(contrib, 0.0), 1.0);\n"
    "  } else if (kind == 2) {\n"
    "    float2 d = p1.xy * texel;\n"
    "    float3 s = color.rgb * 0.2270270270;\n"
    "    s += (tex0.Sample(smp0, uv + d * 1.3846153846).rgb + tex0.Sample(smp0, uv - d * 1.3846153846).rgb) * 0.3162162162;\n"
    "    s += (tex0.Sample(smp0, uv + d * 3.2307692308).rgb + tex0.Sample(smp0, uv - d * 3.2307692308).rgb) * 0.0702702703;\n"
    "    color = float4(s, 1.0);\n"
    "  } else if (kind == 3) {\n"
    "    color.rgb += tex1.Sample(smp0, uv).rgb * p1.x;\n"
    "  } else if (kind == 4) {\n"
    "    float2 d = suv - p1.xy; d.x *= p2.z;\n"
    "    float dist = p2.y < 0.5 ? length(d) : (p2.y < 1.5 ? abs(d.y) : abs(d.x));\n"
    "    float coc = smoothstep(p1.z, p1.z + max(p1.w, 1e-4), dist) * p2.x;\n"
    "    color = lerp(color, tex1.Sample(smp0, uv), coc);\n"
    "  } else if (kind == 5) {\n"
    "    int n = (int)(p1.z + 0.5);\n"
    "    float2 c = uv - p2.xy;\n"
    "    float4 acc = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    [loop] for (int i = 0; i < 32; ++i) {\n"
    "      if (i >= n) break;\n"
    "      float t = (float)i / (float)max(n - 1, 1) - 0.5;\n"
    "      acc += tex0.Sample(smp0, uv + p1.xy * t + c * p1.w * t);\n"
    "    }\n"
    "    color = acc / (float)max(n, 1);\n"
    "  } else if (kind == 6) {\n"
    "    float2 d = uv - p1.yz;\n"
    "    float f = pow(max(length(d) * 2.0, 1e-4), p1.w);\n"
    "    float2 o = d * p1.x * f;\n"
    "    color = float4(tex0.Sample(smp0, uv + o).r, color.g, tex0.Sample(smp0, uv - o).b, color.a);\n"
    "  } else if (kind == 7) {\n"
    "    float3 c = color.rgb * p1.y;\n"
    "    int op = (int)(p1.x + 0.5);\n"
    "    if (op == 1) c = c / (1.0 + c); else if (op == 2) c = aces(c); else if (op == 3) c = filmic(c);\n"
    "    float l = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
    "    c = lerp(float3(l, l, l), c, p1.w);\n"
    "    c = (c - 0.5) * p2.x + 0.5 + p2.y;\n"
    "    c = pow(max(c, float3(0.0, 0.0, 0.0)), float3(1.0, 1.0, 1.0) / max(p1.z, 1e-3));\n"
    "    color = float4(c, color.a);\n"
    "  } else if (kind == 8) {\n"
    "    float2 d = (suv - 0.5) * 2.0; d.x *= lerp(1.0, p2.w, p1.w);\n"
    "    float v = 1.0 - smoothstep(p1.x - max(p1.y, 1e-4), p1.x, length(d));\n"
    "    color.rgb = lerp(color.rgb, p2.xyz, (1.0 - v) * p1.z);\n"
    "  } else if (kind == 9) {\n"
    "    float2 c = (uv - 0.5) / p1.w;\n"
    "    float s = sin(p1.z), co = cos(p1.z);\n"
    "    c = float2(c.x * co - c.y * s, c.x * s + c.y * co);\n"
    "    color = tex0.Sample(smp0, clamp(c + 0.5 + p1.xy, float2(0.0, 0.0), float2(1.0, 1.0)));\n"
    "  } else if (kind == 10) {\n"
    "    float4 p3 = params[3], p4 = params[4], p5 = params[5];\n"
    "    float3 direct = tex1.Sample(smp0, uv).rgb, volume = tex2.Sample(smp0, uv).rgb;\n"
    "    float3 lit = color.rgb * direct;\n"
    "    float fogamt = 0.0;\n"
    "    if (p5.w > 0.5) {\n"
    "      float2 screen = suv * p2.xy;\n"
    "      float2 world = p1.xy + (screen - p2.xy * 0.5) / p1.z;\n"
    "      if (p1.w > 0.5) world.y = p1.y - (screen.y - p2.y * 0.5) / p1.z;\n"
    "      float2 q = (world + p3.zw * p2.z) / max(p2.w, 1.0);\n"
    "      float n = fbm(q, (int)(p5.x + 0.5));\n"
    "      n = clamp((n - 0.5) * p3.y + 0.5, 0.0, 1.0);\n"
    "      fogamt = clamp(p3.x * n, 0.0, 1.0);\n"
    "    }\n"
    "    float3 fogc = p4.rgb * (p4.w + direct * p5.y + volume);\n"
    "    color = float4(lerp(lit, fogc, fogamt) + volume * p5.z, color.a);\n"
    "  }\n"
    "  return color;\n"
    "}\n";

const char *const r2d_post_vs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct vs_in { float2 pos [[attribute(0)]]; };\n"
    "struct vs_out { float4 pos [[position]]; float2 uv; };\n"
    "vertex vs_out vs_main(vs_in inp [[stage_in]]) {\n"
    "  vs_out outp; outp.pos = float4(inp.pos, 0.0, 1.0); outp.uv = float2(inp.pos.x * 0.5 + 0.5, 0.5 - inp.pos.y * 0.5); return outp;\n"
    "}\n";

const char *const r2d_post_fs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct params_t { float4 params[8]; };\n"
    "struct vs_out { float4 pos [[position]]; float2 uv; };\n"
    "static float3 aces(float3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }\n"
    "static float3 u2(float3 x) { return ((x * (0.15 * x + 0.05) + 0.004) / (x * (0.15 * x + 0.50) + 0.06)) - 0.0667; }\n"
    "static float3 filmic(float3 x) { return u2(x * 2.0) / u2(float3(11.2)); }\n"
    "static float hash2(float2 p) { return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }\n"
    "static float vnoise(float2 p) {\n"
    "  float2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);\n"
    "  float a = hash2(i), b = hash2(i + float2(1.0, 0.0)), c = hash2(i + float2(0.0, 1.0)), d = hash2(i + float2(1.0, 1.0));\n"
    "  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n"
    "}\n"
    "static float fbm(float2 p, int octaves) {\n"
    "  float v = 0.0, amp = 0.5, total = 0.0;\n"
    "  for (int i = 0; i < 6; ++i) { if (i >= octaves) break; v += amp * vnoise(p); total += amp; p = p * 2.03 + float2(17.3, 9.1); amp *= 0.5; }\n"
    "  return v / max(total, 1e-4);\n"
    "}\n"
    "fragment float4 fs_main(vs_out inp [[stage_in]], constant params_t& u [[buffer(0)]],\n"
    "                        texture2d<float> tex0 [[texture(0)]], texture2d<float> tex1 [[texture(1)]],\n"
    "                        texture2d<float> tex2 [[texture(2)]], sampler smp0 [[sampler(0)]]) {\n"
    "  float2 uv = inp.uv;\n"
    "  int kind = int(u.params[0].x + 0.5);\n"
    "  float2 texel = u.params[0].yz;\n"
    "  bool flip = u.params[0].w > 0.5;\n"
    "  float2 suv = float2(uv.x, flip ? uv.y : 1.0 - uv.y);\n"
    "  float4 p1 = u.params[1], p2 = u.params[2];\n"
    "  float4 color = tex0.sample(smp0, uv);\n"
    "  if (kind == 1) {\n"
    "    float br = max(color.r, max(color.g, color.b));\n"
    "    float knee = p1.x * p1.y + 1e-5;\n"
    "    float soft = clamp(br - p1.x + knee, 0.0, 2.0 * knee);\n"
    "    soft = soft * soft / (4.0 * knee);\n"
    "    float contrib = max(soft, br - p1.x) / max(br, 1e-4);\n"
    "    color = float4(color.rgb * max(contrib, 0.0), 1.0);\n"
    "  } else if (kind == 2) {\n"
    "    float2 d = p1.xy * texel;\n"
    "    float3 s = color.rgb * 0.2270270270;\n"
    "    s += (tex0.sample(smp0, uv + d * 1.3846153846).rgb + tex0.sample(smp0, uv - d * 1.3846153846).rgb) * 0.3162162162;\n"
    "    s += (tex0.sample(smp0, uv + d * 3.2307692308).rgb + tex0.sample(smp0, uv - d * 3.2307692308).rgb) * 0.0702702703;\n"
    "    color = float4(s, 1.0);\n"
    "  } else if (kind == 3) {\n"
    "    color.rgb += tex1.sample(smp0, uv).rgb * p1.x;\n"
    "  } else if (kind == 4) {\n"
    "    float2 d = suv - p1.xy; d.x *= p2.z;\n"
    "    float dist = p2.y < 0.5 ? length(d) : (p2.y < 1.5 ? abs(d.y) : abs(d.x));\n"
    "    float coc = smoothstep(p1.z, p1.z + max(p1.w, 1e-4), dist) * p2.x;\n"
    "    color = mix(color, tex1.sample(smp0, uv), coc);\n"
    "  } else if (kind == 5) {\n"
    "    int n = int(p1.z + 0.5);\n"
    "    float2 c = uv - p2.xy;\n"
    "    float4 acc = float4(0.0);\n"
    "    for (int i = 0; i < 32; ++i) {\n"
    "      if (i >= n) break;\n"
    "      float t = float(i) / float(max(n - 1, 1)) - 0.5;\n"
    "      acc += tex0.sample(smp0, uv + p1.xy * t + c * p1.w * t);\n"
    "    }\n"
    "    color = acc / float(max(n, 1));\n"
    "  } else if (kind == 6) {\n"
    "    float2 d = uv - p1.yz;\n"
    "    float f = pow(max(length(d) * 2.0, 1e-4), p1.w);\n"
    "    float2 o = d * p1.x * f;\n"
    "    color = float4(tex0.sample(smp0, uv + o).r, color.g, tex0.sample(smp0, uv - o).b, color.a);\n"
    "  } else if (kind == 7) {\n"
    "    float3 c = color.rgb * p1.y;\n"
    "    int op = int(p1.x + 0.5);\n"
    "    if (op == 1) c = c / (1.0 + c); else if (op == 2) c = aces(c); else if (op == 3) c = filmic(c);\n"
    "    float l = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
    "    c = mix(float3(l), c, p1.w);\n"
    "    c = (c - 0.5) * p2.x + 0.5 + p2.y;\n"
    "    c = pow(max(c, float3(0.0)), float3(1.0 / max(p1.z, 1e-3)));\n"
    "    color = float4(c, color.a);\n"
    "  } else if (kind == 8) {\n"
    "    float2 d = (suv - 0.5) * 2.0; d.x *= mix(1.0, p2.w, p1.w);\n"
    "    float v = 1.0 - smoothstep(p1.x - max(p1.y, 1e-4), p1.x, length(d));\n"
    "    color.rgb = mix(color.rgb, p2.xyz, (1.0 - v) * p1.z);\n"
    "  } else if (kind == 9) {\n"
    "    float2 c = (uv - 0.5) / p1.w;\n"
    "    float s = sin(p1.z), co = cos(p1.z);\n"
    "    c = float2(c.x * co - c.y * s, c.x * s + c.y * co);\n"
    "    color = tex0.sample(smp0, clamp(c + 0.5 + p1.xy, float2(0.0), float2(1.0)));\n"
    "  } else if (kind == 10) {\n"
    "    float4 p3 = u.params[3], p4 = u.params[4], p5 = u.params[5];\n"
    "    float3 direct = tex1.sample(smp0, uv).rgb, volume = tex2.sample(smp0, uv).rgb;\n"
    "    float3 lit = color.rgb * direct;\n"
    "    float fogamt = 0.0;\n"
    "    if (p5.w > 0.5) {\n"
    "      float2 screen = suv * p2.xy;\n"
    "      float2 world = p1.xy + (screen - p2.xy * 0.5) / p1.z;\n"
    "      if (p1.w > 0.5) world.y = p1.y - (screen.y - p2.y * 0.5) / p1.z;\n"
    "      float2 q = (world + p3.zw * p2.z) / max(p2.w, 1.0);\n"
    "      float n = fbm(q, int(p5.x + 0.5));\n"
    "      n = clamp((n - 0.5) * p3.y + 0.5, 0.0, 1.0);\n"
    "      fogamt = clamp(p3.x * n, 0.0, 1.0);\n"
    "    }\n"
    "    float3 fogc = p4.rgb * (p4.w + direct * p5.y + volume);\n"
    "    color = float4(mix(lit, fogc, fogamt) + volume * p5.z, color.a);\n"
    "  }\n"
    "  return color;\n"
    "}\n";

/* Light pass: one additive quad per light into a two-target lightmap
 * (direct light, volumetric light). Vertex uniforms: rect in clip space.
 * Fragment uniforms fs_params[5]:
 *   [0] light x, y (pixels, y down), radius px, shadow map row (v coordinate)
 *   [1] color * intensity, falloff exponent
 *   [2] volumetric strength, shadow softness (steps), shadow angles, flip
 *   [3] lightmap width, height, spot direction, cone width
 *   [4] cone softness, spot flag, shadows flag, unused
 * Texture: shadow map, one row per light, R32F distance per angle. */
const char *const r2d_light_vs_glsl =
    "#version 410\n"
    "uniform vec4 vs_params[1];\n"
    "in vec2 pos;\n"
    "out vec2 uv;\n"
    "void main() { vec2 clip = mix(vs_params[0].xy, vs_params[0].zw, pos); gl_Position = vec4(clip, 0.0, 1.0); uv = clip * 0.5 + 0.5; }\n";

#define R2D_LIGHT_FS_BODY(VEC2, VEC3, VEC4, FRACT, SAMPLE) \
    "  " VEC4 " p0 = P(0), p1 = P(1), p2 = P(2), p3 = P(3), p4 = P(4);\n" \
    "  bool flip = p2.w > 0.5;\n" \
    "  " VEC2 " suv = " VEC2 "(uv.x, flip ? uv.y : 1.0 - uv.y);\n" \
    "  " VEC2 " px = suv * p3.xy;\n" \
    "  " VEC2 " d = px - p0.xy;\n" \
    "  float dist = length(d);\n" \
    "  float R = p0.z;\n" \
    "  " VEC3 " direct = " VEC3 "(0.0, 0.0, 0.0), volume = " VEC3 "(0.0, 0.0, 0.0);\n" \
    "  if (dist < R) {\n" \
    "    float angle = atan2(d.y, d.x);\n" \
    "    float a = " FRACT "(angle / 6.28318530718);\n" \
    "    float stepa = 1.0 / max(p2.z, 1.0);\n" \
    "    float vis = 0.0, sd_center = R;\n" \
    "    for (int k = -2; k <= 2; ++k) {\n" \
    "      float ak = " FRACT "(a + float(k) * p2.y * stepa);\n" \
    "      float sd = " SAMPLE ";\n" \
    "      if (k == 0) sd_center = sd;\n" \
    "      vis += dist <= sd + 0.75 ? 1.0 : 0.0;\n" \
    "    }\n" \
    "    vis /= 5.0;\n" \
    "    if (p4.z < 0.5) { vis = 1.0; sd_center = R; }\n" \
    "    float fall = pow(clamp(1.0 - dist / R, 0.0, 1.0), p1.w);\n" \
    "    float cone = 1.0;\n" \
    "    if (p4.y > 0.5) {\n" \
    "      float da = abs(" FRACT "((angle - p3.z) / 6.28318530718 + 0.5) - 0.5) * 6.28318530718;\n" \
    "      float half_cone = p3.w * 0.5;\n" \
    "      float soft = max(half_cone * p4.x, 1e-3);\n" \
    "      cone = 1.0 - smoothstep(half_cone - soft, half_cone, da);\n" \
    "    }\n" \
    "    direct = p1.xyz * fall * vis * cone;\n" \
    "    float lit = min(dist, sd_center);\n" \
    "    float integral = (1.0 - pow(1.0 - lit / R, 3.0)) * R / 3.0;\n" \
    "    float vol = integral / max(dist, 1.0) * clamp((1.0 - dist / R) * 2.5, 0.0, 1.0);\n" \
    "    volume = p1.xyz * vol * p2.x * cone;\n" \
    "  }\n"

const char *const r2d_light_fs_glsl =
    "#version 410\n"
    "uniform vec4 fs_params[5];\n"
    "uniform sampler2D shadow_tex;\n"
    "in vec2 uv;\n"
    "layout(location = 0) out vec4 out_direct;\n"
    "layout(location = 1) out vec4 out_volume;\n"
    "#define P(i) fs_params[i]\n"
    "#define atan2(y, x) atan(y, x)\n"
    "void main() {\n"
    R2D_LIGHT_FS_BODY("vec2", "vec3", "vec4", "fract", "texture(shadow_tex, vec2(ak, p0.w)).r")
    "  out_direct = vec4(direct, 1.0);\n"
    "  out_volume = vec4(volume, 1.0);\n"
    "}\n";

const char *const r2d_light_vs_hlsl =
    "cbuffer vs_cb : register(b0) { float4 vs_params[1]; };\n"
    "struct vs_in { float2 pos : POSITION; };\n"
    "struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "vs_out vs_main(vs_in inp) {\n"
    "  vs_out outp; float2 clip = lerp(vs_params[0].xy, vs_params[0].zw, inp.pos);\n"
    "  outp.pos = float4(clip, 0.0, 1.0); outp.uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5); return outp;\n"
    "}\n";

const char *const r2d_light_fs_hlsl =
    "cbuffer fs_cb : register(b0) { float4 fs_params[5]; };\n"
    "Texture2D<float4> shadow_tex : register(t0);\n"
    "SamplerState shadow_smp : register(s0);\n"
    "struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "struct fs_out { float4 direct : SV_Target0; float4 volume : SV_Target1; };\n"
    "#define P(i) fs_params[i]\n"
    "fs_out fs_main(vs_out inp) {\n"
    "  float2 uv = inp.uv;\n"
    R2D_LIGHT_FS_BODY("float2", "float3", "float4", "frac", "shadow_tex.Sample(shadow_smp, float2(ak, p0.w)).r")
    "  fs_out o; o.direct = float4(direct, 1.0); o.volume = float4(volume, 1.0); return o;\n"
    "}\n";

const char *const r2d_light_vs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct vs_params_t { float4 vs_params[1]; };\n"
    "struct vs_in { float2 pos [[attribute(0)]]; };\n"
    "struct vs_out { float4 pos [[position]]; float2 uv; };\n"
    "vertex vs_out vs_main(vs_in inp [[stage_in]], constant vs_params_t& u [[buffer(0)]]) {\n"
    "  vs_out outp; float2 clip = mix(u.vs_params[0].xy, u.vs_params[0].zw, inp.pos);\n"
    "  outp.pos = float4(clip, 0.0, 1.0); outp.uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5); return outp;\n"
    "}\n";

const char *const r2d_light_fs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct fs_params_t { float4 fs_params[5]; };\n"
    "struct vs_out { float4 pos [[position]]; float2 uv; };\n"
    "struct fs_out { float4 direct [[color(0)]]; float4 volume [[color(1)]]; };\n"
    "#define P(i) u.fs_params[i]\n"
    "fragment fs_out fs_main(vs_out inp [[stage_in]], constant fs_params_t& u [[buffer(0)]],\n"
    "                        texture2d<float> shadow_tex [[texture(0)]], sampler shadow_smp [[sampler(0)]]) {\n"
    "  float2 uv = inp.uv;\n"
    R2D_LIGHT_FS_BODY("float2", "float3", "float4", "fract", "shadow_tex.sample(shadow_smp, float2(ak, p0.w)).r")
    "  fs_out o; o.direct = float4(direct, 1.0); o.volume = float4(volume, 1.0); return o;\n"
    "}\n";


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
