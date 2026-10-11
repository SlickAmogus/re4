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
         '    r = __builtin_sinf(x);   /* prep.py: paired-single Taylor series; CT-02 ports the exact polynomial */\n'),
        # COSF carries the same block after `x = LIMIT_ANGLE(x + pi/2)`, so the rule above covers it too
        (r'    asm\("fcmpu 0, %0, %2.*?: "cr0"\);\n',
         '    if (x >= max) { do { x -= step; } while (x >= max); } else if (x < min) { do { x += step; } while (x < min); }\n'),
    ],
    'include/math_sub.h': [
        (r'asm volatile\("fabs %0,%1" : "=f"\((\w+)\) : "f"\((\w+)\)\);', r'\1 = __builtin_fabsf(\2);'),
    ],
}

# literal (not regex) per-file patches for const-correctness and similar one-liners
SITE_PATCHES = {
    'src/game/xml.cpp': [('= strstr(', '= (char*) strstr(')],
    'src/game/main_mem.cpp': [("name = strrchr(file, '/');", "name = (char*) strrchr(file, '/');")],
    'src/st3/r332.cpp': [('p = strstr(', 'p = (char*) strstr(')],
    'src/game/esp0a.cpp': [('AddOtWorldPos(e, Esp0a_Trans2,', 'AddOtWorldPos(e, (void (*)(void*)) Esp0a_Trans2,')],
}

PSQ_MACROS = [
    # trans.cpp / shape.cpp / espgen: paired-single quantised loads become plain conversions
    (re.compile(r'#define PSQ_L_U8\(p\) .*'), '#define PSQ_L_U8(p) ((f32) *(const u8*) (p))'),
    (re.compile(r'#define PSQ_L_U8_TO\(dst, p\) .*'), '#define PSQ_L_U8_TO(dst, p) ((dst) = (f32) *(const u8*) (p))'),
    (re.compile(r'#define PSQ_L_S16\(p\) .*'), '#define PSQ_L_S16(p) ((f32) *(const s16*) (p))'),
    (re.compile(r'#define PSQ_ST_S16\(f, p\) .*'), '#define PSQ_ST_S16(f, p) (*(s16*) (p) = (s16) (f))'),
]

SDK_C_UNITS = ('src/lib/mtx.c', 'src/lib/mtxvec.c', 'src/lib/mtx44.c', 'src/lib/vec.c', 'src/lib/quat.c')

# The `;` is optional: inside a macro body (pendulum.cpp PEN_ANCHOR) the lever has none, and the
# operand list never contains `;` or `#` (so the match cannot run into the next line of a macro).
EMPTY_ASM = re.compile(r'\basm\s*(?:volatile\s*)?\(\s*""\s*(?P<ops>:[^;#]*)?\)[ \t]*;?')
# top-level section/alignment levers (`asm(".section .sdata; .balign 8");`): x86 has no .sdata
SECTION_ASM = re.compile(r'^asm\("\s*\.section [^"]*"\);[ \t]*\n', re.M)

# `asm void PSVECAdd(...) {` (MWCC) and `void PSVECScale(register ...) {` (C body with asm blocks)
PS_FUNC = re.compile(r'^(?:asm\s+)?(?:void|f32|u32|int)\s+PS\w+\s*\([^)]*\)\s*\{', re.M)


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


def strip_ps_functions(text):
    out = []
    pos = 0
    for m in PS_FUNC.finditer(text):
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
    text = EMPTY_ASM.sub(rewrite_empty_asm, text)            # 2. codeless levers
    text = SECTION_ASM.sub('', text)
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
    for suffix, patches in SITE_PATCHES.items():              # 5. one-liners
        if upath.endswith(suffix):
            for old, new in patches:
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
