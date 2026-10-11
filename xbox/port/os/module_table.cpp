// module_table.cpp — statically linked REL modules and their lifecycle
// (xbox/docs/07-COMPILER.md "Linking the modules", cloud-tasks/CT-12).
//
// On the GameCube read.cpp / roomdata.cpp load a module's REL out of its .drs archive (or Rel/*.rel),
// call OSLink(&header->info, bss), which relocates it, and then call DLL_PROLOG(header)(). Here every
// module is already in the image: tools/isolate_modules.py renamed its symbols (entry points become
// re4mod_<name>_prolog/_epilog), and the asset converter keeps only the REL's 0x4C-byte header, from
// which OSLink reads the module id and into which it writes the entry points.
//
// Lifecycle: a REL read from disc starts with its initial .data, zeroed .bss and runs its static
// constructors from _prolog. tools/gen_modseg.py puts each module's .data / .bss / dynamic initialisers
// into contiguous per-module ranges (re4seg_* markers), so:
//   * re4_modules_init() (boot, before the game runs) snapshots every module's .data;
//   * a *fresh* OSLink restores .data from the snapshot, zeroes .bss and runs the initialisers;
//   * OSUnlink saves the live .data/.bss; an OSLink of the same header that was not re-read from disc
//     since (cRoomData::stopRelData / restartRelData) restores that saved state instead.
#include "re4port/re4_xbox.h"
#include <string.h>
#include <stdlib.h>

