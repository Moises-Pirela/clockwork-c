#define _CRT_SECURE_NO_WARNINGS 1
#include "r2d_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return false;
}

/* ---------------------------------------------------------------------------
 * inflate (RFC 1951) with the zlib wrapper (RFC 1950)
 * ------------------------------------------------------------------------- */
typedef struct {
    const unsigned char *src;
    size_t size, pos;
    uint32_t bits;
    int count;
    bool error;
} bit_reader;
static uint32_t take(bit_reader *r, int n) {
    while (r->count < n) {
        if (r->pos >= r->size) { r->error = true; return 0; }
        r->bits |= (uint32_t)r->src[r->pos++] << r->count;
        r->count += 8;
    }
    uint32_t v = r->bits & ((n == 32) ? 0xffffffffu : ((1u << n) - 1u));
    r->bits >>= n;
    r->count -= n;
    return v;
}
typedef struct { uint16_t count[16], symbol[320]; } huffman;
static bool build(huffman *h, const unsigned char *lengths, int n) {
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; ++i) h->count[lengths[i]]++;
    h->count[0] = 0;
    uint16_t offsets[16];
    offsets[1] = 0;
    for (int i = 1; i < 15; ++i) offsets[i + 1] = (uint16_t)(offsets[i] + h->count[i]);
    for (int i = 0; i < n; ++i) if (lengths[i]) h->symbol[offsets[lengths[i]]++] = (uint16_t)i;
    /* over-subscription check */
    int left = 1;
    for (int i = 1; i < 16; ++i) { left <<= 1; left -= h->count[i]; if (left < 0) return false; }
    return true;
}
static int decode(bit_reader *r, const huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= (int)take(r, 1);
        if (r->error) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}
static const uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static bool codes(bit_reader *r, const huffman *lit, const huffman *dist, unsigned char *dst, size_t capacity, size_t *out) {
    for (;;) {
        int sym = decode(r, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (*out >= capacity) return false;
            dst[(*out)++] = (unsigned char)sym;
        } else if (sym == 256) return true;
        else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = length_base[sym] + take(r, length_extra[sym]);
            int d = decode(r, dist);
            if (d < 0 || d >= 30) return false;
            size_t distance = dist_base[d] + take(r, dist_extra[d]);
            if (r->error || distance > *out || *out + len > capacity) return false;
            for (size_t i = 0; i < len; ++i, ++*out) dst[*out] = dst[*out - distance];
        }
    }
}
size_t r2d_inflate(const unsigned char *src, size_t size, unsigned char *dst, size_t capacity, bool zlib_header) {
    bit_reader r = {src, size, 0, 0, 0, false};
    if (zlib_header) {
        if (size < 2 || (src[0] & 0x0f) != 8 || ((src[0] << 8) | src[1]) % 31 != 0 || (src[1] & 0x20)) return 0;
        r.pos = 2;
    }
    size_t out = 0;
    huffman lit, dist;
    static const unsigned char fixed_lengths[288] = {
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
        9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8};
    static const unsigned char fixed_dist[30] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    int final;
    do {
        final = (int)take(&r, 1);
        int type = (int)take(&r, 2);
        if (r.error) return 0;
        if (type == 0) {
            r.bits = 0;
            r.count = 0;   /* byte-align */
            if (r.pos + 4 > size) return 0;
            size_t len = src[r.pos] | (src[r.pos + 1] << 8), nlen = src[r.pos + 2] | (src[r.pos + 3] << 8);
            r.pos += 4;
            if ((len ^ 0xffff) != nlen || r.pos + len > size || out + len > capacity) return 0;
            memcpy(dst + out, src + r.pos, len);
            r.pos += len;
            out += len;
        } else if (type == 1) {
            build(&lit, fixed_lengths, 288);
            build(&dist, fixed_dist, 30);
            if (!codes(&r, &lit, &dist, dst, capacity, &out)) return 0;
        } else if (type == 2) {
            int nlen = (int)take(&r, 5) + 257, ndist = (int)take(&r, 5) + 1, ncode = (int)take(&r, 4) + 4;
            if (r.error || nlen > 286 || ndist > 30) return 0;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            unsigned char lengths[320];
            memset(lengths, 0, sizeof(lengths));
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = (unsigned char)take(&r, 3);
            huffman code_huff;
            if (r.error || !build(&code_huff, lengths, 19)) return 0;
            int index = 0;
            while (index < nlen + ndist) {
                int sym = decode(&r, &code_huff);
                if (sym < 0) return 0;
                if (sym < 16) lengths[index++] = (unsigned char)sym;
                else {
                    int repeat, value = 0;
                    if (sym == 16) { if (index == 0) return 0; value = lengths[index - 1]; repeat = 3 + (int)take(&r, 2); }
                    else if (sym == 17) repeat = 3 + (int)take(&r, 3);
                    else repeat = 11 + (int)take(&r, 7);
                    if (r.error || index + repeat > nlen + ndist) return 0;
                    while (repeat--) lengths[index++] = (unsigned char)value;
                }
            }
            if (!build(&lit, lengths, nlen) || !build(&dist, lengths + nlen, ndist)) return 0;
            if (!codes(&r, &lit, &dist, dst, capacity, &out)) return 0;
        } else return 0;
    } while (!final);
    return out;
}

