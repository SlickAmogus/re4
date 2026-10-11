#!/usr/bin/env python3
"""prep_cri.py -- the CRI Middleware (ADX / Sofdec / GCCI) part of the x86 source preparation (CT-07).

Imported by prep.py (the lead wires it in), or run standalone with the same arguments as prep.py:

    py -3 xbox/tools/prep_cri.py <src> <out>

Exports
  CRI_UNITS              the 149 CRI units of the DOL (configure.py CRI_LIBS), as 'src/lib/<name>.c'
  CRI_REPLACEMENTS       {unit suffix: [(regex, replacement)]}  -- same format as prep.REPLACEMENTS (re.S)
  CRI_SITE_PATCHES       {unit suffix: [(old, new)]}            -- same format as prep.SITE_PATCHES
  CRI_FUNC_REPLACEMENTS  {unit suffix: {function name: replacement text}} -- whole definitions
  cri_prep(text, path)   applies everything CRI-specific; call it on the raw text BEFORE prep.prep()
                         (it also handles the generic MWCC `asm { ... }` statement blocks, which prep.py
                         does not know about).

Why: CodeWarrior's `asm { ... }` statement blocks are parsed by nxdk's clang (i386-pc-win32, MS-style asm)
as x86 assembly, so every one of them must disappear before compiling. They are of three kinds:
  * codeless matching levers (`asm { mr x, x }`, `asm { mr r11, p; mr p, r11 }`) -> removed;
  * cache/SPR housekeeping (dcbi/dcbz_l/dcbt, mfspr HID2, mtspr GQRn)          -> removed / site patches;
  * real kernels (paired-single IDCT and merge, colour conversion, BE SWAR MC) -> whole functions
    replaced by portable C in xbox/port/include/re4port/cri/*.inc.
Any `asm { }` block left after that becomes RE4_CRI_ASM_UNPORTED() (traps at run time) and prep_cri
prints a warning, so nothing silently compiles as x86 garbage.

Endianness: the CRI code is big-endian. The kernels above are rewritten to produce the GameCube memory
image byte for byte; the bit readers and table punning sites are patched below (see the comments).
"""
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))

CRI_NAMES = (
    # libadxgc.a
    "adx_tlk adx_tsvr adx_rnaa lsc lsc_ini lsc_svr sj_rbf sj_utl sj_err svm ax_rna rna_res adx_inis adx_sjd "
    "adx_bahx adx_bsc adx_bsps adx_amp adx_crs adx_dcd adx_errs adx_fsvr adx_stmc adx_xpnd adx_mgc adx_dcd5 "
    "adx_fini adx_fs cri_cvfs lsc_err lsc_crs sj_crs sj_mem sj_uni rna_err rna_crs adx_bwav adx_baif adx_bau "
    "adx_fcch adx_insh adx_sfa adx_sje adx_sugc adx_gc adx_dcd3 "
    # libsfdgc.a / libmwsfdgc.a
    "mwsfdfrm mwsfdsvm mwsfdsfx mwsfdcre mwsfdlib mwsfdply mwsfdset mwsfdsvr mwsfdsst mwsfx_ARGB8888PLN "
    "mwsfx_Y84C44 sud_lib sfx_set sfx_cnv sfx_lib sfx_alp sfx_zmv sfx_sud sfx_cnv_to_ARGB8888PLN "
    "sfx_cnv_to_Y84C44 sfx_YCC420PLN_to_ARGB8888PLN sfx_YCC420PLN_to_Y84C44 sfd_adxt sfd_aoap sfd_buf sfd_con "
    "sfd_cre sfd_hds sfd_lib sfd_mem sfd_mps sfd_mpv sfd_mpvf sfd_pl2 sfd_ply sfd_pts sfd_see sfd_set sfd_tim "
    "sfd_trn sfd_uo sfd_vom sfd_tmr sfd_tst sfh_main cmptime memsetd muldiv muldivr uty_tmr mwsfdrna mwsfdrsc "
    "mwsfdsl mwstm mwsfdsee cftyp422_ppc cftfx cftcoladj sfx_inf mcp_not mps_dec mps_del mps_get mps_lib "
    "mpv_deli mpv_emp mpv_err mpv_frm mpv_get mpv_hdec mpv_lib mpv_vlc mpv_umc mpv_mcy mpv_m2v mpvabdec "
    "memcpyd uty_ppc dct_fsri mpv_bdec mpv_cdec mpv_cmc mpv_dec mpv_mc dct_ac dct_ver pl2link pl2enc cft_common "
    # libgcci
    "mfci gcci gcci_sub"
).split()
CRI_UNITS = tuple('src/lib/%s.c' % n for n in CRI_NAMES)
CRI_HEADERS = ('src/lib/cri/',)

