"""re4conv command line.

usage: py -3 -m re4conv convert <disc1/files> [<disc2/files>] <out> [--only PATTERN ...] [-q]
       py -3 -m re4conv check   <disc1/files> [<disc2/files>] <out> [--only PATTERN ...]
       py -3 -m re4conv file    <input file> <output file>          (one file, prints its record)

(run from xbox/tools, or with xbox/tools on PYTHONPATH)

convert walks both disc trees (relative paths as spelled on the disc; a path present on both discs
with identical bytes is converted once, with different bytes the disc-2 copy goes to
out/files_disc2/<path>), dispatches each file on extension / content and writes out/files/<path>
plus out/manifest.json. --only takes fnmatch patterns on the relative path (case-insensitive), e.g.
--only em/pl00.drs St1/r100*. Statuses: converted, partial (a container with passthrough entries),
passthrough (copied verbatim), dropped (Rel/*.rel), failed (copied verbatim, error recorded).

check reloads the manifest and re-validates every output against its original: converted entries
through the module's check(), passthrough bytes for identity. Exit status 1 on any problem.
"""
import fnmatch
import hashlib
import json
import os
import sys
import time

if __package__ in (None, ''):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = 're4conv'

from re4conv import ConvertError, container, model, motion, simple, sound, tpl  # noqa: E402
import archive  # noqa: E402  (tools/motion, via re4conv)

STATUSES = ('converted', 'partial', 'passthrough', 'dropped', 'failed')


def _walk(root):
    for dp, _, fns in os.walk(root):
        for fn in fns:
            p = os.path.join(dp, fn)
            yield os.path.relpath(p, root).replace(os.sep, '/'), p


def _sha1(d):
    return hashlib.sha1(d).hexdigest()


def classify(rel: str, d: bytes) -> str:
    low = rel.lower()
    ext = os.path.splitext(low)[1]
    if ext == '.rel':
        return 'rel'
    if simple.handler(rel):
        return 'simple'
    if sound.handler(rel):
        return 'sound'
    if ext == '.sbb':
        return 'sbb'
    if ext in ('.drs', '.das') and archive.is_hakase(d):
        return 'hakase'
    if ext == '.evd' and d[:5] == b'event':
        return 'event'
    if ext == '.dat' and archive.is_body(d):
        return 'body'
    if ext == '.fnt':
        return 'FNT'
    if ext == '.uwf':
        return 'UWF'
    if ext == '.eff' and d[:4] == bytes(3) + bytes([0xB]):
        return 'EFF'
    if ext == '.tpl':
        return 'TPL'
    if ext == '.bin':
        return 'BIN'
    if ext == '.fcv':
        return 'FCV'
    return 'raw'


def _count_entries(recs, acc):
    for r in recs:
        acc[r['status']] = acc.get(r['status'], 0) + 1
        if 'files' in r:
            _count_entries(r['files'], acc)


