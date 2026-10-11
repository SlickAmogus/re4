#!/usr/bin/env python3
"""prep.py — prepares one game source for the x86 compilers (xbox/docs/07-COMPILER.md).

    python3 xbox/tools/prep.py <src> <out> [--module NAME]

The game tree is never edited. Rules (extend as CT-03 finds more):
  * `register T x asm("rN")` / `asm("fN")` pins become plain declarations.
  * `asm("" : ...)` statements are valid on x86 and are kept.
  * Known asm bodies are replaced by #include of the port's C versions (REPLACEMENTS).
  * `#line` directives are kept so diagnostics point at the real file.
  * `--module NAME` is informational; the Makefile passes -D_prolog=NAME_prolog etc. on the command line.
Based on tools/motion/host/prepare.py from the decomp.
"""
import re
import sys

# unit path suffix -> (regex of the function to remove, replacement text)
REPLACEMENTS = {
    'src/game/trans.cpp': [
        (r'void CalcSk1_x\(void\* dst, void\* src, u32 n\)\n\{\n    asm volatile\(.*?\);\n\}\n', '#include "re4port/skin_calcsk1.inc"\n'),
        (r'void CalcSk1_x2\(void\* dst, void\* src, u32 n\)\n\{\n    asm volatile\(.*?\);\n\}\n', '#include "re4port/skin_calcsk1_x2.inc"\n'),
        (r'void setupGQR6\(u32 v\)\n\{\n    asm volatile\("mtspr 918, %0" : : "r"\(v\)\);\n\}\n', 'void setupGQR6(u32 v)\n{\n    extern u32 re4_gqr6; re4_gqr6 = v;\n}\n'),
    ],
}

PSQ_MACROS = [
    # trans.cpp / shape.cpp / espgen: paired-single quantised loads become plain conversions
    (re.compile(r'#define PSQ_L_U8\(p\) .*'), '#define PSQ_L_U8(p) ((f32) *(const u8*) (p))'),
    (re.compile(r'#define PSQ_L_U8_TO\(dst, p\) .*'), '#define PSQ_L_U8_TO(dst, p) ((dst) = (f32) *(const u8*) (p))'),
    (re.compile(r'#define PSQ_L_S16\(p\) .*'), '#define PSQ_L_S16(p) ((f32) *(const s16*) (p))'),
    (re.compile(r'#define PSQ_ST_S16\(f, p\) .*'), '#define PSQ_ST_S16(f, p) (*(s16*) (p) = (s16) (f))'),
]


def prep(text, path):
    text = re.sub(r'\s*asm\("f?r\d+"\)', '', text)            # register pins
    for pat, rep in PSQ_MACROS:
        text = pat.sub(rep, text)
    for suffix, rules in REPLACEMENTS.items():
        if path.replace('\\', '/').endswith(suffix):
            for pat, rep in rules:
                text, n = re.subn(pat, rep, text, flags=re.S)
                if n == 0:
                    sys.stderr.write(f'prep.py: {path}: pattern not found: {pat[:50]}...\n')
    return text


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    src, out = args[0], args[1]
    with open(src, encoding='utf-8', errors='surrogateescape') as f:
        text = f.read()
    with open(out, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write(f'#line 1 "{src}"\n')
        f.write(prep(text, src))


if __name__ == '__main__':
    main()