_INC = '#include "re4port/cri/%s"\n'

# Whole function definitions replaced by portable C (name -> text). The text is inserted where the
# definition was, so file-static declarations above it stay visible.
CRI_FUNC_REPLACEMENTS = {
    # BE SWAR / PPC-asm motion compensation -> byte-wise C (same memory image)
    'src/lib/mpv_mc.c': {
        'mpvmc08_OneRefH2Body': '',
        'MPVMC08_OneRef4p_TuneC': '#define RE4_CRI_MC08 1\n' + _INC % 'mpv_mc_c.inc',
        'MPVMC08_OneRefH2_TuneC': '',
        'MPVMC08_OneRefV2_TuneC': '',
        'MPVMC08_OneRef1p_TuneC': '',
    },
    'src/lib/mpv_mcy.c': {
        'MPVMC16_OneRef4p_TuneC': '#define RE4_CRI_MC16 1\n' + _INC % 'mpv_mc_c.inc',
        'MPVMC16_OneRefH2_TuneC': '',
        'MPVMC16_OneRefV2_TuneC': '',
        'MPVMC16_OneRef1p_TuneC': '',
    },
    # paired-single merge kernels + GQR setup
    'src/lib/mpv_umc.c': {
        'mpvumc_BiMakeMb': _INC % 'mpv_umc_c.inc',
        'mpvumc_OneMakeMb': '',
        'mpvumc_OutputIntra6blk': '',
        'MPVUMC_SetGqr': '',
    },
    # paired-single AAN IDCT + GQR7 setup
    'src/lib/dct_fsri.c': {
        'DCT_FsriTransCore': _INC % 'dct_fsri_c.inc',
        'DCT_FsriSetGqr': '',
    },
    # colour converters (Y8/CbCr tiles, RGBA8 tiles)
    'src/lib/cftyp422_ppc.c': {
        'CFT_Ycc420plnToY84C44': '#define RE4_CFT_Y84C44 1\n' + _INC % 'cftyp422_c.inc',
        'cnvDynamicYcc420plnToArgb8888': '#define RE4_CFT_ARGB 1\n' + _INC % 'cftyp422_c.inc',
        'cnvStaticYcc420plnToArgb8888': '',
    },
    # PowerPC time base (mftb) -> the port's OSGetTime, which counts in the GameCube timer unit
    'src/lib/uty_tmr.c': {
        'utytmr_read': 'extern Sint64 OSGetTime(void);\n'
                       'static Sint64 utytmr_read(void) { return OSGetTime(); } /* prep_cri: was mftb */\n',
    },
    # GQR2..7 save/restore around the frame decode: no GQRs on x86
    'src/lib/uty_ppc.c': {
        'UTY_PopGqr': 'void UTY_PopGqr(UTY_GQR *gqr) { (void)gqr; }\n',
        'UTY_PushGqr': 'void UTY_PushGqr(UTY_GQR *gqr) { (void)gqr; }\n',
    },
}

# big-endian bit readers: every 32-bit window load of the MPEG bitstream gets a byte swap
_BSWAP_PTR = (r'\b(bbuf|nbuf) = \*ptr\+\+', r'\1 = __builtin_bswap32(*ptr++)')

# ADX streams are big-endian: every typed load from the stream/header bytes (`*(Sint16 *)src`,
# `*(Uint16 *)(data + 2)`, ...) becomes an explicit big-endian load. (All such casts in these units are
# reads of stream data; the PCM output buffers are written through plain Sint16 pointers.)
_BE_LOAD = (r'\*\(\s*(Sint16|Uint16|Sint32|Uint32)\s*\*\)\s*(\([^()]*\)|[A-Za-z_][\w.]*(?:->[\w.]+)*)',
            r'RE4_BE_\1(\2)')

