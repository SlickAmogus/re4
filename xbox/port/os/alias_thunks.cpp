// alias_thunks.cpp — definitions the game expects from the GameCube toolchain that cannot be expressed as
// a plain linker alias (xbox/tools/link_resolve.py handles the cdecl ones with /alternatename).
//
// * Room code calls two __thiscall members through free-function declarations bound with asm labels
//   (`void EvtFlgOnStatus(Event*, u32) asm("FlgOnStatus__5EventUl")`). GCC on PowerPC passes `this` as the
//   first argument, so that worked; on x86 a member uses __thiscall, so forward explicitly.
// * The SN linker script labels the .ctors/.dtors lists _ctors/_dtors (null terminated); each REL's
//   _prolog walks them. On Xbox the CRT runs every static constructor at startup already, so the lists
//   are empty.
#include "types.h"
#include "event.h"
#include "obj02.h"

void EvtFlgOnStatus(Event* e, u32 no)
{
    e->FlgOnStatus(no);
}

void cObjScrSetCallBack(cObj* o, void (*func)(cObj*))
{
    static_cast<cObjScr*>(o)->SetCallBack(func);
}

void (*_ctors[])(void) = { 0 };
void (*_dtors[])(void) = { 0 };
