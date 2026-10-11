/* gx_null_log.h — inspection API of the null backend (xbox/port/gx/gx_null.cpp) for tests (CT-08).
 *
 * The null backend draws nothing; it appends one record per vtable call to a ring, keeps a copy of the
 * vertices of every draw since the last reset and the matrices of the last set_matrices.
 */
#ifndef RE4PORT_GX_NULL_LOG_H
#define RE4PORT_GX_NULL_LOG_H

#include <stdint.h>
#include <stddef.h>
#include "re4port/gx_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

enum GxNullKind {
    GX_NULL_INIT = 1, GX_NULL_SHUTDOWN, GX_NULL_BEGIN_FRAME, GX_NULL_END_FRAME,
    GX_NULL_VIEWPORT, GX_NULL_SCISSOR, GX_NULL_PIPELINE, GX_NULL_MATRICES, GX_NULL_DRAW,
    GX_NULL_CREATE_TEX, GX_NULL_DESTROY_TEX, GX_NULL_BIND_TEX,
    GX_NULL_CREATE_RT, GX_NULL_SET_RT, GX_NULL_COPY_TO_TEX, GX_NULL_CLEAR, GX_NULL_PEEK_Z
};

typedef struct GxNullRecord {
    uint8_t  kind;        /* GxNullKind */
    uint8_t  prim;        /* DRAW: GX_PRIM_* as handed to the backend (already expanded) */
    uint16_t pad;
    uint32_t count;       /* DRAW: vertex count; CLEAR: rgba; BIND/CREATE/DESTROY: handle; VIEWPORT/SCISSOR: 0 */
    uint64_t hash;        /* PIPELINE: key hash; otherwise 0 */
    int32_t  map;         /* BIND_TEX: map; COPY_TO_TEX/CLEAR: flags; PEEK_Z: x */
    float    f[6];        /* VIEWPORT: l t w h nz fz; SCISSOR: l t w h; BIND: wrap_s wrap_t min mag bias aniso */
} GxNullRecord;

void   gx_null_log_reset(void);                 /* clears records, vertices, counters */
size_t gx_null_log_count(void);                 /* records since reset (capped at the ring size) */
const GxNullRecord* gx_null_log_get(size_t i);  /* i-th record since reset, or NULL */
size_t gx_null_log_count_kind(int kind);
const GxNullRecord* gx_null_log_last_kind(int kind);  /* most recent record of that kind, or NULL */

/* All vertices passed to draw() since the last reset, in order (triangles are consecutive triples). */
const GxVertex* gx_null_vertices(size_t* count);
size_t gx_null_triangles(void);                 /* total GX_PRIM_TRIANGLES vertices / 3 since reset */

/* Copies from the last set_matrices(); proj_type is -1 when none happened since reset. */
int  gx_null_last_proj_type(void);
const float (*gx_null_last_proj(void))[4];
const float (*gx_null_last_pos_mtx(int slot))[4];   /* slot 0..9 */
const float (*gx_null_last_tex_mtx(int slot))[4];   /* slot 0..10 */

/* The last pipeline key (a copy; its tev pointer still points into the state machine). */
const GxPipelineKey* gx_null_last_pipeline(void);

#ifdef __cplusplus
}
#endif
#endif /* RE4PORT_GX_NULL_LOG_H */