/* ---------------------------------------------------------------------------
 * PNG
 * ------------------------------------------------------------------------- */
static uint32_t be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static unsigned char paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (unsigned char)(pa <= pb && pa <= pc ? a : (pb <= pc ? b : c));
}
static bool decode_png(r2d_image *img, const unsigned char *data, size_t size, char *error, size_t capacity) {
    if (size < 33 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0) return fail(error, capacity, "not a PNG");
    size_t pos = 8;
    int width = 0, height = 0, depth = 0, color = 0, interlace = 0;
    unsigned char palette[256 * 4];
    int palette_count = 0;
    bool trns_key = false;
    int key[3] = {0, 0, 0};
    unsigned char *idat = NULL;
    size_t idat_size = 0, idat_capacity = 0;
    bool ok = true, seen_header = false, done = false;
    for (int i = 0; i < 256; ++i) { palette[i * 4 + 3] = 255; }
    while (ok && !done && pos + 12 <= size) {
        uint32_t len = be32(data + pos);
        const unsigned char *type = data + pos + 4, *chunk = data + pos + 8;
        if (len > size - pos - 12) { ok = fail(error, capacity, "truncated PNG chunk"); break; }
        if (memcmp(type, "IHDR", 4) == 0) {
            if (len != 13) { ok = fail(error, capacity, "bad IHDR"); break; }
            width = (int)be32(chunk);
            height = (int)be32(chunk + 4);
            depth = chunk[8];
            color = chunk[9];
            interlace = chunk[12];
            seen_header = true;
            if (width <= 0 || height <= 0 || width > R2D_IMAGE_MAX_DIMENSION || height > R2D_IMAGE_MAX_DIMENSION) { ok = fail(error, capacity, "PNG dimensions out of range"); break; }
            if (interlace) { ok = fail(error, capacity, "interlaced PNGs are not supported"); break; }
            bool depth_ok = depth == 8 || depth == 16 || ((color == 0 || color == 3) && (depth == 1 || depth == 2 || depth == 4));
            if (!depth_ok || (color != 0 && color != 2 && color != 3 && color != 4 && color != 6) || (color == 3 && depth == 16)) { ok = fail(error, capacity, "unsupported PNG colour type or bit depth"); break; }
        } else if (memcmp(type, "PLTE", 4) == 0) {
            palette_count = (int)(len / 3);
            if (palette_count > 256) palette_count = 256;
            for (int i = 0; i < palette_count; ++i) { palette[i * 4] = chunk[i * 3]; palette[i * 4 + 1] = chunk[i * 3 + 1]; palette[i * 4 + 2] = chunk[i * 3 + 2]; }
        } else if (memcmp(type, "tRNS", 4) == 0) {
            if (color == 3) for (uint32_t i = 0; i < len && i < 256; ++i) palette[i * 4 + 3] = chunk[i];
            else if (color == 0 && len >= 2) { trns_key = true; key[0] = (chunk[0] << 8) | chunk[1]; }
            else if (color == 2 && len >= 6) { trns_key = true; key[0] = (chunk[0] << 8) | chunk[1]; key[1] = (chunk[2] << 8) | chunk[3]; key[2] = (chunk[4] << 8) | chunk[5]; }
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_size + len > idat_capacity) {
                idat_capacity = (idat_size + len) * 2;
                unsigned char *grown = realloc(idat, idat_capacity);
                if (!grown) { ok = fail(error, capacity, "out of memory"); break; }
                idat = grown;
            }
            memcpy(idat + idat_size, chunk, len);
            idat_size += len;
        } else if (memcmp(type, "IEND", 4) == 0) done = true;
        pos += 12 + len;
    }
    if (ok && (!seen_header || !idat_size)) ok = fail(error, capacity, "PNG has no image data");
    if (!ok) { free(idat); return false; }
    int channels = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : 4;
    size_t bits_per_pixel = (size_t)channels * (size_t)depth;
    size_t stride = (bits_per_pixel * (size_t)width + 7) / 8, raw_size = (stride + 1) * (size_t)height;
    unsigned char *raw = malloc(raw_size);
    if (!raw) { free(idat); return fail(error, capacity, "out of memory"); }
    size_t got = r2d_inflate(idat, idat_size, raw, raw_size, true);
    free(idat);
    if (got != raw_size) { free(raw); return fail(error, capacity, "corrupt PNG image data"); }
    /* unfilter in place */
    size_t bpp = bits_per_pixel < 8 ? 1 : bits_per_pixel / 8;
    for (int y = 0; y < height; ++y) {
        unsigned char *row = raw + (size_t)y * (stride + 1), *line = row + 1, *prev = y ? raw + (size_t)(y - 1) * stride : NULL;   /* the previous row was compacted */
        int filter = row[0];
        for (size_t x = 0; x < stride; ++x) {
            int a = x >= bpp ? line[x - bpp] : 0, b = prev ? prev[x] : 0, c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            switch (filter) {
                case 1: line[x] = (unsigned char)(line[x] + a); break;
                case 2: line[x] = (unsigned char)(line[x] + b); break;
                case 3: line[x] = (unsigned char)(line[x] + ((a + b) >> 1)); break;
                case 4: line[x] = (unsigned char)(line[x] + paeth(a, b, c)); break;
                default: break;
            }
        }
        memmove(raw + (size_t)y * stride, line, stride);   /* drop the filter byte: rows become contiguous */
    }
    unsigned char *pixels = malloc((size_t)width * (size_t)height * 4);
    if (!pixels) { free(raw); return fail(error, capacity, "out of memory"); }
    for (int y = 0; y < height; ++y) {
        const unsigned char *line = raw + (size_t)y * stride;
        for (int x = 0; x < width; ++x) {
            unsigned char *out = pixels + ((size_t)y * (size_t)width + (size_t)x) * 4;
            int v[4] = {0, 0, 0, 255};
            if (depth < 8) {
                int per_byte = 8 / depth, shift = 8 - depth * (x % per_byte + 1);
                int sample = (line[x / per_byte] >> shift) & ((1 << depth) - 1);
                if (color == 3) { v[0] = palette[sample * 4]; v[1] = palette[sample * 4 + 1]; v[2] = palette[sample * 4 + 2]; v[3] = palette[sample * 4 + 3]; }
                else { int g = sample * 255 / ((1 << depth) - 1); v[0] = v[1] = v[2] = g; if (trns_key && sample == key[0]) v[3] = 0; }
            } else {
                int step = depth / 8;
                const unsigned char *p = line + (size_t)x * (size_t)channels * (size_t)step;
                int s[4];
                for (int c = 0; c < channels; ++c) s[c] = p[c * step];   /* 16-bit: the high byte */
                int raw16[4];
                for (int c = 0; c < channels; ++c) raw16[c] = depth == 16 ? (p[c * 2] << 8) | p[c * 2 + 1] : p[c];
                switch (color) {
                    case 0: v[0] = v[1] = v[2] = s[0]; if (trns_key && raw16[0] == key[0]) v[3] = 0; break;
                    case 2: v[0] = s[0]; v[1] = s[1]; v[2] = s[2]; if (trns_key && raw16[0] == key[0] && raw16[1] == key[1] && raw16[2] == key[2]) v[3] = 0; break;
                    case 3: { int i = s[0] < 256 ? s[0] : 0; v[0] = palette[i * 4]; v[1] = palette[i * 4 + 1]; v[2] = palette[i * 4 + 2]; v[3] = palette[i * 4 + 3]; break; }
                    case 4: v[0] = v[1] = v[2] = s[0]; v[3] = s[1]; break;
                    default: v[0] = s[0]; v[1] = s[1]; v[2] = s[2]; v[3] = s[3]; break;
                }
            }
            out[0] = (unsigned char)v[0]; out[1] = (unsigned char)v[1]; out[2] = (unsigned char)v[2]; out[3] = (unsigned char)v[3];
        }
    }
    free(raw);
    img->width = width;
    img->height = height;
    img->pixels = pixels;
    return true;
}

