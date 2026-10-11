#!/usr/bin/env python3
"""gen_tev_cases.py — turns a CT-05 pipeline catalogue (xbox/port/gx/tests/tev_cases.schema.json) into a
C++ file that replays every case into the GX state machine through the real GXSet* entry points.

    python3 gen_tev_cases.py <catalogue.json> <out.cpp> [--schema <schema.json>]

Each case becomes `static int case_<id>(void)` returning 1 when the replay is approximate: a value the
catalogue only knows at run time (a string starting with '?') is replaced by the first concrete enum of
its kind, and constructs the state machine cannot express (indirect kinds "tile"/"other", unparsable
colour strings) fall back to a neutral setting. Enum names are emitted verbatim and resolved by the C++
compiler against include/dolphin/gx/GXEnum.h, so a typo in the catalogue is a compile error, not a
silently wrong test. The file ends with the table `gx_tev_cases[]` / `gx_tev_case_count` that
xbox/port/tests/gx_test.cpp walks.

The catalogue is validated against the schema when the `jsonschema` module is available; the required
keys are checked by hand otherwise.
"""
import json
import os
import re
import sys

ENUM_RE = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*(\s*[+|]\s*[A-Za-z0-9_]+)*$')

# First concrete enum of each kind, used for '?' values.
DEFAULTS = {
    'coord': 'GX_TEXCOORD0', 'map': 'GX_TEXMAP0', 'channel': 'GX_COLOR0A0',
    'cc': 'GX_CC_ZERO', 'ca': 'GX_CA_ZERO',
    'op': 'GX_TEV_ADD', 'bias': 'GX_TB_ZERO', 'scale': 'GX_CS_SCALE_1', 'out': 'GX_TEVPREV',
    'kcsel': 'GX_TEV_KCSEL_1', 'kasel': 'GX_TEV_KASEL_1', 'swap': 'GX_TEV_SWAP0',
    'ind_stage': 'GX_INDTEXSTAGE0', 'matrix_sel': 'GX_ITM_0', 'its': 'GX_ITS_1',
    'tg_func': 'GX_TG_MTX2x4', 'tg_src': 'GX_TG_TEX0', 'texmtx': 'GX_IDENTITY', 'ptmtx': 'GX_PTIDENTITY',
    'chan': 'GX_COLOR0A0', 'colorsrc': 'GX_SRC_REG', 'diff_fn': 'GX_DF_NONE', 'attn_fn': 'GX_AF_NONE',
    'kcolor': 'GX_KCOLOR0', 'tevreg': 'GX_TEVPREV', 'ch': 'GX_CH_RED',
    'bm': 'GX_BM_NONE', 'bl': 'GX_BL_ONE', 'lo': 'GX_LO_CLEAR', 'compare': 'GX_ALWAYS', 'aop': 'GX_AOP_AND',
    'cull': 'GX_CULL_NONE', 'fog': 'GX_FOG_NONE', 'tevmode': 'GX_MODULATE',
}

REQUIRED_CASE = ['id', 'source', 'num_tev_stages', 'num_texgens', 'num_chans', 'stages', 'nv2a']
REQUIRED_STAGE = ['stage', 'order', 'color_in', 'color_op', 'alpha_in', 'alpha_op']


