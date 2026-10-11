#!/usr/bin/env python3
"""prep.py — prepares one game source (or every header of a directory) for the x86 compilers
(xbox/docs/07-COMPILER.md).

    python3 xbox/tools/prep.py <src> <out> [--module NAME]
    python3 xbox/tools/prep.py --dir <include dir> <out dir>

The game tree is never edited. Rules, in order:
  1. `register T x asm("rN")` / `asm("fN")` pins become plain declarations.
  2. Empty-template asm levers `asm("" : outputs : inputs : clobbers);` (the decomp's codeless
     COMPILER-DIFF mechanisms) are rewritten: a tied output (`"=r"(a) : "0"(b)`) becomes `a = b;`,
     everything else becomes `((void) 0);`. `asm volatile("" ::: "memory")` is valid on x86 and stays.
  3. Known asm bodies are replaced (REPLACEMENTS): trans.cpp skinning -> #include of the port's C
     versions, math_sub.cpp SQRTF/SINF/COSF/LIMIT_ANGLE -> C (exact polynomials come with CT-02),
     math_sub.h fabs.
  4. The SDK's C sources (src/lib mtx*.c, vec.c, quat.c): every PS* function (MWCC `asm void PS...`
     bodies and C bodies with paired-single asm blocks) is removed; the game links the C_* versions
     under the PS* names (re4_xbox.h), and "fake_tgmath.h" becomes <math.h>.
  5. A few const-correctness / function-pointer sites GCC 2.95 accepted (SITE_PATCHES).
  6. `#line` directives are kept so diagnostics point at the real file.
`--module NAME` is informational; the Makefile passes -D_prolog=NAME_prolog etc. on the command line.
Based on tools/motion/host/prepare.py from the decomp.
"""
import os
import re
import sys

# unit path suffix -> [(regex, replacement)], applied with re.S
REPLACEMENTS = {
    'src/game/trans.cpp': [
        (r'void CalcSk1_x\(void\* dst, void\* src, u32 n\)\n\{\n    asm volatile\(.*?\);\n\}\n',
         '#include "re4port/skin_calcsk1.inc"\n'),
        (r'void CalcSk1_x2\(void\* dst, void\* src, u32 n\)\n\{\n    asm volatile\(.*?\);\n\}\n',
         '#include "re4port/skin_calcsk1_x2.inc"\n'),
        (r'void setupGQR6\(u32 v\)\n\{\n    asm volatile\("mtspr 918, %0" : : "r"\(v\)\);\n\}\n',
         'void setupGQR6(u32 v)\n{\n    extern unsigned int re4_gqr6; re4_gqr6 = (unsigned int) v;\n}\n'),
    ],
    # GQR2..GQR7 setup (`li 3, N; oris 3, 3, N; mtspr 91x, 3` = 0x000N000N: load/store type N, scale 0)
    # in scheduler.cpp TaskExec_hook and main.cpp; the port's skinning reads re4_gqr6 instead.
    'src/game/scheduler.cpp': [(r'    asm\("li 3, 4\\n".*?\);\n', '    ((void) 0);   /* prep.py: GQR2..7 = 0x00040004.. (paired-single quantisation; no-op on x86) */\n')],
    'src/game/main.cpp': [(r'    asm\("li 3, 4\\n".*?\);\n', '    ((void) 0);   /* prep.py: GQR setup, see scheduler.cpp */\n')],
    # the game's placement new collides with libc++'s <new> (pulled in through include/xml.h)
    'include/cManager.h': [(r'inline void\* operator new\(unsigned int, void\* p\) \{ return p; \}', '#include <new>')],
    'include/cam_extra.h': [(r'inline void\* operator new\(unsigned int, void\* p\) \{ return p; \}', '#include <new>')],
    'src/game/math_sub.cpp': [
        (r'    asm\("frsqrte 2, %1.*?"fr2", "fr3", "fr4"\);\n',
         '    r = __builtin_sqrtf(x); (void) half; (void) three;   /* prep.py: SDK frsqrte + Newton */\n'),
        (r'    asm volatile\(\n        "lis 9, Coeff@ha.*?"ps_sum0 %0, 4, 4, 4"\n        : "=f"\(r\)\n        : "f"\(x\)\n        : "r9", "r10", "r11", "fr2", "fr3", "fr4", "fr5"\);\n',
         '    r = re4_ppc_sin_series(x);   /* prep.py: the game\'s paired-single series, port/mtx/math_ppc.cpp (CT-02) */\n'),
        # COSF carries the same block after `x = LIMIT_ANGLE(x + pi/2)`, so the rule above covers it too
        (r'    asm\("fcmpu 0, %0, %2.*?: "cr0"\);\n',
         '    if (x >= max) { do { x -= step; } while (x >= max); } else if (x < min) { do { x += step; } while (x < min); }\n'),
    ],
    # The GC header renames libm's fabsf out of the way and defines a static inline asm one; under
    # clang's MSVC mode the static inline merges with pdclib's extern declaration and becomes an
    # exported duplicate. Use the platform fabsf instead.
    # pointers are no longer in 0x80000000..0x82FFFFFF (heaps live in the port arena, statics in the XBE)
    'include/main_mem.h': [
        (r'#define VALID_PTR\(p\) \(\(u32\) \(p\) >= 0x80000000 && \(u32\) \(p\) <= 0x82FFFFFF\)',
         '#define VALID_PTR(p) RE4_VALID_PTR(p)   /* prep.py */'),
    ],
    # MWCC section-placed asm declarations in the SDK's private header (ppc_eabi_init)
    'src/lib/__os.h': [(r'__declspec\(section "\.init"\) asm [^;]*;\n', '')],
    'include/math_sub.h': [
        (r'#define fabsf fabsf_libm\n#include <math.h>\n#undef fabsf\n.*?static inline f32 fabsf\(f32 x\)\n\{\n.*?\n\}\n',
         '#include <math.h>   /* prep.py: the GC asm inline fabsf -> compiler builtin */\n'
         '#ifndef fabsf\n#define fabsf(x) __builtin_fabsf(x)\n#endif\n'),
    ],
}