/* ---------------------------------------------------------------------------
 * TGA (types 2/3 uncompressed, 10/11 RLE; 8/24/32 bits)
 * ------------------------------------------------------------------------- */
static bool decode_tga(r2d_image *img, const unsigned char *data, size_t size, char *error, size_t capacity) {
    if (size < 18) return fail(error, capacity, "truncated TGA");
    int id_len = data[0], cmap = data[1], type = data[2];
    int width = data[12] | (data[13] << 8), height = data[14] | (data[15] << 8), bits = data[16], desc = data[17];
    bool rle = type == 10 || type == 11, grey = type == 3 || type == 11;
    if (cmap || !(type == 2 || type == 3 || rle) || (bits != 8 && bits != 24 && bits != 32) || (grey && bits != 8) || (!grey && bits == 8))
        return fail(error, capacity, "unsupported TGA variant");
    if (width <= 0 || height <= 0 || width > R2D_IMAGE_MAX_DIMENSION || height > R2D_IMAGE_MAX_DIMENSION) return fail(error, capacity, "TGA dimensions out of range");
    bool top_down = (desc & 0x20) != 0;
    size_t pos = 18 + (size_t)id_len, bpp = (size_t)bits / 8, count = (size_t)width * (size_t)height;
    unsigned char *pixels = malloc(count * 4);
    if (!pixels) return fail(error, capacity, "out of memory");
    size_t i = 0;
    while (i < count) {
        size_t run = 1;
        bool repeat = false;
        if (rle) {
            if (pos >= size) { free(pixels); return fail(error, capacity, "truncated TGA"); }
            int packet = data[pos++];
            run = (size_t)(packet & 0x7f) + 1;
            repeat = (packet & 0x80) != 0;
        }
        if (run > count - i) run = count - i;
        for (size_t k = 0; k < run; ++k) {
            const unsigned char *p = data + pos;
            if (pos + bpp > size) { free(pixels); return fail(error, capacity, "truncated TGA"); }
            unsigned char *out = pixels + (i + k) * 4;
            if (grey) { out[0] = out[1] = out[2] = p[0]; out[3] = 255; }
            else { out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = bpp == 4 ? p[3] : 255; }
            if (!repeat || k + 1 == run) pos += bpp;
        }
        i += run;
    }
    if (!top_down) {   /* bottom-up rows: flip */
        for (int y = 0; y < height / 2; ++y) {
            unsigned char *a = pixels + (size_t)y * (size_t)width * 4, *b = pixels + (size_t)(height - 1 - y) * (size_t)width * 4;
            for (size_t k = 0; k < (size_t)width * 4; ++k) { unsigned char t = a[k]; a[k] = b[k]; b[k] = t; }
        }
    }
    img->width = width;
    img->height = height;
    img->pixels = pixels;
    return true;
}