class Case:
    def __init__(self, data):
        self.data = data
        self.approx = []   # reasons
        self.lines = []

    def enum(self, value, kind):
        """Returns C++ text for an enum-valued field."""
        if isinstance(value, bool):
            return '1' if value else '0'
        if isinstance(value, int):
            return str(value)
        if value is None:
            self.approx.append('%s missing' % kind)
            return DEFAULTS[kind]
        s = str(value).strip()
        if s.startswith('?'):
            self.approx.append('%s = %s' % (kind, s))
            return DEFAULTS[kind]
        if ENUM_RE.match(s):
            return s
        self.approx.append('%s = %r not an enum' % (kind, s))
        return DEFAULTS[kind]

    def count(self, value, what, default):
        if isinstance(value, bool):
            return str(int(value))
        if isinstance(value, int):
            return str(value)
        self.approx.append('%s = %r' % (what, value))
        return str(default)

    def color(self, value, what):
        """'#RRGGBB[AA]', '0xRRGGBBAA', 'r,g,b,a', '{r,g,b,a}' -> (r,g,b,a) ints, or None."""
        if isinstance(value, (list, tuple)) and len(value) in (3, 4):
            v = [int(x) for x in value]
            return v + [255] * (4 - len(v))
        s = str(value).strip().strip('{}()')
        m = re.match(r'^(?:#|0x)([0-9A-Fa-f]{6})([0-9A-Fa-f]{2})?$', s)
        if m:
            rgb = int(m.group(1), 16)
            a = int(m.group(2), 16) if m.group(2) else 255
            return [(rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a]
        parts = [p.strip() for p in s.split(',')]
        if len(parts) in (3, 4) and all(re.match(r'^-?\d+$', p) for p in parts):
            v = [int(p) for p in parts]
            return v + [255] * (4 - len(v))
        self.approx.append('%s = %r unparsable colour' % (what, value))
        return None

    def light_mask(self, value):
        if value is None:
            return 'GX_LIGHT_NULL'
        if isinstance(value, int):
            return str(value)
        s = str(value).strip()
        if s.startswith('?') or not re.match(r'^[A-Za-z0-9_|\s]+$', s):
            self.approx.append('light_mask = %r' % s)
            return 'GX_LIGHT_NULL'
        return s

    def emit(self, line):
        self.lines.append('    ' + line)

    def generate(self):
        d = self.data
        e = self.emit
        e('GXSetNumTevStages(%s);' % self.count(d['num_tev_stages'], 'num_tev_stages', 1))
        e('GXSetNumTexGens(%s);' % self.count(d['num_texgens'], 'num_texgens', 1))
        e('GXSetNumChans(%s);' % self.count(d['num_chans'], 'num_chans', 0))

        for st in d.get('stages', []):
            n = st['stage']
            stage = 'GX_TEVSTAGE%d' % n
            o = st['order']
            e('GXSetTevOrder(%s, %s, %s, %s);' % (stage, self.enum(o.get('coord'), 'coord'),
                                                  self.enum(o.get('map'), 'map'), self.enum(o.get('channel'), 'channel')))
            via = st.get('via_tev_op')
            if via:
                e('GXSetTevOp(%s, %s);' % (stage, self.enum(via, 'tevmode')))
            else:
                ci = [self.enum(x, 'cc') for x in st['color_in']]
                ai = [self.enum(x, 'ca') for x in st['alpha_in']]
                co, ao = st['color_op'], st['alpha_op']
                e('GXSetTevColorIn(%s, %s);' % (stage, ', '.join(ci)))
                e('GXSetTevColorOp(%s, %s, %s, %s, %s, %s);' % (stage, self.enum(co.get('op'), 'op'), self.enum(co.get('bias'), 'bias'),
                                                                 self.enum(co.get('scale'), 'scale'), self.enum(co.get('clamp', True), 'op'),
                                                                 self.enum(co.get('out'), 'out')))
                e('GXSetTevAlphaIn(%s, %s);' % (stage, ', '.join(ai)))
                e('GXSetTevAlphaOp(%s, %s, %s, %s, %s, %s);' % (stage, self.enum(ao.get('op'), 'op'), self.enum(ao.get('bias'), 'bias'),
                                                                 self.enum(ao.get('scale'), 'scale'), self.enum(ao.get('clamp', True), 'op'),
                                                                 self.enum(ao.get('out'), 'out')))
            if st.get('kcsel'):
                e('GXSetTevKColorSel(%s, %s);' % (stage, self.enum(st['kcsel'], 'kcsel')))
            if st.get('kasel'):
                e('GXSetTevKAlphaSel(%s, %s);' % (stage, self.enum(st['kasel'], 'kasel')))
            sw = st.get('swap')
            if sw:
                e('GXSetTevSwapMode(%s, %s, %s);' % (stage, self.enum(sw.get('ras'), 'swap'), self.enum(sw.get('tex'), 'swap')))
            ind = st.get('indirect')
            if ind:
                kind = ind.get('kind', 'other')
                ind_stage = self.enum(ind.get('ind_stage', 'GX_INDTEXSTAGE0'), 'ind_stage')
                mtx = self.enum(ind.get('matrix_sel', 'GX_ITM_0'), 'matrix_sel')
                if kind == 'direct':
                    e('GXSetTevDirect(%s);' % stage)
                elif kind == 'warp':
                    e('GXSetTevIndWarp(%s, %s, %d, %d, %s);' % (stage, ind_stage, int(bool(ind.get('signed_offset', False))),
                                                                int(bool(ind.get('replace_mode', False))), mtx))
                elif kind == 'bump_xyz':
                    e('GXSetTevIndBumpXYZ(%s, %s, %s);' % (stage, ind_stage, mtx))
                else:
                    self.approx.append('indirect kind %r on stage %d not expressible' % (kind, n))
                    e('GXSetTevIndWarp(%s, %s, 0, 0, %s);   /* approximation of kind "%s" */' % (stage, ind_stage, mtx, kind))

        for tg in d.get('texgens', []):
            e('GXSetTexCoordGen2(%s, %s, %s, %s, %s, %s);' % (
                self.enum(tg.get('coord'), 'coord'), self.enum(tg.get('func'), 'tg_func'), self.enum(tg.get('src'), 'tg_src'),
                self.enum(tg.get('mtx'), 'texmtx'), self.enum(tg.get('normalize', False), 'op'),
                self.enum(tg.get('postmtx', 'GX_PTIDENTITY'), 'ptmtx')))

        for ch in d.get('channels', []):
            e('GXSetChanCtrl(%s, %s, %s, %s, %s, %s, %s);' % (
                self.enum(ch.get('chan'), 'chan'), self.enum(ch.get('enable', False), 'op'),
                self.enum(ch.get('amb_src', 'GX_SRC_REG'), 'colorsrc'), self.enum(ch.get('mat_src', 'GX_SRC_VTX'), 'colorsrc'),
                self.light_mask(ch.get('light_mask')), self.enum(ch.get('diff_fn', 'GX_DF_NONE'), 'diff_fn'),
                self.enum(ch.get('attn_fn', 'GX_AF_NONE'), 'attn_fn')))

        for ind in d.get('ind_stages', []):
            s = self.enum(ind.get('ind_stage'), 'ind_stage')
            e('GXSetIndTexOrder(%s, %s, %s);' % (s, self.enum(ind.get('coord'), 'coord'), self.enum(ind.get('map'), 'map')))
            e('GXSetIndTexCoordScale(%s, %s, %s);' % (s, self.enum(ind.get('scale_s', 'GX_ITS_1'), 'its'),
                                                       self.enum(ind.get('scale_t', 'GX_ITS_1'), 'its')))

        for kc in d.get('kcolors', []):
            c = self.color(kc.get('value'), 'kcolor %s' % kc.get('id'))
            if c is None:
                c = [255, 255, 255, 255]
            e('{ GXColor c = { %d, %d, %d, %d }; GXSetTevKColor(%s, c); }' % (c[0] & 255, c[1] & 255, c[2] & 255, c[3] & 255,
                                                                              self.enum(kc.get('id'), 'kcolor')))
        for tr in d.get('tev_regs', []):
            c = self.color(tr.get('value'), 'tev_reg %s' % tr.get('id'))
            if c is None:
                c = [0, 0, 0, 0]
            e('{ GXColorS10 c = { %d, %d, %d, %d }; GXSetTevColorS10(%s, c); }' % (c[0], c[1], c[2], c[3], self.enum(tr.get('id'), 'tevreg')))

        for sw in d.get('swap_tables', []):
            e('GXSetTevSwapModeTable(%s, %s, %s, %s, %s);' % (self.enum(sw.get('table'), 'swap'), self.enum(sw.get('r'), 'ch'),
                                                              self.enum(sw.get('g'), 'ch'), self.enum(sw.get('b'), 'ch'), self.enum(sw.get('a'), 'ch')))

        b = d.get('blend')
        if b:
            e('GXSetBlendMode(%s, %s, %s, %s);' % (self.enum(b.get('type'), 'bm'), self.enum(b.get('src'), 'bl'),
                                                   self.enum(b.get('dst'), 'bl'), self.enum(b.get('op'), 'lo')))
        z = d.get('zmode')
        if z:
            e('GXSetZMode(%s, %s, %s);' % (self.enum(z.get('compare', True), 'op'), self.enum(z.get('func'), 'compare'),
                                           self.enum(z.get('update', True), 'op')))
        if d.get('z_comp_loc') is not None:
            e('GXSetZCompLoc(%d);' % int(bool(d['z_comp_loc'])))
        ac = d.get('alpha_compare')
        if ac:
            e('GXSetAlphaCompare(%s, %s, %s, %s, %s);' % (
                self.enum(ac.get('comp0'), 'compare'), self.count(ac.get('ref0', 0), 'alpha ref0', 0),
                self.enum(ac.get('op'), 'aop'), self.enum(ac.get('comp1'), 'compare'), self.count(ac.get('ref1', 0), 'alpha ref1', 0)))
        if d.get('cull'):
            e('GXSetCullMode(%s);' % self.enum(d['cull'], 'cull'))
        fog = d.get('fog')
        if fog:
            s = str(fog).strip()
            m = re.match(r'^(GX_FOG_[A-Z0-9_]+)', s)
            if m:
                if s != m.group(1):
                    self.approx.append('fog parameters %r' % s)
                e('{ GXColor c = { 0, 0, 0, 0 }; GXSetFog(%s, 0.0f, 1.0f, 0.1f, 1.0f, c); }' % m.group(1))
            else:
                self.approx.append('fog = %r' % s)


def validate(cat, schema_path):
    if schema_path and os.path.exists(schema_path):
        try:
            import jsonschema  # type: ignore
            with open(schema_path) as f:
                jsonschema.validate(cat, json.load(f))
            return 'schema'
        except ImportError:
            pass
    if cat.get('version') != 1 or not isinstance(cat.get('cases'), list):
        raise SystemExit('catalogue: version must be 1 and cases a list')
    for c in cat['cases']:
        for k in REQUIRED_CASE:
            if k not in c:
                raise SystemExit('case %r: missing %r' % (c.get('id'), k))
        if not re.match(r'^[a-z0-9_]+$', c['id']):
            raise SystemExit('case id %r is not ^[a-z0-9_]+$' % c['id'])
        for st in c['stages']:
            for k in REQUIRED_STAGE:
                if k not in st:
                    raise SystemExit('case %r stage: missing %r' % (c['id'], k))
    return 'required keys'


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    src, out = argv[1], argv[2]
    schema = None
    if '--schema' in argv:
        schema = argv[argv.index('--schema') + 1]
    else:
        here = os.path.dirname(os.path.abspath(__file__))
        schema = os.path.join(here, '..', 'gx', 'tests', 'tev_cases.schema.json')
    with open(src) as f:
        cat = json.load(f)
    how = validate(cat, schema)

    cases = []
    seen = set()
    for data in cat['cases']:
        if data['id'] in seen:
            raise SystemExit('duplicate case id %r' % data['id'])
        seen.add(data['id'])
        c = Case(data)
        c.generate()
        cases.append(c)

    o = []
    o.append('// Generated by xbox/port/tests/gen_tev_cases.py from %s (validated: %s). Do not edit.' % (os.path.basename(src), how))
    o.append('#include "re4port/re4_xbox.h"')
    o.append('#include "gx.h"')
    o.append('#include <dolphin/gx/GXEnum.h>')
    o.append('#include "gx_test_cases.h"')
    o.append('')
    for c in cases:
        d = c.data
        o.append('// %s: %s:%s (%s)' % (d['id'], d['source'].get('file'), d['source'].get('line'), d['source'].get('function')))
        for a in c.approx:
            o.append('//   approximate: %s' % a)
        o.append('static int case_%s(void)' % d['id'])
        o.append('{')
        o.extend(c.lines)
        o.append('    return %d;' % (1 if c.approx else 0))
        o.append('}')
        o.append('')
    o.append('const GxTevCase gx_tev_cases[] = {')
    for c in cases:
        d = c.data
        nv = d.get('nv2a', {})
        o.append('    { "%s", case_%s, "%s", %d, %d, %d },' % (
            d['id'], d['id'], d['source'].get('file', ''), 1 if c.approx else 0,
            int(bool(nv.get('fits_8_stages', True))), int(nv.get('textures', 0) or 0)))
    o.append('};')
    o.append('const int gx_tev_case_count = %d;' % len(cases))
    o.append('')
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w') as f:
        f.write('\n'.join(o))
    approx = sum(1 for c in cases if c.approx)
    sys.stderr.write('gen_tev_cases: %d cases (%d approximate) from %s -> %s\n' % (len(cases), approx, src, out))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