# regex replacements (re.S), prep.REPLACEMENTS format
CRI_REPLACEMENTS = {
    'src/lib/adx_dcd.c': [_BE_LOAD],
    'src/lib/adx_dcd5.c': [_BE_LOAD],
    'src/lib/adx_bsc.c': [(r'if \(\*\(Uint16 \*\)buf == 0x8000\)', 'if (RE4_BE_Uint16(buf) == 0x8000)')],
    'src/lib/adx_sjd.c': [_BE_LOAD],
    'src/lib/adx_tlk.c': [_BE_LOAD],
    'src/lib/cri/mpv_bit.h': [_BSWAP_PTR],
    'src/lib/mpv_emp.c': [_BSWAP_PTR],
    'src/lib/mpvabdec.c': [_BSWAP_PTR],
    # the fixed-length run/level groups are `const Uint32` arrays read as Sint16 pairs (mpvabdec.c
    # `((const Sint16 *)mpv->rl_1)[i]`): swap the halves of every initialiser so element order survives
    'src/lib/mpv_vlc.c': [
        (r'(const Uint32 mpvvlt_run_level_(?:0a|0b|0c|1|2|4)\[\d+\] = \{)([^}]*)(\})',
         lambda m: m.group(1) + re.sub(r'0x([0-9A-Fa-f]{4})([0-9A-Fa-f]{4})', r'0x\2\1', m.group(2))
         + m.group(3) + ' /* prep_cri: halves swapped (read as Sint16 pairs) */'),
    ],
    'src/lib/mps_dec.c': [
        (r'\bnxt = \*p\+\+', r'nxt = __builtin_bswap32(*p++)'),
        (r'\bcur = p\[0\]', r'cur = __builtin_bswap32(p[0])'),
        (r'\bnxt = p\[1\]', r'nxt = __builtin_bswap32(p[1])'),
    ],
    # .bss pool addressing through asm (COMPILER-DIFF M2): name the objects instead; the version pointer
    # was stored through the pool at +0x400, which assumes the MWCC .bss layout
    'src/lib/dct_ac.c': [
        (r'asm \{ lis hi, dctac_i_const@ha; addi bss, hi, dctac_i_const@l \}[^\n]*\n',
         'bss = (Uint8 *)dctac_i_const; hi = bss; /* prep_cri */\n'),
        (r'\*\(const Char8 \*\*\)\(bss \+ 0x400\) = DCT_GetVerStr\(\);',
         'dctac_version_dummy = DCT_GetVerStr(); /* prep_cri: was a .bss pool store */'),
        (r'asm \{ addi ip, bss, __ArenaHi@l \}[^\n]*\n', 'ip = (Float64 *)dctac_i_const; /* prep_cri */\n'),
        (r'asm \{ addi fp, bss, 0x200 \}[^\n]*\n', 'fp = (Float64 *)dctac_f_const; /* prep_cri */\n'),
    ],
    # locked-cache housekeeping and the HID2 read: no L1 lock cache on x86 (hid2 = 0 -> LCE off)
    'src/lib/mpv_lib.c': [
        (r'asm \{ mfspr r0, 920 ; stw r0, hid2 \}', 'hid2 = 0; /* prep_cri: HID2 (no locked cache) */'),
        (r'asm \{ dcbi (\w+), i \}', r'(void)\1; /* prep_cri: dcbi */'),
        (r'asm \{ dcbz_l (\w+), i \}', r'(void)\1; /* prep_cri: dcbz_l */'),
    ],
}