# literal (not regex) per-file patches for const-correctness and similar one-liners
SITE_PATCHES = {
    'src/game/xml.cpp': [('= strstr(', '= (char*) strstr(')],
    'src/game/main_mem.cpp': [("name = strrchr(file, '/');", "name = (char*) strrchr(file, '/');")],
    'src/st3/r332.cpp': [('p = strstr(', 'p = (char*) strstr(')],
    'src/game/main_sub.cpp': [('if (addr < GC_U32(0x80000000) || addr > 0x82FFFFFF) {', 'if (!RE4_VALID_PTR(addr)) {')],
    # PowerPC .rodata labels defined in top-level asm -> C++ definitions (the asm-labelled `_v` aliases
    # resolve to them through link_resolve.py's "asm label data" rule)
    'src/st2/r222.cpp': [('asm(".section \\".rodata\\"\\n\\t.align 2\\nr222_k212:\\n\\t.long 0x4007b8a5\\n\\t.section \\".text\\"");',
                          'extern const f32 r222_k212; const f32 r222_k212 = 2.12f;   /* prep.py: was a .rodata asm label (0x4007b8a5) */')],
    'src/st2/r224.cpp': [('asm(".section \\".rodata\\"\\n\\t.align 2\\nr224_zero:\\n\\t.long 0\\n\\t.section \\".text\\"");',
                          'extern const f32 r224_zero; const f32 r224_zero = 0.0f;   /* prep.py: was a .rodata asm label */')],
    # The SN linker resolved references to `static` definitions and to non-emitted `inline` members from
    # other units (and from REL modules). A standard linker does not: give them external linkage.
    'src/lib/OSTime.c': [('void __OSSetTime(OSTime time) {',
                          'static void __SetTime(OSTime t) { (void) t; }   /* prep.py: was mtspr TBL/TBU */\nvoid __OSSetTime(OSTime time) {')],
    'src/game/foot_shadow_tbl.cpp': [('static FootShadowTbl Em32_fs_tbl', 'FootShadowTbl Em32_fs_tbl')],
    'src/game/em_sub.cpp': [('static void EmCatchSubSet(', 'void EmCatchSubSet(')],
    'src/game/quake.cpp': [('static void QuakeKill(u8 id)', 'void QuakeKill(u8 id)')],
    'src/game/model.cpp': [('inline int cModel::isTrans()', 'int cModel::isTrans()')],
    'src/game/pl_class.cpp': [('inline void cPlayer::subCharLiveCheck()', 'void cPlayer::subCharLiveCheck()')],
    'src/game/EtcModel.cpp': [('static int setRoomEtcBreakDisp(', 'int setRoomEtcBreakDisp(')],
    'src/em21/em21.cpp': [('static CAMERA em21_trap_cam = { 0 };', 'CAMERA em21_trap_cam = { 0 };')],
    'src/em2a/em2a.cpp': [('static CAMERA em2a_rescue_cam = { 0 };', 'CAMERA em2a_rescue_cam = { 0 };')],
    'src/game/esp0a.cpp': [('AddOtWorldPos(e, Esp0a_Trans2,', 'AddOtWorldPos(e, (void (*)(void*)) Esp0a_Trans2,')],
    # skinning palette lived in locked cache at 0xE0000000 (CT-02); the port keeps it in re4_skin_palette
    'src/game/trans.cpp': [('PSMTXReorder(m, (f32(*)[3]) (0xE0000000 + i * 0x30));', 'PSMTXReorder(m, RE4_SKIN_PALETTE(i));')],
}