/* ---------------------------------------------------------------------------
 * PPM / PGM (binary)
 * ------------------------------------------------------------------------- */
static bool decode_pnm(r2d_image *img, const unsigned char *data, size_t size, char *error, size_t capacity) {
    bool grey = data[1] == '5';
    size_t pos = 2;
    int values[3], n = 0;
    while (n < 3 && pos < size) {
        while (pos < size && (data[pos] == ' ' || data[pos] == '\n' || data[pos] == '\r' || data[pos] == '\t')) ++pos;
        if (pos < size && data[pos] == '#') { while (pos < size && data[pos] != '\n') ++pos; continue; }
        int v = 0;
        bool any = false;
        while (pos < size && data[pos] >= '0' && data[pos] <= '9') { v = v * 10 + (data[pos] - '0'); ++pos; any = true; }
        if (!any) return fail(error, capacity, "bad PNM header");
        values[n++] = v;
    }
    if (n < 3 || pos >= size) return fail(error, capacity, "bad PNM header");
    ++pos;   /* the single whitespace after maxval */
    int width = values[0], height = values[1], maxval = values[2];
    if (width <= 0 || height <= 0 || width > R2D_IMAGE_MAX_DIMENSION || height > R2D_IMAGE_MAX_DIMENSION || maxval != 255) return fail(error, capacity, "unsupported PNM");
    size_t count = (size_t)width * (size_t)height, channels = grey ? 1 : 3;
    if (size - pos < count * channels) return fail(error, capacity, "truncated PNM");
    unsigned char *pixels = malloc(count * 4);
    if (!pixels) return fail(error, capacity, "out of memory");
    for (size_t i = 0; i < count; ++i) {
        const unsigned char *p = data + pos + i * channels;
        pixels[i * 4] = p[0]; pixels[i * 4 + 1] = grey ? p[0] : p[1]; pixels[i * 4 + 2] = grey ? p[0] : p[2]; pixels[i * 4 + 3] = 255;
    }
    img->width = width;
    img->height = height;
    img->pixels = pixels;
    return true;
}

