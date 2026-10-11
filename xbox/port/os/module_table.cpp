// module_table.cpp — statically linked REL modules (xbox/docs/07-COMPILER.md "Linking the modules").
//
// On the GameCube read.cpp / main_sub.cpp load a module's REL out of its .drs archive and call
// OSLink(&header->info, bss), which relocates it and runs its _prolog. Here every module is already in
// the image (tools/isolate_modules.py renamed its entry points to re4mod_<name>_prolog/_epilog), so
// OSLink only reads the module id from the REL header the game loaded and calls the matching prolog.
// The asset converter must therefore keep at least the REL header (OSModuleInfo, 0x40 bytes) inside
// each .drs; the code and relocation sections can be dropped.
#include "re4port/re4_xbox.h"
#include <string.h>

extern "C" {

#define RE4_MODULE(name, id) void re4mod_##name##_prolog(void); void re4mod_##name##_epilog(void);
#include "module_list.inc"
#undef RE4_MODULE

struct Re4Module {
    const char* name;
    unsigned id;
    void (*prolog)(void);
    void (*epilog)(void);
    int linked;
};

static Re4Module g_modules[] = {
#define RE4_MODULE(name, id) { #name, id, re4mod_##name##_prolog, re4mod_##name##_epilog, 0 },
#include "module_list.inc"
#undef RE4_MODULE
};

static unsigned module_id(const void* info)
{
    // OSModuleInfo starts with u32 id. The REL header may still be big-endian (raw disc bytes) or
    // already converted; module ids are small, so a value above 0xFFFF means it needs swapping.
    unsigned v;
    memcpy(&v, info, 4);
    if (v > 0xFFFF) v = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
    return v;
}

static Re4Module* find_module(unsigned id)
{
    for (unsigned i = 0; i < sizeof g_modules / sizeof g_modules[0]; i++) {
        if (g_modules[i].id == id) return &g_modules[i];
    }
    return 0;
}

// BOOL OSLink(OSModuleInfo* newModule, void* bss)
int OSLink(void* info, void* bss)
{
    (void) bss;   // module statics live in the image's own .bss
    unsigned id = module_id(info);
    Re4Module* m = find_module(id);
    if (!m) {
        re4_log("OSLink: unknown REL module id %u\n", id);
        return 0;
    }
    re4_log("OSLink: %s (id %u)%s\n", m->name, id, m->linked ? " (relink)" : "");
    m->linked = 1;
    m->prolog();
    return 1;
}

// BOOL OSUnlink(OSModuleInfo* oldModule)
int OSUnlink(void* info)
{
    Re4Module* m = find_module(module_id(info));
    if (!m) return 0;
    if (m->linked) m->epilog();
    m->linked = 0;
    return 1;
}

} // extern "C"
