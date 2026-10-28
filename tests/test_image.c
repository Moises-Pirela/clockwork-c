/* Image decoding checks: PNG (every colour type, 8/16/2-bit, all five
 * filters, palette with tRNS, a 64x64 image through dynamic Huffman
 * codes), TGA (32-bit bottom-up, RLE 24-bit top-down) and PPM/PGM, plus the
 * inflate primitive. The images come from a directory the build generates
 * with Python (tools/gen-test-images.py); without it the generated-image
 * checks are skipped and only the self-made ones run.
 *
 *   clockwork_image_tests [image_dir]
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "r2d_image.h"
#include "r2d_material.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const unsigned char *px(const r2d_image *i, int x, int y) { return i->pixels + ((size_t)y * (size_t)i->width + (size_t)x) * 4; }
static bool load(const char *dir, const char *name, r2d_image *img, char *error, size_t cap) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return r2d_image_load(img, path, error, cap);
}

int main(int argc, char **argv) {
    char error[160] = "";
    r2d_image img;
    /* inflate: a stored block and a fixed-huffman stream made by hand */
    {
        static const unsigned char stored[] = {0x78, 0x01, 0x01, 0x05, 0x00, 0xfa, 0xff, 'h', 'e', 'l', 'l', 'o'};
        unsigned char out[32];
        CHECK(r2d_inflate(stored, sizeof(stored), out, sizeof(out), true) == 5 && memcmp(out, "hello", 5) == 0, "stored block");
        CHECK(r2d_inflate(stored, sizeof(stored), out, 3, true) == 0, "too small a buffer fails cleanly");
        static const unsigned char bad[] = {0x78, 0x9c, 0xff, 0xff, 0xff};
        CHECK(r2d_inflate(bad, sizeof(bad), out, sizeof(out), true) == 0, "garbage fails");
        /* "a" compressed with fixed codes: 78 9c 4b 04 00 00 62 00 62 */
        static const unsigned char fixed[] = {0x78, 0x9c, 0x4b, 0x04, 0x00, 0x00, 0x62, 0x00, 0x62};
        CHECK(r2d_inflate(fixed, sizeof(fixed), out, sizeof(out), true) == 1 && out[0] == 'a', "fixed huffman block");
    }
    CHECK(r2d_image_solid(&img, 3, 2, 1, 2, 3, 4) && img.width == 3 && px(&img, 2, 1)[3] == 4, "solid image");
    r2d_image_free(&img);
    CHECK(!r2d_image_decode(&img, "nonsense", 8, error, sizeof(error)) && error[0], "unknown format: %s", error);
    CHECK(!r2d_image_load(&img, "missing.png", error, sizeof(error)), "missing file: %s", error);
    if (argc < 2) { printf("(no image directory: generated-image checks skipped)\n"); goto done; }
    const char *dir = argv[1];
    CHECK(load(dir, "rgba.png", &img, error, sizeof(error)), "rgba.png: %s", error);
    if (img.pixels) {
        CHECK(img.width == 5 && img.height == 4, "size");
        const unsigned char *p = px(&img, 3, 2);
        CHECK(p[0] == 150 && p[1] == 120 && p[2] == 100 && p[3] == 128, "RGBA pixel through the Average/Paeth filters (%d %d %d %d)", p[0], p[1], p[2], p[3]);
        CHECK(px(&img, 0, 0)[3] == 255 && px(&img, 1, 0)[3] == 128, "alpha");
        r2d_image_free(&img);
    }
    CHECK(load(dir, "rgb.png", &img, error, sizeof(error)) && px(&img, 4, 3)[0] == 200 && px(&img, 4, 3)[1] == 180 && px(&img, 4, 3)[2] == 7 && px(&img, 4, 3)[3] == 255, "rgb.png (%s)", error);
    r2d_image_free(&img);
    CHECK(load(dir, "gray.png", &img, error, sizeof(error)) && px(&img, 2, 3)[0] == 83 && px(&img, 2, 3)[1] == 83 && px(&img, 2, 3)[3] == 255, "gray.png (%s)", error);
    r2d_image_free(&img);
    CHECK(load(dir, "rgb16.png", &img, error, sizeof(error)) && px(&img, 2, 1)[0] == (24000 >> 8) && px(&img, 2, 1)[1] == (15000 >> 8) && px(&img, 2, 1)[2] == 3, "16-bit png takes the high byte (%s)", error);
    r2d_image_free(&img);
    CHECK(load(dir, "pal2.png", &img, error, sizeof(error)), "pal2.png: %s", error);
    if (img.pixels) {
        CHECK(px(&img, 0, 0)[0] == 255 && px(&img, 0, 0)[3] == 255, "palette entry 0");
        CHECK(px(&img, 1, 0)[1] == 255 && px(&img, 1, 0)[3] == 200, "palette entry 1 with tRNS alpha");
        CHECK(px(&img, 2, 0)[2] == 255 && px(&img, 2, 0)[3] == 0, "transparent entry 2");
        CHECK(px(&img, 3, 0)[0] == 9 && px(&img, 4, 0)[0] == 255, "2-bit unpacking across the row");
        r2d_image_free(&img);
    }
    CHECK(load(dir, "ga.png", &img, error, sizeof(error)) && px(&img, 4, 3)[0] == 120 && px(&img, 4, 3)[3] == 210, "gray+alpha (%s)", error);
    r2d_image_free(&img);
    CHECK(load(dir, "big.png", &img, error, sizeof(error)) && img.width == 64 && img.height == 64, "64x64 dynamic-huffman png (%s)", error);
    if (img.pixels) {
        bool plausible = true;
        for (size_t i = 0; i < 64 * 64 * 4; ++i) { unsigned char v = img.pixels[i]; if (v != 0 && v != 10 && v != 200 && v != 255) plausible = false; }
        CHECK(plausible, "every byte is one of the generated values");
        r2d_image_free(&img);
    }
    CHECK(load(dir, "bgra.tga", &img, error, sizeof(error)), "bgra.tga: %s", error);
    if (img.pixels) {
        const unsigned char *p = px(&img, 2, 1);   /* bottom-up storage flipped back */
        CHECK(p[0] == 100 && p[1] == 60 && p[2] == 60 && p[3] == 130, "tga pixel (%d %d %d %d)", p[0], p[1], p[2], p[3]);
        r2d_image_free(&img);
    }
    CHECK(load(dir, "rle.tga", &img, error, sizeof(error)) && px(&img, 4, 2)[0] == 5 && px(&img, 4, 2)[1] == 2 && px(&img, 4, 2)[2] == 1, "rle tga (%s)", error);
    r2d_image_free(&img);
    CHECK(load(dir, "rgb.ppm", &img, error, sizeof(error)) && px(&img, 1, 2)[0] == 50 && px(&img, 1, 2)[1] == 120, "ppm with a comment (%s)", error);
    r2d_image_free(&img);
    CHECK(load(dir, "gray.pgm", &img, error, sizeof(error)) && px(&img, 3, 1)[0] == 121 && px(&img, 3, 1)[2] == 121, "pgm (%s)", error);
    r2d_image_free(&img);
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/deflate_stored.z", dir);
        FILE *f = fopen(path, "rb");
        unsigned char z[256], out[64];
        size_t n = f ? fread(z, 1, sizeof(z), f) : 0;
        if (f) fclose(f);
        CHECK(n && r2d_inflate(z, n, out, sizeof(out), true) == 23 && memcmp(out, "hello hello hello hello", 23) == 0, "zlib stored stream from python");
    }

    /* --- materials: defaults, effects, text round trip --- */
    {
        r2d_material m, back;
        r2d_material_defaults(&m, "hero");
        CHECK(strcmp(m.name, "hero") == 0 && m.effect == R2D_EFFECT_SPRITE && m.blend == R2D_BLEND_ALPHA && m.tint[3] == 1 && m.tiling[0] == 1, "material defaults");
        r2d_material_set_effect(&m, R2D_EFFECT_OUTLINE);
        CHECK(m.effect == R2D_EFFECT_OUTLINE && m.params[1][0] == 1 && m.params[0][3] == 1, "changing the effect loads its default parameters");
        size_t count = 0;
        const r2d_material_param *params = r2d_material_params(R2D_EFFECT_OUTLINE, &count);
        CHECK(count == 5 && params && params[4].index == 1 && strcmp(params[4].label, "thickness") == 0, "parameter table for outline (%zu)", count);
        params = r2d_material_params(R2D_EFFECT_SPRITE, &count);
        CHECK(count == 0, "the plain sprite has no parameters");
        /* the interface effects: every one has parameters with a colour or a range, and defaults that land in range */
        for (int e = R2D_EFFECT_ROUNDED; e <= R2D_EFFECT_SCANLINES; ++e) {
            params = r2d_material_params((r2d_effect)e, &count);
            bool ok = count > 0;
            for (size_t i = 0; i < count && ok; ++i) ok = params[i].min <= params[i].def && params[i].def <= params[i].max && params[i].index < R2D_MATERIAL_PARAMS && params[i].component < 4 && params[i].label && params[i].label[0];
            CHECK(ok, "effect %s has %zu sane parameters", r2d_effect_name((r2d_effect)e), count);
        }
        r2d_effect parsed0;
        CHECK(r2d_effect_parse("ring", &parsed0) && parsed0 == R2D_EFFECT_RING && r2d_effect_parse("custom", &parsed0) && parsed0 == R2D_EFFECT_CUSTOM && R2D_EFFECT_CUSTOM == R2D_EFFECT_COUNT - 1, "names of the new effects; custom stays last");
        for (int e = 0; e < R2D_EFFECT_COUNT; ++e) {
            r2d_effect parsed;
            CHECK(r2d_effect_parse(r2d_effect_name((r2d_effect)e), &parsed) && parsed == (r2d_effect)e, "effect name %d round trips", e);
            CHECK(r2d_effect_help((r2d_effect)e) != NULL, "effect %d has help", e);
        }
        for (int b = 0; b < R2D_BLEND_COUNT; ++b) {
            r2d_blend parsed;
            CHECK(r2d_blend_parse(r2d_blend_name((r2d_blend)b), &parsed) && parsed == (r2d_blend)b, "blend name %d round trips", b);
        }
        CHECK(!r2d_effect_parse("sparkle", NULL) && !r2d_blend_parse("", NULL), "unknown names are refused");
        {
            r2d_material ring;
            r2d_material_defaults(&ring, "ring");
            r2d_material_set_effect(&ring, R2D_EFFECT_RING);
            CHECK(ring.params[0][0] == 6 && ring.params[1][3] == 0.8f, "ring defaults: thickness 6, track alpha 0.8");
        }
        snprintf(m.texture, sizeof(m.texture), "hero.png");
        m.blend = R2D_BLEND_ADDITIVE;
        m.nearest = true; m.repeat = true;
        m.tint[0] = 0.5f; m.tint[3] = 0.25f;
        m.tiling[1] = 2; m.offset[0] = 0.125f;
        m.params[1][0] = 3.5f;
        char text[2048];
        size_t n = r2d_material_format(&m, text, sizeof(text));
        CHECK(n > 0 && n < sizeof(text) && strncmp(text, "cwmaterial 1\n", 13) == 0 && strstr(text, "effect outline") && strstr(text, "blend additive") && strstr(text, "filter nearest") && strstr(text, "wrap repeat"), "formatted material:\n%s", text);
        memset(&back, 0, sizeof(back));
        CHECK(r2d_material_parse(&back, text, n, error, sizeof(error)), "parse it back: %s", error);
        CHECK(r2d_material_equal(&m, &back), "round trip is lossless");
        back.params[1][0] = 4;
        CHECK(!r2d_material_equal(&m, &back), "a parameter change is noticed");
        CHECK(!r2d_material_parse(&back, "cwmaterial 9\n", 13, error, sizeof(error)) && error[0], "wrong version: %s", error);
        CHECK(!r2d_material_parse(&back, "nonsense", 8, error, sizeof(error)), "not a material");
        const char *custom = "cwmaterial 1\nname rim\neffect custom\nshader rim.cwshader\np2 1 2 3 4\n";
        CHECK(r2d_material_parse(&back, custom, strlen(custom), error, sizeof(error)) && back.effect == R2D_EFFECT_CUSTOM && strcmp(back.shader, "rim.cwshader") == 0 && back.params[2][3] == 4 && back.texture[0] == 0 && back.tint[3] == 1, "sparse file keeps defaults for the rest (%s)", error);
        /* through files */
        char path[512];
        snprintf(path, sizeof(path), "%s/roundtrip.cwmat", dir);
        CHECK(r2d_material_save(&m, path, error, sizeof(error)), "save: %s", error);
        memset(&back, 0, sizeof(back));
        CHECK(r2d_material_load(&back, path, error, sizeof(error)) && r2d_material_equal(&m, &back), "load: %s", error);
        remove(path);
        CHECK(!r2d_material_load(&back, "missing.cwmat", error, sizeof(error)) && error[0], "missing material: %s", error);
    }
done:
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("OK: %d image checks passed\n", checks);
    return 0;
}
