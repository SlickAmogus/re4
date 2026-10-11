/* xbox/port/os/memfill.c -- CT-02: memset_asm / memclr_asm / mtxclr_asm (src/game/memset_2.cpp) in C.
 * See memfill.h for the semantics kept from the asm. */
#include "memfill.h"
#include <string.h>

void memset_asm(void* dst, int c, unsigned int n)
{
    if (n == 0) {
        return;
    }
    memset(dst, c & 0xFF, n);
}

void memclr_asm(void* dst, unsigned int n)
{
    if (n == 0) {
        return;
    }
    memset(dst, 0, n);
}

void mtxclr_asm(void* dst, unsigned int count)
{
    if (count == 0) {
        return;
    }
    memset(dst, 0, count * 12u * 4u);
}
