#!/usr/bin/env python3
"""link_resolve.py — closes the static link of the game (xbox/docs/07-COMPILER.md "Linking the modules").

    python3 xbox/tools/link_resolve.py <undefined.txt> <out.rsp> <out_stubs.c> <obj/lib/dir>...

<undefined.txt> holds the raw (non-demangled) symbols lld reported undefined (`/demangle:no`).
Every other argument is an object, a library or a directory searched recursively for .obj/.lib; their
defined symbols are read with llvm-nm. For each undefined symbol U, in order:

  1. U is defined by a module under its isolated name `re4m_<mod>_U` (a REL-to-REL import, or a stage
     REL's st1.cpp/st2.cpp table naming a room that lives in a sibling REL: on the GameCube the loader
     pointed those at _unresolved; here they bind to the real definition, which is only called while
     its own module is the loaded one) -> `/alternatename:U=re4m_<mod>_U` (first module in sorted order).
  2. U is a variable whose MSVC mangling encodes a type that differs from the definition's (GCC 2.95 did
     not mangle data, so `extern GameWork Game;` against `GAME_WORK Game;` was fine) -> alias to the one
     defined `?Name@@3...` (or a module's isolated copy of it) with the same name.
  3. U is a C name `_Name` defined only with C++ linkage, or the reverse -> alias.
  4. Otherwise U is a missing implementation (SDK surface not written yet, CRI movie lib, debug host
     services): functions -> `_re4_unimpl_fn` (logs the first call), data -> `_re4_unimpl_data` (a
     zeroed 64 KB block). The list is written to <out_stubs.c> as a comment table plus the two
     definitions, and summarised on stdout so it doubles as the port's to-do list.

The .rsp is passed to lld-link as @file; it is regenerated on every build.
"""
import os
import re
import shutil
import subprocess
import sys


def tool(name):
    for d in (os.environ.get('LLVM_BIN'), r'C:\Program Files\LLVM\bin'):
        if d:
            for ext in ('.exe', ''):
                p = os.path.join(d, name + ext)
                if os.path.exists(p):
                    return p
    p = shutil.which(name)
    if not p:
        sys.exit(f'link_resolve.py: {name} not found (set LLVM_BIN)')
    return p


def collect_inputs(args):
    files = []
    for a in args:
        if os.path.isdir(a):
            for dp, _, fn in os.walk(a):
                for f in fn:
                    if f.endswith(('.obj', '.lib')):
                        files.append(os.path.join(dp, f))
        elif os.path.exists(a):
            files.append(a)
    return files


def defined_symbols(nm, files):
    out = set()
    # batch to keep command lines short
    for i in range(0, len(files), 64):
        r = subprocess.run([nm, '--defined-only', '--extern-only', '--format=just-symbols', *files[i:i + 64]],
                           capture_output=True, text=True)
        for line in r.stdout.splitlines():
            s = line.strip()
            if s and not s.endswith(':'):
                out.add(s)
    return out


ISOLATED = re.compile(r'^re4m_(\w+?)_(.+)$')
DATA = re.compile(r'^\?([A-Za-z_][A-Za-z0-9_]*)@@3')        # ?Name@@3<type>  (global variable)
CXX_NAME = re.compile(r'^\?([A-Za-z_][A-Za-z0-9_]*)@@')      # ?Name@@...      (global function or data)


QUALIFIED = re.compile(r'^(\?[A-Za-z_][A-Za-z0-9_]*@(?:[A-Za-z_][A-Za-z0-9_]*@)*@)')   # ?Name@Scope@@

# GCC 2.95 names bound through `asm("...")` labels (the label is dropped by prep.py) -> MSVC names.
# cdecl free functions only: a mismatched argument list is harmless for cdecl (the caller pops).
EXPLICIT = {
    '___builtin_new': '??2@YAPAXI@Z',            # operator new(unsigned)
    '___builtin_vec_new': '??_U@YAPAXI@Z',       # operator new[](unsigned)
    '___builtin_delete': '??3@YAXPAX@Z',         # operator delete(void*)
    '___builtin_vec_delete': '??_V@YAXPAX@Z',    # operator delete[](void*)
    '_memset_v': '_memset',
    '_r300_memset': '_memset',
    '_ScreenReSizeI': '?ScreenReSize@@YAXGG@Z',
    '?ScreenReSizeI@@YAXHK@Z': '?ScreenReSize@@YAXGG@Z',
    '?Filter09RenderI@@YAXH@Z': '?Filter09Render@@YAXXZ',
    '?SndCallV@@YAXGGPAUVec@@HHPAVcUnit@@@Z': '?SndCall@@YAKGGPAUVec@@HHPAVcUnit@@@Z',
    # __thiscall targets get real thunks in xbox/port/os/alias_thunks.cpp (EvtFlgOnStatus,
    # cObjScrSetCallBack), so they never reach this table
}


def is_data(sym):
    return bool(DATA.match(sym)) or (not sym.startswith('?') and not sym.startswith('_'))