def convert_file(rel: str, d: bytes):
    """-> (output bytes or None for dropped, record)."""
    kind = classify(rel, d)
    rec = {'kind': kind, 'in': len(d), 'sha1_in': _sha1(d)}
    out = d
    try:
        if kind == 'rel':
            # code linked statically; the OSModuleHeader stays (module_table.cpp reads the module id)
            from . import container as _c
            out = _c.rel_header_le(d)
            rec['status'] = 'converted'
            rec['format'] = 'rel_header'
            rec['note'] = 'module linked statically; OSModuleHeader kept (0x4C bytes, LE)'
            rec['out'] = len(out)
            rec['sha1_out'] = _sha1(out)
            return out, rec
        if kind == 'simple':
            name, conv, _ = simple.handler(rel)
            rec['format'] = name
            out = conv(d)
            rec['status'] = 'converted'
        elif kind == 'sound':
            name, conv, _ = sound.handler(rel)
            rec['format'] = name
            out = conv(d)
            rec['status'] = 'converted'
            rec['note'] = 'tables little-endian, DSP-ADPCM samples unchanged (CT-14)'
        elif kind == 'sbb':
            rec['status'] = 'passthrough'
            rec['note'] = 'raw DSP-ADPCM stream data, endian-neutral (kept as DSP-ADPCM, CT-14)'
        elif kind in ('hakase', 'event', 'body'):
            fn = {'hakase': container.convert_hakase, 'event': container.convert_event,
                  'body': container.convert_body}[kind]
            out, info = fn(d)
            rec.update(info)
            acc = {}
            _count_entries(info['entries'], acc)
            rec['entry_counts'] = acc
            rec['status'] = info['status']
        elif kind in container.ENTRY:
            out, er = container.convert_entry(kind, d)
            rec['status'] = er['status']
            if 'error' in er:
                rec['error'] = er['error']
            if 'note' in er:
                rec['note'] = er['note']
        else:
            rec['status'] = 'passthrough'
    except (ConvertError, ValueError, IndexError, AssertionError) as e:
        out = d
        rec['status'] = 'failed'
        rec['error'] = f'{type(e).__name__}: {e}'
    rec['out'] = len(out)
    return out, rec


def check_file(rel, out: bytes, orig: bytes, rec) -> list:
    st, kind = rec['status'], rec['kind']
    if st in ('passthrough', 'failed'):
        return [] if out == orig else ['passthrough bytes differ']
    if st == 'dropped':
        return []
    try:
        if kind == 'simple':
            return simple.handler(rel)[2](out, orig)
        if kind == 'sound':
            return sound.handler(rel)[2](out, orig)
        if kind == 'hakase':
            return container.check_hakase(out, orig, rec)
        if kind == 'event':
            return container.check_event(out, orig, rec)
        if kind == 'body':
            return container.check_body(out, orig, rec)
        if kind in container.ENTRY:
            return container.check_entry(kind, out, orig)
        if kind == 'rel':
            return [] if out == container.rel_header_le(orig) else ['REL header does not match the original']
    except (ConvertError, ValueError, IndexError, AssertionError) as e:
        return [f'check raised {type(e).__name__}: {e}']
    return [f'no checker for kind {kind}']


def _sources(roots, only):
    """[(rel, path, disc index)] with the disc-2 duplicate rule applied later."""
    out = []
    for di, root in enumerate(roots):
        for rel, p in sorted(_walk(root)):
            if only and not any(fnmatch.fnmatch(rel.lower(), pat.lower()) for pat in only):
                continue
            out.append((rel, p, di))
    return out


def _out_path(out_root, rel, di, dup):
    sub = 'files_disc2' if dup else 'files'
    return os.path.join(out_root, sub, *rel.split('/'))


def cmd_convert(roots, out_root, only, quiet):
    man_path = os.path.join(out_root, 'manifest.json')
    manifest = {'tool': 're4conv', 'format_version': 1, 'inputs': [os.path.abspath(r) for r in roots],
                'files': {}}
    if only and os.path.exists(man_path):   # incremental: keep earlier records
        manifest['files'] = json.load(open(man_path, encoding='utf-8')).get('files', {})
    seen, seen_rel = {}, {}
    t0 = time.time()
    for rel, p, di in _sources(roots, only):
        d = open(p, 'rb').read()
        key = rel.lower()
        dup = False
        if key in seen:
            if seen[key] == _sha1(d):
                manifest['files'][seen_rel[key]]['discs'].append(di + 1)
                continue
            dup = True
        t1 = time.time()
        out, rec = convert_file(rel, d)
        rec['disc'] = di + 1
        rec['discs'] = [di + 1]
        rec['seconds'] = round(time.time() - t1, 2)
        if out is not None:
            op = _out_path(out_root, rel, di, dup)
            os.makedirs(os.path.dirname(op), exist_ok=True)
            with open(op, 'wb') as f:
                f.write(out)
            rec['output'] = os.path.relpath(op, out_root).replace(os.sep, '/')
        mkey = rel if not dup else f'disc2:{rel}'
        manifest['files'][mkey] = rec
        if not dup:
            seen[key] = rec['sha1_in']
            seen_rel[key] = mkey
        if not quiet:
            ec = rec.get('entry_counts')
            print(f'{rel}: {rec["status"]} ({rec["kind"]}) {rec["in"]} -> {rec.get("out", 0)}'
                  + (f' entries {ec}' if ec else '') + (f' ERROR {rec["error"]}' if 'error' in rec else ''),
                  flush=True)
    manifest['summary'] = _summary(manifest['files'])
    manifest['seconds'] = round(time.time() - t0, 1)
    os.makedirs(out_root, exist_ok=True)
    with open(man_path, 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=1)
        f.write('\n')
    print(json.dumps(manifest['summary'], indent=1))
    return 0



