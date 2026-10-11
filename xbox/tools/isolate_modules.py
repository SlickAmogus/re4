#!/usr/bin/env python3
"""isolate_modules.py — namespaces one REL module's objects so it can be linked statically.

    python3 xbox/tools/isolate_modules.py <module name> <out .lib or dir> <obj>...

On the GameCube every REL was its own link unit: two modules (or a module and the DOL) may define the
same symbol (shared sources such as src/st/em_wrap.cpp v1/v2/v3, src/wep/pl_handgun.cpp, header
inlines, _prolog/_epilog/_unresolved). Here every *defined external* symbol of the module's objects is
renamed to `re4m_<module>_<symbol>` in all of the module's objects (llvm-objcopy --redefine-syms), so
references inside the module follow the rename and references to the DOL (undefined in the module)
stay untouched. The REL entry points become plain C names the module table can call:
_prolog -> re4mod_<module>_prolog (likewise _epilog, _unresolved).

Writes the renamed objects to <out dir>/<module>/*.obj, prints them (for the link line).
Needs llvm-nm and llvm-objcopy (LLVM_BIN or PATH; on Windows C:/Program Files/LLVM/bin).
"""
import os
import shutil
import subprocess
import sys


def tool(name):
    for d in (os.environ.get('LLVM_BIN'), r'C:\Program Files\LLVM\bin', '/usr/bin', '/usr/local/bin'):
        if d:
            for ext in ('.exe', ''):
                p = os.path.join(d, name + ext)
                if os.path.exists(p):
                    return p
    p = shutil.which(name)
    if not p:
        sys.exit(f'isolate_modules.py: {name} not found (set LLVM_BIN)')
    return p


# cdecl C names carry a leading underscore on i386-win32 (nxdk); on the -m32 ELF host they do not.
ENTRY = {'__prolog': 'prolog', '__epilog': 'epilog', '__unresolved': 'unresolved',
         '_prolog': 'prolog', '_epilog': 'epilog', '_unresolved': 'unresolved'}


def defined_symbols(nm, obj):
    r = subprocess.run([nm, '--defined-only', '--extern-only', '--format=just-symbols', obj],
                       capture_output=True, text=True, check=True)
    return set(s for s in r.stdout.split() if s)


def comdat_symbols(nm, obj):
    """Weak/COMDAT definitions (header inlines, vtables, template instances, string literals): duplicates
    are legal. ELF: nm marks them W/V/u. COFF: llvm-nm does not, so read the section characteristics
    (IMAGE_SCN_LNK_COMDAT) with llvm-readobj and collect the external symbols placed in those sections."""
    r = subprocess.run([nm, '--defined-only', '--extern-only', obj], capture_output=True, text=True, check=True)
    out = set()
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ('W', 'V', 'w', 'v', 'u', 'C'):   # C = common (tentative) data
            out.add(parts[2])
    readobj = tool('llvm-readobj')
    r = subprocess.run([readobj, '--sections', '--symbols', obj], capture_output=True, text=True)
    if r.returncode:
        return out
    comdat_secs = set()
    sec_no = None
    sym_name = None
    sym_sec = None
    in_sections = in_symbols = False
    for line in r.stdout.splitlines():
        t = line.strip()
        if t.startswith('Sections ['):
            in_sections, in_symbols = True, False
        elif t.startswith('Symbols ['):
            in_sections, in_symbols = False, True
        if in_sections:
            if t.startswith('Number:'):
                sec_no = int(t.split()[1])
            elif 'IMAGE_SCN_LNK_COMDAT' in t and sec_no is not None:
                comdat_secs.add(sec_no)
        elif in_symbols:
            if t.startswith('Name:'):
                sym_name = t[5:].strip()
                sym_sec = None
            elif t.startswith('Section:'):
                p = t[t.rfind('(') + 1:t.rfind(')')]
                sym_sec = int(p) if p.isdigit() else None
            elif t.startswith('StorageClass: External') and sym_sec in comdat_secs:
                out.add(sym_name)
    return out


def main():
    mod, out_dir, objs = sys.argv[1], sys.argv[2], sys.argv[3:]
    nm, objcopy = tool('llvm-nm'), tool('llvm-objcopy')
    per_obj = {o: defined_symbols(nm, o) for o in objs}
    weak = set()
    for o in objs:
        weak |= comdat_symbols(nm, o)
    syms = set().union(*per_obj.values()) if per_obj else set()
    coff = any(s == '__prolog' or s.startswith('?') for s in syms)
    lead = '_' if coff else ''
    # A strong symbol defined by several objects of one module (st4_0: r405 and r406 both define
    # setTexRender) existed on the GameCube too: each object resolved it locally. Keep that: such a
    # symbol is renamed per object inside its definers; other objects of the module get the first one.
    owners = {}
    for o in objs:
        for s in per_obj[o]:
            owners.setdefault(s, []).append(o)
    multi = {s for s, os_ in owners.items() if len(os_) > 1 and s not in weak and s not in ENTRY}

    def iso(s, prefix):
        # cdecl C names (`_foo` on COFF) keep their leading underscore so the renamed symbol is still a C
        # name (`_re4m_<mod>_foo` = C identifier re4m_<mod>_foo); C++ / other names get a plain prefix.
        if coff and s.startswith('_') and not s.startswith('__'):
            return f'_{prefix}{s[1:]}'
        return f'{prefix}{s}'

    def base_map():
        m = {}
        for s in syms:
            if s in ENTRY:
                m[s] = f'{lead}re4mod_{mod}_{ENTRY[s]}'
            else:
                m[s] = iso(s, f're4m_{mod}_')
        return m

    dst = os.path.join(out_dir, mod)
    os.makedirs(dst, exist_ok=True)
    outs = []
    for i, o in enumerate(objs):
        m = base_map()
        stem = os.path.splitext(os.path.basename(o))[0].split('.')[0]
        for s in multi:
            if o in owners[s] and owners[s][0] != o:
                m[s] = iso(s, f're4m_{mod}_{stem}_')
        map_path = os.path.join(dst, f'rename{i}.map')
        with open(map_path, 'w', newline='\n') as f:
            for a in sorted(m):
                f.write(f'{a} {m[a]}\n')
        t = os.path.join(dst, os.path.basename(o))
        subprocess.run([objcopy, f'--redefine-syms={map_path}', o, t], check=True)
        outs.append(t)
    if multi:
        sys.stderr.write(f'isolate_modules.py: {mod}: per-object duplicates kept: {", ".join(sorted(multi))}\n')
    print(' '.join(p.replace('\\', '/') for p in outs))


if __name__ == '__main__':
    main()
