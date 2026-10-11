// gx_null.cpp — backend that draws nothing and logs what it was asked (tests, early bring-up).
#include "re4port/re4_xbox.h"
#include "re4port/gx_shim.h"

const GxBackend* gx_backend = 0;

extern "C" void gx_select_backend(const GxBackend* b)
{
    gx_backend = b;
    re4_log("GX backend: %s\n", b ? b->name : "(none)");
}

static int  null_init(int w, int h) { re4_log("gx_null: init %dx%d\n", w, h); return 0; }
static void null_shutdown(void) { }
static void null_begin_frame(void) { }
static void null_end_frame(void) { }
static void null_set_viewport(float, float, float, float, float, float) { }
static void null_set_scissor(uint32_t, uint32_t, uint32_t, uint32_t) { }
static void null_set_pipeline(const GxPipelineKey*) { }
static void null_set_matrices(const float[4][4], int, const float (*)[3][4], const float (*)[3][4], const float (*)[3][4]) { }
static void null_draw(int prim, const GxVertex*, size_t n) { re4_log_once("null_draw", "gx_null: draw prim 0x%02x x%u (first only)\n", prim, (unsigned) n); }
static GxTexHandle null_create_texture(const ConvertedImage*) { static GxTexHandle next = 1; return next++; }
static void null_destroy_texture(GxTexHandle) { }
static void null_bind_texture(int, GxTexHandle, int, int, int, int, float, int) { }
static GxRtHandle null_create_rt(int, int, int) { static GxRtHandle next = 1; return next++; }
static void null_set_rt(GxRtHandle) { }
static void null_copy_to_texture(GxTexHandle, int, int, int, int, int) { }
static void null_clear(uint32_t, uint32_t, int, int) { }
static uint32_t null_peek_z(int, int) { return 0xFFFFFF; }

const GxBackend gx_null_backend = {
    "null",
    null_init, null_shutdown, null_begin_frame, null_end_frame,
    null_set_viewport, null_set_scissor, null_set_pipeline, null_set_matrices, null_draw,
    null_create_texture, null_destroy_texture, null_bind_texture,
    null_create_rt, null_set_rt, null_copy_to_texture, null_clear, null_peek_z,
};