# Literal GameCube MEM1 addresses the game uses as pointers (fixed memory map, low-memory globals, DVD and
# font buffers). Each listed literal becomes GC_U32(lit) = its address inside the port's arena
# (re4_xbox.h). Bit masks such as 0x80000000 in flag tests are untouched because only these files and
# these exact literals are rewritten. Found with the grep in 07-COMPILER.md "Fixed addresses".
GC_ADDRESS_SITES = {
    'src/game/main_mem.cpp': ['0x817F4000', '0x80350000', '0x80370000', '0x803F0000', '0x80460000', '0x80578000',
                              '0x807AC000', '0x807EC000', '0x80904000', '0x80974000', '0x81800000', '0x8181FB00',
                              '0x8034FFFF'],
    'src/game/main_sub.cpp': ['0x80000000', '0x80460000', '0x803F0000'],
    'src/game/dvd.cpp': ['0x80000000', '0x80350000', '0x80360000', '0x816D3100', '0x817D3EE0'],
    'src/game/debug.cpp': ['0x80000000'],
    'src/game/game.cpp': ['0x807EC000', '0x80974000'],
    'src/game/file.cpp': ['0x81800000'],
    'src/game/read.cpp': ['0x807EC000', '0x80904000'],
    'src/game/snd.cpp': ['0x80370000'],
}

# headers a unit needs for the port replacements above, inserted after the `#line 1` marker
PREPEND = {
    'src/game/trans.cpp': '#include "re4port/skin.h"\n#line 1\n',
    'src/game/math_sub.cpp': '#include "re4port/math_ppc.h"\n#line 1\n',
}

# `RET Name(args) asm("gcc295_mangled");` aliases (15 sites): the GCC 2.95 names do not exist under
# clang, so the label is dropped and xbox/port/os/alias_thunks.cpp defines Name as a forwarder.
ALIAS_LABEL = re.compile(r'\)\s*asm\("[A-Za-z_][A-Za-z0-9_]*"\)\s*;')

PSQ_MACROS = [
    # trans.cpp / shape.cpp / espgen: paired-single quantised loads become plain conversions
    (re.compile(r'#define PSQ_L_U8\(p\) .*'), '#define PSQ_L_U8(p) ((f32) *(const u8*) (p))'),
    (re.compile(r'#define PSQ_L_U8_TO\(dst, p\) .*'), '#define PSQ_L_U8_TO(dst, p) ((dst) = (f32) *(const u8*) (p))'),
    (re.compile(r'#define PSQ_L_S16\(p\) .*'), '#define PSQ_L_S16(p) ((f32) *(const s16*) (p))'),
    # psq_st saturates to the s16 range on Gekko
    (re.compile(r'#define PSQ_ST_S16\(f, p\) .*'),
     '#define PSQ_ST_S16(f, p) (*(s16*) (p) = (s16) ((f) >= 32767.0f ? 32767 : (f) <= -32768.0f ? -32768 : (int) (f)))'),
]

