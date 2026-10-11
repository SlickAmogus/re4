/* gx_shim.h — port-internal interface of the GX reimplementation (xbox/port/gx/).
 *
 * The game includes its own include/gx.h (plain-int enum parameters, inline FIFO writers). This header
 * is for the port layer: the backend vtable, the state container and the interpreter entry points.
 * See xbox/docs/02-GRAPHICS.md (sections 2, 3, 7) and the "Implementation notes (CT-08)" there.
 */
#ifndef RE4PORT_GX_SHIM_H
#define RE4PORT_GX_SHIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Primitive types as GX encodes them in the FIFO opcode (GX_DRAW_* | vtxfmt). The interpreter expands
 * quads, strips and fans, so a backend only ever sees GX_PRIM_TRIANGLES, GX_PRIM_LINES, GX_PRIM_POINTS. */
enum GxPrim {
    GX_PRIM_QUADS = 0x80, GX_PRIM_TRIANGLES = 0x90, GX_PRIM_TRISTRIP = 0x98, GX_PRIM_TRIFAN = 0xA0,
    GX_PRIM_LINES = 0xA8, GX_PRIM_LINESTRIP = 0xB0, GX_PRIM_POINTS = 0xB8
};

/* Interleaved vertex the interpreter produces for the backends.
 * col[] holds RGBA8 with r in the lowest byte of the memory word (bytes r,g,b,a in memory, which is how
 * GX itself streams RGBA8); use GX_RGBA8()/GX_RGBA8_R..A in gx_vertex.h.
 * mtxidx is the position/normal matrix slot 0..9 (GX_PNMTXn / 3), from the vertex's GX_VA_PNMTXIDX when the
 * VCD has one, otherwise from GXSetCurrentMtx. texmtxidx[i] is the texture matrix slot for texcoord i:
 * 0..9 = GX_TEXMTX0..9, 10 = GX_IDENTITY (from GX_VA_TEXnMTXIDX or the texgen's matrix). */
typedef struct GxVertex {
    float    pos[3];
    float    nrm[3];
    uint32_t col[2];      /* RGBA8, bytes r,g,b,a */
    float    uv[8][2];
    uint8_t  mtxidx;
    uint8_t  texmtxidx[8];
    uint8_t  pad[3];
} GxVertex;

/* A converted texture blob (xbox/docs/04-DATA.md section 5). data_size == 0 means "no pixel data": the
 * state machine passes such a header to create_texture for an EFB-copy destination (GXCopyTex), which
 * the backend must allocate as a writable texture of width x height. */
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

struct GxTevProgram;

/* Resolved pipeline state the backend consumes. Filled by gx_pipeline.cpp from the GX state.
 * Backend-neutral: the host compiles it to GLSL, the Xbox to register combiners.
 * `hash` covers every field below except the fog distances/colour (uniforms) plus the program part of
 * `tev` (stages, texgens, channel controls, indirect stages, swap tables); the uniform part of `tev`
 * (konst colours, TEV registers, channel colours, lights, indirect matrices) is excluded, so a backend
 * caches compiled programs by hash and refreshes constants on every set_pipeline. */
typedef struct GxPipelineKey {
    uint64_t hash;
    uint8_t  num_tev_stages;
    uint8_t  num_texgens;
    uint8_t  num_chans;
    uint8_t  num_ind_stages;
    uint8_t  cull_mode;
    uint8_t  blend_type, blend_src, blend_dst, blend_op;
    uint8_t  z_enable, z_func, z_update, z_comp_loc;
    uint8_t  alpha_comp0, alpha_ref0, alpha_op, alpha_comp1, alpha_ref1;
    uint8_t  color_update, alpha_update, dst_alpha_enable, dst_alpha;
    uint8_t  dither;
    uint8_t  fog_type;
    uint8_t  line_width;        /* GXSetLineWidth, 1/6 pixel units */
    uint8_t  pixel_fmt, z_fmt;
    float    fog_start, fog_end, fog_near, fog_far;
    uint32_t fog_color;         /* RGBA8, bytes r,g,b,a */
    const struct GxTevProgram* tev;   /* owned by the state machine, rebuilt at the next draw after a change */
} GxPipelineKey;

/* Matrix palettes handed to set_matrices: pos/nrm point to GX_PNMTX_COUNT (10) 3x4 matrices (slot = id/3),
 * tex to GX_TEXMTX_COUNT (11) 3x4 matrices (slots 0..9 = GX_TEXMTX0..9, slot 10 = GX_IDENTITY; a 2x4 load
 * leaves row 2 as 0 0 1 0). The projection is the 4x4 the game gave GXSetProjection; proj_type is
 * GX_PERSPECTIVE (0) or GX_ORTHOGRAPHIC (1). The backend applies GX's clip-space convention. */
#define GX_PNMTX_COUNT  10
#define GX_TEXMTX_COUNT 11

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
void gx_select_backend(const GxBackend* b);   /* also runs gx_init() the first time */

extern const GxBackend gx_nv2a_backend;  /* Xbox: pbkit / register combiners (gx_nv2a.cpp, CT-15) */
extern const GxBackend gx_null_backend;  /* records calls, draws nothing (tests, CT-08); see gx_null_log.h */

/* State machine (gx_state.cpp). gx_init() puts the state at the SDK's power-on defaults (__GXInitGX);
 * gx_select_backend calls it, tests call it to reset between cases. gx_flush() draws a partial
 * immediate-mode primitive, if any, and pushes pending dirty state to the backend. */
void gx_init(void);
void gx_flush(void);

/* Interpreter (gx_dl.cpp): decode a FIFO/display-list stream using the current VCD/VAT/arrays and draw it.
 * big_endian selects the byte order of the stream's u16 counts and indices AND of the array data it
 * dereferences (raw GameCube data is big-endian, converted assets and CPU-built lists are native). */
void gx_run_stream(const uint8_t* stream, size_t bytes, int big_endian);

/* Byte order GXCallDisplayList assumes for the lists the game hands it (default 0 = native; set 1 when
 * running raw GameCube model data). Immediate-mode primitives are always native. */
void gx_set_dl_big_endian(int big_endian);
int  gx_get_dl_big_endian(void);

#ifdef __cplusplus
}
#endif
#endif /* RE4PORT_GX_SHIM_H */
