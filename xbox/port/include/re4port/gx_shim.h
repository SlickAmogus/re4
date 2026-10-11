/* gx_shim.h — port-internal interface of the GX reimplementation (xbox/port/gx/).
 *
 * The game includes its own include/gx.h (plain-int enum parameters, inline FIFO writers). This header
 * is for the port layer: the backend vtable, the state container and the interpreter entry points.
 * See xbox/docs/02-GRAPHICS.md.
 */
#ifndef RE4PORT_GX_SHIM_H
#define RE4PORT_GX_SHIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Primitive types as GX encodes them in the FIFO opcode (GX_DRAW_* | vtxfmt). */
enum GxPrim {
    GX_PRIM_QUADS = 0x80, GX_PRIM_TRIANGLES = 0x90, GX_PRIM_TRISTRIP = 0x98, GX_PRIM_TRIFAN = 0xA0,
    GX_PRIM_LINES = 0xA8, GX_PRIM_LINESTRIP = 0xB0, GX_PRIM_POINTS = 0xB8
};

/* Interleaved vertex the interpreter produces for the backends. */
typedef struct GxVertex {
    float    pos[3];
    float    nrm[3];
    uint32_t col[2];      /* RGBA8 as in the stream */
    float    uv[8][2];
    uint8_t  mtxidx;      /* position matrix index (0..30 step 3 in GX terms; stored as 0..9) */
    uint8_t  texmtxidx[8];
} GxVertex;

/* A converted texture blob (xbox/docs/04-DATA.md section 5). */
#define RE4_IMG_MAGIC 0x58344552u /* 'RE4X' little-endian */
typedef struct ConvertedImage {
    uint32_t magic;
    uint16_t width, height;
    uint8_t  nv2a_format;  /* SZ_* code (A8R8G8B8, R5G6B5, A1R5G5B5, A8Y8, Y8, DXT1 ...) */
    uint8_t  mip_count;
    uint8_t  swizzled;
    uint8_t  pad;
    uint32_t data_size;
    /* image bytes follow, mips appended */
} ConvertedImage;

typedef uint32_t GxTexHandle;
typedef uint32_t GxRtHandle;

/* Resolved pipeline state the backend consumes. Filled by gx_pipeline.cpp from the GX state.
 * Backend-neutral: the host compiles it to GLSL, the Xbox to register combiners. */
typedef struct GxPipelineKey {
    uint64_t hash;              /* of everything below + the TEV program */
    uint8_t  num_tev_stages;
    uint8_t  num_texgens;
    uint8_t  num_chans;
    uint8_t  cull_mode;
    uint8_t  blend_type, blend_src, blend_dst, blend_op;
    uint8_t  z_enable, z_func, z_update;
    uint8_t  alpha_comp0, alpha_ref0, alpha_op, alpha_comp1, alpha_ref1;
    uint8_t  color_update, alpha_update, dst_alpha_enable, dst_alpha;
    uint8_t  fog_type;
    float    fog_start, fog_end, fog_near, fog_far;
    uint32_t fog_color;
    const struct GxTevProgram* tev;   /* owned by the state machine, stable until GXSetNumTevStages etc. */
} GxPipelineKey;

typedef struct GxBackend {
    const char* name;
    int  (*init)(int width, int height);
    void (*shutdown)(void);
    void (*begin_frame)(void);
    void (*end_frame)(void);                 /* present */
    void (*set_viewport)(float l, float t, float w, float h, float nz, float fz);
    void (*set_scissor)(uint32_t l, uint32_t t, uint32_t w, uint32_t h);
    void (*set_pipeline)(const GxPipelineKey* key);
    void (*set_matrices)(const float proj[4][4], int proj_type, const float (*pos)[3][4], const float (*nrm)[3][4], const float (*tex)[3][4]);
    void (*draw)(int prim, const GxVertex* v, size_t count);
    GxTexHandle (*create_texture)(const ConvertedImage* img);
    void (*destroy_texture)(GxTexHandle h);
    void (*bind_texture)(int map, GxTexHandle h, int wrap_s, int wrap_t, int min_filt, int mag_filt, float lod_bias, int aniso);
    GxRtHandle (*create_render_target)(int w, int h, int with_depth);
    void (*set_render_target)(GxRtHandle rt);   /* 0 = back buffer */
    void (*copy_to_texture)(GxTexHandle dst, int left, int top, int w, int h, int clear);
    void (*clear)(uint32_t rgba, uint32_t z, int color, int depth);
    uint32_t (*peek_z)(int x, int y);
} GxBackend;

extern const GxBackend* gx_backend;      /* selected at init: gx_host_gl, gx_nv2a or gx_null */
void gx_select_backend(const GxBackend* b);

extern const GxBackend gx_null_backend;  /* records calls, draws nothing (tests, CT-08) */

/* Interpreter: decode a FIFO/display-list stream using the current VCD/VAT/arrays and draw it. */
void gx_run_stream(const uint8_t* stream, size_t bytes, int big_endian);

#ifdef __cplusplus
}
#endif
#endif /* RE4PORT_GX_SHIM_H */