extern "C" {

int re4_dvd_overwritten_since(const void* p, unsigned gen);
extern unsigned re4_dvd_generation;

#define RE4_MODULE(name, id)                                                                          \
    void re4mod_##name##_prolog(void);                                                                \
    void re4mod_##name##_epilog(void);                                                                \
    extern char re4m_##name##_re4seg_data_begin, re4m_##name##_re4seg_data_end;                       \
    extern char re4m_##name##_re4seg_bss_begin, re4m_##name##_re4seg_bss_end;                         \
    extern void (*re4m_##name##_re4seg_ct_begin)(void), (*re4m_##name##_re4seg_ct_end)(void);
#include "module_list.inc"
#undef RE4_MODULE

typedef void (*CtorFn)(void);

struct Re4Module {
    const char* name;
    unsigned id;
    void (*prolog)(void);
    void (*epilog)(void);
    char* data_begin;
    char* data_end;
    char* bss_begin;
    char* bss_end;
    CtorFn* ct_begin;
    CtorFn* ct_end;
    // runtime
    int linked;
    char* data_snapshot;          // initial .data, taken at boot
    char* saved;                  // .data + .bss saved by OSUnlink
    const void* unlinked_header;  // header address at OSUnlink
    unsigned unlink_gen;          // re4_dvd_generation at OSUnlink
};

static Re4Module g_modules[] = {
#define RE4_MODULE(name, id)                                                                          \
    { #name, id, re4mod_##name##_prolog, re4mod_##name##_epilog,                                      \
      &re4m_##name##_re4seg_data_begin, &re4m_##name##_re4seg_data_end,                               \
      &re4m_##name##_re4seg_bss_begin, &re4m_##name##_re4seg_bss_end,                                 \
      &re4m_##name##_re4seg_ct_begin, &re4m_##name##_re4seg_ct_end, 0, 0, 0, 0, 0 },
#include "module_list.inc"
#undef RE4_MODULE
};
enum { NMODULES = sizeof g_modules / sizeof g_modules[0] };

static unsigned data_size(const Re4Module* m) { return (unsigned) (m->data_end - m->data_begin); }
static unsigned bss_size(const Re4Module* m) { return (unsigned) (m->bss_end - m->bss_begin); }

// Boot: snapshot every module's initial .data (call before re4_main).
void re4_modules_init(void)
{
    unsigned total_data = 0, total_bss = 0;
    for (int i = 0; i < NMODULES; i++) {
        Re4Module* m = &g_modules[i];
        if (m->data_end < m->data_begin || m->bss_end < m->bss_begin) {
            re4_log("modules: %s has inverted section markers; lifecycle disabled for it\n", m->name);
            m->data_end = m->data_begin;
            m->bss_end = m->bss_begin;
            continue;
        }
        unsigned n = data_size(m);
        total_data += n;
        total_bss += bss_size(m);
        if (n) {
            m->data_snapshot = (char*) malloc(n);
            if (m->data_snapshot) memcpy(m->data_snapshot, m->data_begin, n);
        }
    }
    re4_log("modules: %d, .data %u bytes (snapshotted), .bss %u bytes\n", NMODULES, total_data, total_bss);
}

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
    for (int i = 0; i < NMODULES; i++) {
        if (g_modules[i].id == id) return &g_modules[i];
    }
    return 0;
}

// OSModuleHeader (include/dolphin/os/OSModule.h, version 3, 0x4C bytes). The converter keeps it
// little-endian in place of the REL (re4conv container.rel_header_le).
struct ModuleHeader {
    unsigned id, link_next, link_prev, numSections, sectionInfoOffset, nameOffset, nameSize, version;
    unsigned bssSize, relOffset, impOffset, impSize;
    unsigned char prologSection, epilogSection, unresolvedSection, bssSection;
    unsigned prolog, epilog, unresolved;
    unsigned align, bssAlign, fixSize;
};
static_assert(sizeof(ModuleHeader) == 0x4C, "OSModuleHeader v3");

void re4_module_unresolved(void)
{
    re4_fatal(__FILE__, __LINE__, "a REL module called an unresolved import");
}

static void fresh_state(Re4Module* m)
{
    if (m->data_snapshot) memcpy(m->data_begin, m->data_snapshot, data_size(m));
    memset(m->bss_begin, 0, bss_size(m));
    int n = 0;
    for (CtorFn* p = m->ct_begin + 1; p < m->ct_end; p++) {
        if (*p) { (*p)(); n++; }
    }
    if (n) re4_log("OSLink: %s ran %d static initialisers\n", m->name, n);
}

// BOOL OSLink(OSModuleInfo* newModule, void* bss)
// As on the GameCube, OSLink "relocates" the header's prolog/epilog/unresolved into addresses; the game
// then calls DLL_PROLOG(header)() itself (read.cpp setEmModule, ReadPlayerData, ReadWepData, roomdata.cpp).
int OSLink(void* info, void* bss)
{
    (void) bss;   // module statics live in the image's own per-module sections
    unsigned id = module_id(info);
    Re4Module* m = find_module(id);
    if (!m) {
        re4_log("OSLink: unknown REL module id %u\n", id);
        return 0;
    }
    int restart = m->saved && m->unlinked_header == info && !re4_dvd_overwritten_since(info, m->unlink_gen);
    if (restart) {
        memcpy(m->data_begin, m->saved, data_size(m));
        memcpy(m->bss_begin, m->saved + data_size(m), bss_size(m));
    } else {
        fresh_state(m);
    }
    re4_log("OSLink: %s (id %u) %s\n", m->name, id, restart ? "restart" : "fresh");
    ModuleHeader* h = (ModuleHeader*) info;
    h->prolog = (unsigned) m->prolog;
    h->epilog = (unsigned) m->epilog;
    h->unresolved = (unsigned) re4_module_unresolved;
    m->linked = 1;
    return 1;
}

// BOOL OSUnlink(OSModuleInfo* oldModule). The game calls the epilog itself before (main_sub.cpp DLL_Unlink).
int OSUnlink(void* info)
{
    Re4Module* m = find_module(module_id(info));
    if (!m) return 0;
    unsigned n = data_size(m) + bss_size(m);
    if (n) {
        if (!m->saved) m->saved = (char*) malloc(n);
        if (m->saved) {
            memcpy(m->saved, m->data_begin, data_size(m));
            memcpy(m->saved + data_size(m), m->bss_begin, bss_size(m));
        }
    }
    m->unlinked_header = info;
    m->unlink_gen = re4_dvd_generation;
    m->linked = 0;
    return 1;
}

} // extern "C"
