// gx_vertex.h — the interleaved vertex (GxVertex, declared in re4port/gx_shim.h so backends see it) and
// small growable CPU buffers used by the interpreter (CT-08). Plain C-style containers: no STL in the
// decode path, malloc/realloc growth, never shrink.
#ifndef RE4PORT_GX_VERTEX_H
#define RE4PORT_GX_VERTEX_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "re4port/gx_shim.h"

// RGBA8 as GxVertex::col stores it: r in the lowest byte, i.e. bytes r,g,b,a in memory.
#define GX_RGBA8(r, g, b, a) ((uint32_t) (uint8_t) (r) | ((uint32_t) (uint8_t) (g) << 8) | \
                              ((uint32_t) (uint8_t) (b) << 16) | ((uint32_t) (uint8_t) (a) << 24))
#define GX_RGBA8_R(c) ((uint8_t) ((c) & 0xFF))
#define GX_RGBA8_G(c) ((uint8_t) (((c) >> 8) & 0xFF))
#define GX_RGBA8_B(c) ((uint8_t) (((c) >> 16) & 0xFF))
#define GX_RGBA8_A(c) ((uint8_t) (((c) >> 24) & 0xFF))

struct GxVertexBuffer {
    GxVertex* data;
    size_t    count;
    size_t    cap;
};

static inline void gx_vb_init(GxVertexBuffer* b) { b->data = 0; b->count = 0; b->cap = 0; }
static inline void gx_vb_free(GxVertexBuffer* b) { free(b->data); gx_vb_init(b); }
static inline void gx_vb_clear(GxVertexBuffer* b) { b->count = 0; }

static inline int gx_vb_reserve(GxVertexBuffer* b, size_t n)
{
    if (b->cap >= n) return 1;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < n) cap *= 2;
    GxVertex* p = (GxVertex*) realloc(b->data, cap * sizeof(GxVertex));
    if (!p) return 0;
    b->data = p;
    b->cap = cap;
    return 1;
}

// Appends a copy of v; returns 0 on allocation failure (the caller drops the primitive and logs).
static inline int gx_vb_push(GxVertexBuffer* b, const GxVertex* v)
{
    if (b->count == b->cap && !gx_vb_reserve(b, b->count + 1)) return 0;
    b->data[b->count++] = *v;
    return 1;
}

// Growable byte buffer (the immediate-mode primitive buffer, display-list recording).
struct GxByteBuffer {
    uint8_t* data;
    size_t   count;
    size_t   cap;
};

static inline void gx_bb_init(GxByteBuffer* b) { b->data = 0; b->count = 0; b->cap = 0; }
static inline void gx_bb_free(GxByteBuffer* b) { free(b->data); gx_bb_init(b); }

static inline int gx_bb_reserve(GxByteBuffer* b, size_t n)
{
    if (b->cap >= n) return 1;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < n) cap *= 2;
    uint8_t* p = (uint8_t*) realloc(b->data, cap);
    if (!p) return 0;
    b->data = p;
    b->cap = cap;
    return 1;
}

#endif // RE4PORT_GX_VERTEX_H
