/* re4port/skin_calcsk1_x2.inc -- CT-02: spliced by xbox/tools/prep.py in place of the asm body of
 * CalcSk1_x2 in src/game/trans.cpp (included at file scope).  Implementation: xbox/port/gx/skin.cpp. */
#include "re4port/skin.h"

void CalcSk1_x2(void* dst, void* src, u32 n)
{
    re4_calcsk1_s8(dst, src, n, re4_gqr6);
}
