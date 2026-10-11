#!/usr/bin/env python3
"""prep.py — prepares one game source for the x86 compilers (xbox/docs/07-COMPILER.md).

    python3 xbox/tools/prep.py <src> <out> [--module NAME]

The game tree is never edited. Rules, in order:
  1. `register T x asm("rN")` / `asm("fN")` pins become plain declarations.
  2. Empty-template asm levers `asm("" : outputs : inputs : clobbers);` (the decomp's codeless
     COMPILER-DIFF mechanisms) are rewritten: a tied output (`"=r"(a) : "0"(b)`) becomes `a = b;`,
     everything else becomes `((void) 0);`. `asm volatile("" ::: "memory")` is valid on x86 and stays.
  3. Known asm bodies are replaced by #include of the port's C versions (REPLACEMENTS).
  4. The SDK's C sources (src/lib mtx*.c, vec.c, quat.c): MWCC `asm <type> <name>(...) { ... }`
     function bodies are removed (the game links the C_* versions under the PS* names, see
     re4_xbox.h), and "fake_tgmath.h" becomes <math.h>.
  5. A few const-correctness sites GCC 2.95 accepted (SITE_PATCHES).
  6. `#line` directives are kept so diagnostics point at the real file.
`--module NAME` is informational; the Makefile passes -D_prolog=NAME_prolog etc. on the command line.
Based on tools/motion/host/prepare.py from the decomp.
"""
import re
import sys

# unit path suffix -> [(regex, replacement)], applied with re.S
REPLACEMENTS = {
    'src/game/trans.cpp': [
        (r'void CalcSk1_x\(void\* dst, void\* src, u32 n\)\n\{\n    asm volatile\(.*?\);\n\}\n', '#include "re4port/skin_calcsk1.inc"\n'),
        (r'void CalcSk1_x2\(void\* dst, void\* src, u32 n\)\n\{\n    asm volatile\(.*?\);\n\}\n', '#include "re4port/skin_calcsk1_x2.inc"\n'),
        (r'void setupGQR6\(u32 v\)\n\{\n    asm volatile\("mtspr 918, %0" : : "r"\(v\)\);\n\}\n', 'void setupGQR6(u32 v)\n{\n    extern u32 re4_gqr6; re4_gqr6 = v;\n}\n'),
    ],
}

# literal (not regex) per-file patches for const-correctness and similar one-liners
SITE_PATCHES = {
    'src/game/xml.cpp': [('= strstr(', '= (char*) strstr(')],
    'src/game/main_mem.cpp': [("name = strrchr(file, '/');", "name = (char*) strrchr(file, '/');")],
    'src/st3/r332.cpp': [('p = strstr(', 'p = (char*) strstr(')],
}

PSQ_MACROS = [
    # trans.cpp / shape.cpp / espgen: paired-single quantised loads become plain conversions
    (re.compile(r'#define PSQ_L_U8\(p\) .*'), '#define PSQ_L_U8(p) ((f32) *(const u8*) (p))'),
    (re.compile(r'#define PSQ_L_U8_TO\(dst, p\) .*'), '#define PSQ_L_U8_TO(dst, p) ((dst) = (f32) *(const u8*) (p))'),
    (re.compile(r'#define PSQ_L_S16\(p\) .*'), '#define PSQ_L_S16(p) ((f32) *(const s16*) (p))'),
    (re.compile(r'#define PSQ_ST_S16\(f, p\) .*'), '#define PSQ_ST_S16(f, p) (*(s16*) (p) = (s16) (f))'),
]

SDK_C_UNITS = ('src/lib/mtx.c', 'src/lib/mtxvec.c', 'src/lib/mtx44.c', 'src/lib/vec.c', 'src/lib/quat.c')

EMPTY_ASM = re.compile(r'\basm\s*(?:volatile\s*)?\(\s*""\s*(?P<ops>:[^;]*)?\)\s*;')


def split_operands(s):
    """'"=r"(a), "=m"(b)' -> [('=r', 'a'), ('=m', 'b')] with balanced parentheses."""
    out = []
    i = 0
    n = len(s)
    while i < n:
        m = re.compile(r'\s*"([^"]*)"\s*\(').match(s, i)
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


MWCC_ASM_FUNC = re.compile(r'^asm\s+[\w*\s]+?\b\w+\s*\([^)]*\)\s*\{', re.M)


def strip_mwcc_asm_functions(text):
    out = []
    pos = 0
    for m in MWCC_ASM_FUNC.finditer(text):
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
        out.append('/* MWCC asm function removed by prep.py (C_ version linked instead) */\n')
        pos = j
    out.append(text[pos:])
    return ''.join(out)


def prep(text, path):
    upath = path.replace('\\', '/')
    text = re.sub(r'\s*asm\("f?r\d+"\)', '', text)            # 1. register pins
    text = EMPTY_ASM.sub(rewrite_empty_asm, text)            # 2. codeless levers
    for pat, rep in PSQ_MACROS:
        text = pat.sub(rep, text)
    for suffix, rules in REPLACEMENTS.items():                # 3. asm bodies
        if upath.endswith(suffix):
            for pat, rep in rules:
                text, n = re.subn(pat, rep, text, flags=re.S)
                if n == 0:
                    sys.stderr.write(f'prep.py: {path}: pattern not found: {pat[:50]}...\n')
    if upath.endswith(SDK_C_UNITS):                           # 4. SDK C units
        text = strip_mwcc_asm_functions(text)
        text = text.replace('#include "fake_tgmath.h"', '#include <math.h>')
    for suffix, patches in SITE_PATCHES.items():              # 5. one-liners
        if upath.endswith(suffix):
            for old, new in patches:
                if old not in text:
                    sys.stderr.write(f'prep.py: {path}: site patch not found: {old}\n')
                text = text.replace(old, new)
    return text


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    src, out = args[0], args[1]
    with open(src, encoding='utf-8', errors='surrogateescape') as f:
        text = f.read()
    with open(out, 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
        f.write(f'#line 1 "{src}"\n')
        f.write(prep(text, src))


if __name__ == '__main__':
    main()
