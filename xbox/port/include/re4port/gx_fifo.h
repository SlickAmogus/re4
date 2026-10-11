/* gx_fifo.h — the immediate-mode vertex FIFO of the port (CT-08).
 *
 * The game's include/gx.h declares `extern volatile WGPipe GXWGFifo[];` (the GameCube write-gather pipe
 * at 0xCC008000) and writes vertex components with `GXWGFifo->f32 = x;` etc., either through its inline
 * GXPosition3f32/GXColor4u8/... helpers or directly (mes.cpp, dbmodule.cpp). That header cannot be
 * edited, so re4_xbox.h (force-included before every game header) defines the header's guard
 * `__GXVERT_H__` to skip the game's block and includes this file instead. It provides:
 *
 *   - Re4GxFifo: a WGPipe look-alike whose members u8..f64 are proxy objects. Assigning to one appends
 *     the value, in native byte order, to the primitive buffer opened by the last GXBegin.
 *   - `#define GXWGFifo (&re4_gx_fifo)` so `GXWGFifo->s16 = x;` compiles unchanged and appends 2 bytes.
 *   - the inline writers with the signatures of include/gx.h.
 *
 * GXBegin(prim, vtxfmt, nverts) computes the byte size of one vertex from the VCD/VAT (direct attributes
 * only) and sets the cursor `end` to buf + nverts*size; the write that reaches `end` completes the
 * primitive (decode + draw, see gx_dl.cpp). A write with no open primitive, or past its end, goes to
 * re4_gx_fifo_overflow() which logs once and drops it. There is no GXEnd in the game.
 *
 * This header is seen before include/types.h, so it uses only built-in types and no standard headers.
 * The game units are all C++; C units (xbox_compat/) never touch the FIFO.
 */
#ifndef RE4PORT_GX_FIFO_H
#define RE4PORT_GX_FIFO_H

#ifdef __cplusplus

extern "C" {
struct Re4GxFifoCursor {
    unsigned char* wr;    /* next byte to write in the primitive buffer */
    unsigned char* end;   /* wr == end: the primitive is complete (or nothing is open) */
};
extern struct Re4GxFifoCursor re4_gx_fifo_cursor;
void re4_gx_fifo_complete(void);                        /* gx_dl.cpp: decode + draw the pending primitive */
void re4_gx_fifo_overflow(const void* p, unsigned n);   /* gx_dl.cpp: no open primitive: log once, drop */
}

template <typename T>
struct Re4GxFifoLane {
    void operator=(T v)
    {
        Re4GxFifoCursor& c = re4_gx_fifo_cursor;
        if ((unsigned) (c.end - c.wr) >= sizeof(T)) {
            __builtin_memcpy(c.wr, &v, sizeof(T));   /* native order; unaligned stores are fine on x86 */
            c.wr += sizeof(T);
            if (c.wr == c.end) re4_gx_fifo_complete();
        } else {
            re4_gx_fifo_overflow(&v, (unsigned) sizeof(T));
        }
    }
};

/* Same member names as the game's `union WGPipe`; declared with built-in types because the u8/s16/f32
 * typedefs of include/types.h have not been seen yet (and the members would shadow them anyway). */
struct Re4GxFifo {
    Re4GxFifoLane<unsigned char>       u8;
    Re4GxFifoLane<unsigned short>      u16;
    Re4GxFifoLane<unsigned int>        u32;
    Re4GxFifoLane<unsigned long long>  u64;
    Re4GxFifoLane<signed char>         s8;
    Re4GxFifoLane<short>               s16;
    Re4GxFifoLane<int>                 s32;
    Re4GxFifoLane<long long>           s64;
    Re4GxFifoLane<float>               f32;
    Re4GxFifoLane<double>              f64;
};
typedef Re4GxFifo WGPipe;
extern Re4GxFifo re4_gx_fifo;
#define GXWGFifo (&re4_gx_fifo)

/* The inline writers of include/gx.h (dolphin/gx/GXVert.h), same names and parameter widths. */
static inline void GXPosition3f32(float x, float y, float z) { GXWGFifo->f32 = x; GXWGFifo->f32 = y; GXWGFifo->f32 = z; }
static inline void GXPosition3s16(short x, short y, short z) { GXWGFifo->s16 = x; GXWGFifo->s16 = y; GXWGFifo->s16 = z; }
static inline void GXPosition2u16(unsigned short x, unsigned short y) { GXWGFifo->u16 = x; GXWGFifo->u16 = y; }
static inline void GXColor4u8(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    GXWGFifo->u8 = r; GXWGFifo->u8 = g; GXWGFifo->u8 = b; GXWGFifo->u8 = a;
}
static inline void GXNormal3f32(float x, float y, float z) { GXWGFifo->f32 = x; GXWGFifo->f32 = y; GXWGFifo->f32 = z; }
static inline void GXNormal3s8(signed char x, signed char y, signed char z) { GXWGFifo->s8 = x; GXWGFifo->s8 = y; GXWGFifo->s8 = z; }
static inline void GXTexCoord2f32(float s, float t) { GXWGFifo->f32 = s; GXWGFifo->f32 = t; }
static inline void GXTexCoord2s16(short s, short t) { GXWGFifo->s16 = s; GXWGFifo->s16 = t; }
static inline void GXMatrixIndex1u8(unsigned char idx) { GXWGFifo->u8 = idx; }

#endif /* __cplusplus */
#endif /* RE4PORT_GX_FIFO_H */
