/* xbox/port/os/memfill.h -- CT-02: C versions of Capcom's hand-written fill helpers
 * (src/game/memset_2.cpp, declared extern "C" in include/main_mem.h).
 *
 *   memset_asm(dst, c, n)   fill n bytes with (u8) c
 *   memclr_asm(dst, n)      zero n bytes
 *   mtxclr_asm(dst, count)  zero `count` 3x4 float matrices (12 words each; no callers in src/game)
 *
 * The originals fill 16-byte blocks of words when n > 15 and dst is 4-byte aligned, then a byte tail;
 * the observable result is a plain memset in every case, so these are memset wrappers.  Edge cases kept
 * from the asm: n == 0 is a no-op; `c` is truncated to its low byte.  mtxclr_asm(p, 0) looped 2^32 times
 * on the PPC (mtctr 0); the port treats it as a no-op. */
#ifndef RE4PORT_MEMFILL_H
#define RE4PORT_MEMFILL_H

#ifdef __cplusplus
extern "C" {
#endif

void memset_asm(void* dst, int c, unsigned int n);
void memclr_asm(void* dst, unsigned int n);
void mtxclr_asm(void* dst, unsigned int count);

#ifdef __cplusplus
}
#endif
#endif /* RE4PORT_MEMFILL_H */