# SDK units compiled from src/lib with their MWCC `asm <type> name(...) { ... }` functions removed (the
# port provides those: OSGetTime/OSGetTick in port/os/os_misc.cpp)
SDK_ASM_FUNC_UNITS = ('src/lib/OSTime.c',)
MWCC_ASM_FUNC_ANY = re.compile(r'^(?:static' + chr(92) + r's+)?asm' + chr(92) + r's+(?:static' + chr(92) + r's+)?[' + chr(92) + r'w' + chr(92) + r's*]+?' + chr(92) + r'b' + chr(92) + r'w+' + chr(92) + r's*' + chr(92) + r'([^)]*' + chr(92) + r')' + chr(92) + r's*' + chr(92) + r'{', re.M)

SDK_C_UNITS = ('src/lib/mtx.c', 'src/lib/mtxvec.c', 'src/lib/mtx44.c', 'src/lib/mtx44vec.c', 'src/lib/vec.c', 'src/lib/quat.c')

# Start of an empty-template asm; the operand list is found by balancing parentheses (sub_empty_asm),
# and the trailing `;` is optional because macro bodies (pendulum.cpp PEN_ANCHOR, em32 EM32_W_FRESH)
# leave it to the use site.
EMPTY_ASM_START = re.compile(r'\basm\s*(?:volatile\s*)?\(\s*""\s*')


class _AsmMatch:
    def __init__(self, whole, ops):
        self._whole, self._ops = whole, ops

    def group(self, name=0):
        return self._ops if name == 'ops' else self._whole


def sub_empty_asm(text):
    out = []
    pos = 0
    for m in EMPTY_ASM_START.finditer(text):
        if m.start() < pos:
            continue
        depth = 1
        j = m.start() + text[m.start():m.end()].index('(') + 1
        in_str = False
        while j < len(text) and depth:
            ch = text[j]
            if ch == '"' and text[j - 1] != '\\':
                in_str = not in_str
            elif not in_str:
                if ch == '(':
                    depth += 1
                elif ch == ')':
                    depth -= 1
            j += 1
        inner = text[m.end():j - 1].strip()
        end = j
        k = j
        while k < len(text) and text[k] in ' \t':
            k += 1
        has_semi = k < len(text) and text[k] == ';'
        if has_semi:
            end = k + 1
        rep = rewrite_empty_asm(_AsmMatch(text[m.start():end], inner if inner.startswith(':') else None))
        if not has_semi and rep.endswith(';'):
            rep = rep[:-1]
        out.append(text[pos:m.start()])
        out.append(rep)
        pos = end
    out.append(text[pos:])
    return ''.join(out)
# top-level section/alignment levers (`asm(".section .sdata; .balign 8");`): x86 has no .sdata
SECTION_ASM = re.compile(r'^asm\("\s*\.section (?:[^"\\]|\\.)*"\);[ \t]*\n', re.M)

# `asm void PSVECAdd(...) {` (MWCC) and `void PSVECScale(register ...) {` (C body with asm blocks)
# also `static void __PSMTX44RotAxisRadInternal(...)` helpers
PS_FUNC = re.compile(r'^(?:static\s+)?(?:asm\s+)?(?:void|f32|u32|int)\s+_*PS(?:MTX|VEC|QUAT)\w*\s*\([^)]*\)\s*\{', re.M)


def split_operands(s):
    """'"=r"(a), "=m"(b)' -> [('=r', 'a'), ('=m', 'b')] with balanced parentheses."""
    out = []
    i = 0
    n = len(s)
    head = re.compile(r'\s*"([^"]*)"\s*\(')
    while i < n:
        m = head.match(s, i)
        if not m:
            break
        constraint = m.group(1)
        depth = 1
        j = m.end()
        while j < n and depth:
            if s[j] == '(':
                depth += 1
            elif s[j] == ')':
                depth -= 1
            j += 1
        out.append((constraint, s[m.end():j - 1].strip()))
        i = j
        while i < n and s[i] in ' ,\n\t':
            i += 1
    return out