# literal one-line patches, prep.SITE_PATCHES format
CRI_SITE_PATCHES = {
    # the bus clock from the OS globals (0x800000F8): GameCube 162 MHz, the timer runs at bus / 4
    'src/lib/uty_tmr.c': [('utytmr_unit = (*(Uint32 *)0x800000F8) >> 2;', 'utytmr_unit = 162000000 >> 2; /* prep_cri */')],
    # big-endian self checks: spin forever on a little-endian host
    'src/lib/mpv_lib.c': [('if (*(const Uint8 *)&test_wrok != 1) {', 'if (0) { /* prep_cri: BE self-check */')],
    'src/lib/mps_lib.c': [('if (*(const Uint8 *)&test_wrok != 1) {', 'if (0) { /* prep_cri: BE self-check */')],
    # the Sofdec header (SFH) stores its fields little-endian: the GameCube byte-swapped every native
    # load; on x86 the native load is already right
    'src/lib/sfh_main.c': [
        ('#define SWAP16(x) ((Uint16)((((x) & 0xFF) << 8) | (((x) >> 8) & 0xFF)))',
         '#define SWAP16(x) ((Uint16)(x)) /* prep_cri: LE header on an LE host */'),
        ('#define SWAP32(x) (((x) << 24) | (((x) << 8) & 0x00FF0000) | (((x) >> 8) & 0x0000FF00) | (((x) >> 24) & 0xFF))',
         '#define SWAP32(x) (x) /* prep_cri: LE header on an LE host */'),
        ('rate = (Sint16)(Uint16)(((SFH_ELEM_BITRATE(elem) & 0xFF) << 8) | (Uint8)(SFH_ELEM_BITRATE(elem) >> 8));',
         'rate = (Sint16)(Uint16)SFH_ELEM_BITRATE(elem); /* prep_cri: LE field */'),
    ],
    # the voice's current address read as one big-endian word (Hi:Lo halves)
    'src/lib/ax_rna.c': [('cur = *(Sint32 *)&vpb->pb.addr.currentAddressHi - rna->buf[nch - 1];',
                          'cur = (Sint32)(((Uint32)vpb->pb.addr.currentAddressHi << 16) | vpb->pb.addr.currentAddressLo)'
                          ' - rna->buf[nch - 1]; /* prep_cri: was a BE word pun */')],
    # MPV_OBJ.skip_func is declared `void (*)(MPV)` but points at MPVUMC_{P,B}picSkipped(mpv, n): on the
    # Gekko the increment `n` was still in r4 at the call; cdecl needs it passed explicitly
    'src/lib/mpv_dec.c': [('mpv->skip_func(mpv);',
                           '((void (*)(MPV, Sint32))mpv->skip_func)(mpv, (Sint32)n); /* prep_cri: r4-carried argument */')],
    # more calls that relied on an argument left in a register by the code before them (found with
    # a call/definition argument-count scan; the extra-argument calls are harmless under cdecl)
    'src/lib/mwsfdsvr.c': [('extern void mwPlyChkSupply(void);', 'extern void mwPlyChkSupply(MWPLY mwply); /* prep_cri */'),
                           ('mwPlyChkSupply();', 'mwPlyChkSupply(mwply); /* prep_cri: r3-carried argument */')],
    # bsize = the chunk length the original had in r5 (`len = ck.len` just above)
    'src/lib/adx_sjd.c': [('hdrlen = ADXB_DecodeHeader(adxb, ck.data);',
                           'hdrlen = ((Sint32 (*)(ADXB, Uint8 *, Sint32))ADXB_DecodeHeader)(adxb, ck.data, len);'
                           ' /* prep_cri: r5-carried argument */')],
    # vrate = prm->prm1 (59940), still in r4 (p1) at the call
    'src/lib/sfd_lib.c': [('SFTIM_Init(SFLIB_libwork.tim);',
                           '((void (*)(void *, Sint32))SFTIM_Init)(SFLIB_libwork.tim, p1); /* prep_cri: r4-carried argument */')],
    # pdclib declares int sscanf(const char *restrict, ...)
    'src/lib/sfx_zmv.c': [('extern Sint32 sscanf(const Char8 *s, const Char8 *fmt, ...);', '#include <stdio.h> /* prep_cri */')],
    # Float64 used as an 8-byte copier: x87 fld/fstp would quiet signalling-NaN bit patterns in pixel data
    'src/lib/mpv_umc.c': [('\tFloat64 *s;\n\tFloat64 *d;\n\tFloat64 a;\n\tFloat64 b;\n',
                           '\tUint64 *s;\n\tUint64 *d;\n\tUint64 a;\n\tUint64 b; /* prep_cri: was Float64 */\n'),
                          ('s = (Float64 *)(out->pln', 's = (Uint64 *)(out->pln'),
                          ('d = (Float64 *)(ref->pln', 'd = (Uint64 *)(ref->pln')],
}

# MWCC statement-level asm block
_ASM_BLOCK = re.compile(r'\basm\s*\{')
_CODELESS_OPS = {'mr', 'nop'}
_CACHE_OPS = {'dcbt', 'dcbtst', 'dcbz', 'dcbz_l', 'dcbi', 'dcbf', 'dcbst', 'sync', 'isync', 'eieio'}


def _comment_mask(text):
    """Sorted list of (start, end) ranges covered by comments or string literals."""
    out = []
    for m in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])\'', text, re.S):
        out.append((m.start(), m.end()))
    return out