bool r2d_image_decode(r2d_image *img, const void *data, size_t size, char *error, size_t capacity) {
    if (!img) return false;
    memset(img, 0, sizeof(*img));
    const unsigned char *p = data;
    if (!p || size < 4) return fail(error, capacity, "no image data");
    if (memcmp(p, "\x89PNG", 4) == 0) return decode_png(img, p, size, error, capacity);
    if (p[0] == 'P' && (p[1] == '5' || p[1] == '6')) return decode_pnm(img, p, size, error, capacity);
    if (size >= 18 && (p[2] == 2 || p[2] == 3 || p[2] == 10 || p[2] == 11) && p[1] == 0) return decode_tga(img, p, size, error, capacity);
    return fail(error, capacity, "unknown image format (PNG, TGA and PPM/PGM are supported)");
}
bool r2d_image_load(r2d_image *img, const char *path, char *error, size_t capacity) {
    if (!img) return false;
    memset(img, 0, sizeof(*img));
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return fail(error, capacity, "cannot open the image file");
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (256 << 20)) { fclose(f); return fail(error, capacity, "empty or oversized image file"); }
    unsigned char *data = malloc((size_t)len);
    if (!data) { fclose(f); return fail(error, capacity, "out of memory"); }
    size_t got = fread(data, 1, (size_t)len, f);
    fclose(f);
    bool ok = got == (size_t)len && r2d_image_decode(img, data, got, error, capacity);
    if (got != (size_t)len) fail(error, capacity, "cannot read the image file");
    free(data);
    return ok;
}
void r2d_image_free(r2d_image *img) {
    if (!img) return;
    free(img->pixels);
    memset(img, 0, sizeof(*img));
}
bool r2d_image_solid(r2d_image *img, int width, int height, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    memset(img, 0, sizeof(*img));
    if (width <= 0 || height <= 0) return false;
    img->pixels = malloc((size_t)width * (size_t)height * 4);
    if (!img->pixels) return false;
    for (size_t i = 0; i < (size_t)width * (size_t)height; ++i) { img->pixels[i * 4] = r; img->pixels[i * 4 + 1] = g; img->pixels[i * 4 + 2] = b; img->pixels[i * 4 + 3] = a; }
    img->width = width;
    img->height = height;
    return true;
}