def rewrite_empty_asm(m):
    ops = m.group('ops')
    if not ops:
        return '((void) 0);'
    parts = ops[1:].split(':')          # outputs, inputs, clobbers
    outputs = split_operands(parts[0]) if len(parts) > 0 else []
    inputs = split_operands(parts[1]) if len(parts) > 1 else []
    clobbers = parts[2] if len(parts) > 2 else ''
    if not outputs and not inputs and 'memory' in clobbers:
        return m.group(0)               # compiler barrier, valid everywhere
    stmts = []
    for constraint, expr in inputs:
        if constraint.isdigit():        # tied to output N: that output receives this input
            idx = int(constraint)
            if idx < len(outputs):
                stmts.append(f'{outputs[idx][1]} = {expr};')
    if not stmts:
        return '((void) 0);'
    return ' '.join(stmts)


def strip_ps_functions(text, pattern=None):
    out = []
    pos = 0
    for m in (pattern or PS_FUNC).finditer(text):
        if m.start() < pos:
            continue
        depth = 1
        j = m.end()
        while j < len(text) and depth:
            if text[j] == '{':
                depth += 1
            elif text[j] == '}':
                depth -= 1
            j += 1
        out.append(text[pos:m.start()])
        out.append('/* PS* function removed by prep.py (the C_ version is linked under the PS name) */\n')
        pos = j
    out.append(text[pos:])
    return ''.join(out)


def prep(text, path):
    upath = path.replace('\\', '/')
    text = re.sub(r'\s*asm\("f?r\d+"\)', '', text)            # 1. register pins
    text = sub_empty_asm(text)                               # 2. codeless levers
    for suffix, patches in SITE_PATCHES.items():              # asm-shaped site patches first, before
        if upath.endswith(suffix):                            # the section-directive strip removes them
            for old, new in patches:
                if old.startswith('asm('):
                    text = text.replace(old, new)
    text = SECTION_ASM.sub('', text)
    text = ALIAS_LABEL.sub(');', text)
    for pat, rep in PSQ_MACROS:
        text = pat.sub(rep, text)
    for suffix, rules in REPLACEMENTS.items():                # 3. asm bodies
        if upath.endswith(suffix):
            for pat, rep in rules:
                text, n = re.subn(pat, rep, text, flags=re.S)
                if n == 0:
                    sys.stderr.write(f'prep.py: {path}: pattern not found: {pat[:50]}...\n')
    if upath.endswith(SDK_C_UNITS):                           # 4. SDK C units
        text = strip_ps_functions(text)
        text = text.replace('#include "fake_tgmath.h"', '#include <math.h>')
    if upath.endswith(SDK_ASM_FUNC_UNITS):                    # SDK units reused minus their asm functions
        text = strip_ps_functions(text, MWCC_ASM_FUNC_ANY)
    for suffix, lits in GC_ADDRESS_SITES.items():             # fixed GameCube addresses -> arena
        if upath.endswith(suffix):
            for lit in lits:
                n_before = len(re.findall(r'\b' + lit + r'\b', text))
                if n_before == 0:
                    sys.stderr.write(f'prep.py: {path}: address literal not found: {lit}\n')
                text = re.sub(r'(?<![\w])' + lit + r'\b', f'GC_U32({lit})', text)
    for suffix, patches in SITE_PATCHES.items():              # 5. one-liners
        if upath.endswith(suffix):
            for old, new in patches:
                if old.startswith('asm('):
                    continue                                  # applied before the section strip
                if old not in text:
                    sys.stderr.write(f'prep.py: {path}: site patch not found: {old}\n')
                text = text.replace(old, new)
    return text


def prep_file(src, out):
    with open(src, encoding='utf-8', errors='surrogateescape') as f:
        text = f.read()
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out, 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
        f.write('#line 1 "%s"\n' % src.replace('\\', '/'))   # forward slashes: clang reads \o as an escape
        upath = src.replace('\\', '/')
        for suffix, head in PREPEND.items():
            if upath.endswith(suffix):
                f.write(head)
        f.write(prep(text, src))


def prep_dir(src_dir, out_dir):
    """--dir: prep every header under src_dir into out_dir (same relative paths)."""
    for dp, _, fn in os.walk(src_dir):
        for name in fn:
            if name.endswith(('.h', '.inc')):
                sp = os.path.join(dp, name)
                prep_file(sp, os.path.join(out_dir, os.path.relpath(sp, src_dir)))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if '--dir' in sys.argv:
        prep_dir(args[0], args[1])
        return
    prep_file(args[0], args[1])


if __name__ == '__main__':
    main()