def _in_ranges(pos, ranges):
    lo, hi = 0, len(ranges)
    while lo < hi:
        mid = (lo + hi) // 2
        s, e = ranges[mid]
        if pos < s:
            hi = mid
        elif pos >= e:
            lo = mid + 1
        else:
            return True
    return False


def _match_brace(text, open_idx):
    depth = 0
    j = open_idx
    while j < len(text):
        c = text[j]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return j
        j += 1
    raise ValueError('unbalanced braces')


def _asm_ops(body):
    body = re.sub(r'/\*.*?\*/|//[^\n]*', '', body, flags=re.S)
    ops = []
    for stmt in re.split(r'[;\n]', body):
        stmt = stmt.strip()
        if not stmt or stmt.endswith(':'):
            continue
        ops.append(stmt.split()[0])
    return ops


def strip_asm_blocks(text, path, warn=True):
    ranges = _comment_mask(text)
    out = []
    pos = 0
    for m in _ASM_BLOCK.finditer(text):
        if m.start() < pos or _in_ranges(m.start(), ranges):
            continue
        close = _match_brace(text, m.end() - 1)
        ops = _asm_ops(text[m.end():close])
        if all(o in _CODELESS_OPS for o in ops):
            rep = '((void) 0); /* prep_cri: codeless asm lever */'
        elif all(o in _CODELESS_OPS | _CACHE_OPS for o in ops):
            rep = '((void) 0); /* prep_cri: cache op */'
        else:
            line = text.count('\n', 0, m.start()) + 1
            if warn:
                sys.stderr.write('prep_cri: %s:%d: unported asm block (%s)\n' % (path, line, ' '.join(ops[:6])))
            rep = 'RE4_CRI_ASM_UNPORTED(); /* prep_cri: see warning */'
        out.append(text[pos:m.start()])
        out.append(rep)
        pos = close + 1
    out.append(text[pos:])
    return ''.join(out)


def replace_function(text, name, rep):
    """Replace the definition of `name` (return type through closing brace) with `rep`."""
    ranges = _comment_mask(text)
    pat = re.compile(r'^[A-Za-z_][^;{}()\n]*?\b' + re.escape(name) + r'\s*\(([^;{}]*?)\)\s*\{', re.M)
    for m in pat.finditer(text):
        if _in_ranges(m.start(), ranges):
            continue
        close = _match_brace(text, m.end() - 1)
        end = close + 1
        if end < len(text) and text[end] == '\n':
            end += 1
        return text[:m.start()] + rep + text[end:], True
    return text, False


def cri_prep(text, path):
    """CRI-specific preparation of one unit or header (raw text in, text out); no-op for other files."""
    upath = path.replace('\\', '/')
    is_unit = upath.endswith(CRI_UNITS)
    is_hdr = any(h in upath for h in CRI_HEADERS)
    if not (is_unit or is_hdr):
        return text
    for suffix, funcs in CRI_FUNC_REPLACEMENTS.items():
        if upath.endswith(suffix):
            for name, rep in funcs.items():
                text, ok = replace_function(text, name, rep)
                if not ok:
                    sys.stderr.write('prep_cri: %s: function not found: %s\n' % (path, name))
    for suffix, rules in CRI_REPLACEMENTS.items():
        if upath.endswith(suffix):
            for pat, rep in rules:
                text, n = re.subn(pat, rep, text, flags=re.S)
                if n == 0:
                    sys.stderr.write('prep_cri: %s: pattern not found: %s\n' % (path, pat[:60]))
    for suffix, patches in CRI_SITE_PATCHES.items():
        if upath.endswith(suffix):
            for old, new in patches:
                if old not in text:
                    sys.stderr.write('prep_cri: %s: site patch not found: %s\n' % (path, old[:60]))
                text = text.replace(old, new)
    text = strip_asm_blocks(text, path)
    if is_unit:
        text = '#include "re4port/cri_x86.h"\n#line 1 "%s"\n' % upath + text
    return text


def main():
    sys.path.insert(0, _HERE)
    import prep
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    orig = prep.prep

    def prep_with_cri(text, path):
        return orig(cri_prep(text, path), path)
    prep.prep = prep_with_cri
    if '--dir' in sys.argv:
        prep.prep_dir(args[0], args[1])
    else:
        prep.prep_file(args[0], args[1])


if __name__ == '__main__':
    main()