def main():
    und_path, rsp_path, stubs_path, inputs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    undefined = [l.strip() for l in open(und_path) if l.strip()]
    nm = tool('llvm-nm')
    files = collect_inputs(inputs)
    defined = defined_symbols(nm, files)
    # module names contain underscores (st1_0): take them from the module libraries' file names
    global ISOLATED
    mods = sorted({os.path.splitext(os.path.basename(f))[0] for f in files
                   if f.replace('\\', '/').split('/')[-2:-1] == ['mod'] and f.endswith('.lib')}, key=len, reverse=True)
    if mods:
        # `re4m_<mod>_<sym>` (C++ / other names) or `_re4m_<mod>_<name>` (C names keep their underscore)
        ISOLATED = re.compile(r'^(_?)re4m_(' + '|'.join(map(re.escape, mods)) + r')_(.+)$')

    by_isolated = {}      # plain symbol -> [isolated names]
    data_by_name = {}     # Name -> [defined ?Name@@3... (plain or isolated)]
    cxx_by_name = {}      # Name -> [defined ?Name@@... ]
    fn_by_qname = {}      # ?Name@Scope@@ -> [defined functions]
    for s in defined:
        m = ISOLATED.match(s) if mods else None
        plain = (m.group(1) + m.group(3)) if m else s
        if m:
            by_isolated.setdefault(plain, []).append(s)
        dm = DATA.match(plain)
        if dm:
            data_by_name.setdefault(dm.group(1), []).append(s)
        cm = CXX_NAME.match(plain)
        if cm:
            cxx_by_name.setdefault(cm.group(1), []).append(s)
        qm = QUALIFIED.match(plain)
        if qm and not dm:
            fn_by_qname.setdefault(qm.group(1), []).append(s)

    aliases, missing_fn, missing_data = [], [], []
    for u in undefined:
        if u in defined:
            continue
        if u in by_isolated:
            aliases.append((u, sorted(by_isolated[u])[0], 'module'))
            continue
        dm = DATA.match(u)
        if dm and dm.group(1) in data_by_name:
            aliases.append((u, sorted(data_by_name[dm.group(1)])[0], 'data type'))
            continue
        if u.startswith('_') and not u.startswith('__') and u[1:] in cxx_by_name:
            aliases.append((u, sorted(cxx_by_name[u[1:]])[0], 'C->C++'))
            continue
        cm = CXX_NAME.match(u)
        if cm and ('_' + cm.group(1)) in defined:
            aliases.append((u, '_' + cm.group(1), 'C++->C'))
            continue
        if not u.startswith(('?', '_')) and ('_' + u) in defined:      # bare data name vs cdecl
            aliases.append((u, '_' + u, 'bare'))
            continue
        if u in EXPLICIT and (EXPLICIT[u] in defined or EXPLICIT[u] in by_isolated):
            t = EXPLICIT[u]
            aliases.append((u, t if t in defined else sorted(by_isolated[t])[0], 'explicit'))
            continue
        # bare name from an asm-labelled declaration (`extern CAMERA em21_trap_cam_v asm("em21_trap_cam")`)
        if not u.startswith(('?', '_')) and u in data_by_name and len(set(data_by_name[u])) >= 1:
            aliases.append((u, sorted(data_by_name[u])[0], 'asm label data'))
            continue
        # same qualified function name, different return type / qualifiers in the declaration (MSVC mangles
        # the return type, GCC 2.95 did not): alias when the name is unique among the definitions
        qm = QUALIFIED.match(u)
        if qm and not DATA.match(u):
            cands = sorted({s for s in fn_by_qname.get(qm.group(1), ())})
            if len(cands) == 1:
                aliases.append((u, cands[0], 'signature'))
                continue
        (missing_data if is_data(u) else missing_fn).append(u)

    with open(rsp_path, 'w', newline='\n') as f:
        for u, t, why in aliases:
            f.write(f'"/alternatename:{u}={t}"\n')
        for u in missing_fn:
            f.write(f'"/alternatename:{u}=_re4_unimpl_fn"\n')
        for u in missing_data:
            f.write(f'"/alternatename:{u}=_re4_unimpl_data"\n')

    with open(stubs_path, 'w', newline='\n') as f:
        f.write('/* Generated by xbox/tools/link_resolve.py: the symbols the game references that nothing in the\n'
                ' * image defines yet. Each is bound with /alternatename to one of the two catch-alls below.\n'
                ' * Replace entries by real implementations in xbox/port/ (the next build drops them from here).\n *\n')
        for u in missing_fn:
            f.write(f' *   fn   {u}\n')
        for u in missing_data:
            f.write(f' *   data {u}\n')
        f.write(' */\n#include "re4port/re4_xbox.h"\n\n'
                'unsigned char re4_unimpl_data[0x10000] __attribute__((aligned(32)));\n\n'
                '/* cdecl, no arguments read: safe to call for any cdecl prototype. A __thiscall/__stdcall\n'
                ' * callee would leave the stack unbalanced; the log line says which symbol was hit. */\n'
                'int re4_unimpl_fn(void)\n{\n'
                '    /* one log line per distinct call site (map the address with the linker map / llvm-symbolizer) */\n'
                '    static void* seen[256];\n'
                '    static unsigned n;\n'
                '    void* ra = __builtin_return_address(0);\n'
                '    for (unsigned i = 0; i < n; i++) if (seen[i] == ra) return 0;\n'
                '    if (n < 256) seen[n++] = ra;\n'
                '    re4_log("UNIMPLEMENTED function called from %p (see build/link_stubs.c)\\n", ra);\n'
                '    return 0;\n}\n')

    kinds = {}
    for _, _, why in aliases:
        kinds[why] = kinds.get(why, 0) + 1
    print(f'link_resolve: {len(undefined)} undefined -> {len(aliases)} aliased {kinds}, '
          f'{len(missing_fn)} missing functions, {len(missing_data)} missing data')


if __name__ == '__main__':
    main()