def _summary(files):
    s = {'files': {k: 0 for k in STATUSES}, 'entries': {}}
    for rec in files.values():
        s['files'][rec['status']] = s['files'].get(rec['status'], 0) + 1
        for k, v in rec.get('entry_counts', {}).items():
            s['entries'][k] = s['entries'].get(k, 0) + v
    return s


def cmd_check(roots, out_root, only):
    manifest = json.load(open(os.path.join(out_root, 'manifest.json'), encoding='utf-8'))
    srcs = {}
    for rel, p, di in _sources(roots, None):
        srcs.setdefault(rel if di == 0 else f'disc2:{rel}', p)
        srcs.setdefault(rel, p)
    bad = ok = 0
    for key, rec in manifest['files'].items():
        rel = key.split(':', 1)[1] if key.startswith('disc2:') else key
        if only and not any(fnmatch.fnmatch(rel.lower(), pat.lower()) for pat in only):
            continue
        if rec['status'] == 'dropped':
            ok += 1
            continue
        src = srcs.get(key) or srcs.get(rel)
        if src is None:
            print(f'{key}: original not found')
            bad += 1
            continue
        orig = open(src, 'rb').read()
        out = open(os.path.join(out_root, rec['output']), 'rb').read()
        probs = check_file(rel, out, orig, rec)
        if probs:
            bad += 1
            print(f'{key}: {rec["status"]}: {len(probs)} problem(s): ' + '; '.join(probs[:5]))
        else:
            ok += 1
    print(f'check: {ok} ok, {bad} with problems')
    return 1 if bad else 0


def cmd_file(inp, outp):
    d = open(inp, 'rb').read()
    rel = os.path.basename(inp)
    parts = inp.replace('\\', '/').split('/')
    if 'files' in parts:
        rel = '/'.join(parts[len(parts) - 1 - parts[::-1].index('files') + 1:])
    out, rec = convert_file(rel, d)
    if out is not None:
        with open(outp, 'wb') as f:
            f.write(out)
        probs = check_file(rel, out, d, rec)
        rec['check'] = probs or 'ok'
    rec.pop('entries', None)
    print(json.dumps(rec, indent=1))
    return 0 if rec['status'] != 'failed' else 1


def main(argv):
    args = [a for a in argv if a not in ('-q',)]
    quiet = '-q' in argv
    only = []
    if '--only' in args:
        i = args.index('--only')
        only = args[i + 1:]
        args = args[:i]
    if not args:
        sys.exit(__doc__)
    cmd, rest = args[0], args[1:]
    if cmd == 'file' and len(rest) == 2:
        return cmd_file(*rest)
    if cmd in ('convert', 'check') and len(rest) in (2, 3):
        roots, out_root = rest[:-1], rest[-1]
        for r in roots:
            if not os.path.isdir(r):
                sys.exit(f'{r}: not a directory')
        return cmd_convert(roots, out_root, only, quiet) if cmd == 'convert' else cmd_check(roots, out_root, only)
    sys.exit(__doc__)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
