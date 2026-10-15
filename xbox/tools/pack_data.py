#!/usr/bin/env python3
"""pack_data.py — bundles the converted data tree into one pack file (or a few parts) the XBE reads directly.

    python3 xbox/tools/pack_data.py <data dir> <out.pak> [--split MB] [--level N] [--store-only]
    python3 xbox/tools/pack_data.py --list <out.pak>
    python3 xbox/tools/pack_data.py --verify <data dir> <out.pak>

<data dir> is the folder holding files/ and files_disc2/ (package_xbox.sh's data/). Every file becomes one
entry named by its path relative to <data dir> with forward slashes, e.g. "files/em/em10.drs".

Format (little-endian), read by xbox/port/dvd/pak.c:
  header (64 bytes)  char magic[8] "RE4PAK1\\0"; u32 version 1; u32 chunk_size; u32 entry_count;
                     u32 chunk_count; u32 names_size; u32 volume_count; u64 volume_size (0 = one file);
                     u64 index_offset; u64 index_size; u8 pad[8]
  data               chunk payloads back to back, in entry order
  index              entries[entry_count]  {u32 name_ofs; u32 size; u32 first_chunk; u32 pad}
                     chunks[chunk_count]   {u64 offset; u32 stored_size; u32 flags}  flags bit0 = deflated
                     names                 NUL-terminated paths, entries sorted by name (byte order)
Each entry is cut into chunk_size (64 KB) pieces; a piece is stored zlib-compressed when that saves at
least 1/16, otherwise raw, so movies and ADPCM streams cost nothing extra to read. Offsets are positions in
the logical stream; with --split it is cut into volumes of exactly --split MB named <out>.000, .001, ...
(the header is in volume 0 and covers the whole set). A chunk may straddle two volumes.
"""
import os
import struct
import sys
import zlib

MAGIC = b'RE4PAK1\0'
CHUNK = 0x10000
HDR = struct.Struct('<8sIIIIIIQQQ8s')
ENTRY = struct.Struct('<IIII')
CHUNKREC = struct.Struct('<QII')
assert HDR.size == 64


class Out:
    """Writes the logical stream, cutting it into volumes when split."""

    def __init__(self, path, volume_size):
        self.path, self.vsize = path, volume_size
        self.pos = 0
        self.files = []
        self.f = None
        self._open(0)

    def _name(self, i):
        return self.path if not self.vsize else f'{self.path}.{i:03d}'

    def _open(self, i):
        if self.f:
            self.f.close()
        self.f = open(self._name(i), 'w+b')
        self.files.append(self._name(i))

    def write(self, b):
        while b:
            if self.vsize:
                while self.pos // self.vsize >= len(self.files):   # crossed into the next volume
                    self._open(len(self.files))
                room = self.vsize - (self.pos % self.vsize)
                part, b = b[:room], b[room:]
            else:
                part, b = b, b''
            self.f.write(part)
            self.pos += len(part)

    def finish(self, header):
        self.f.close()
        with open(self._name(0), 'r+b') as f:
            f.write(header)
        return self.files


def gather(root):
    names = []
    for dp, _, fn in os.walk(root):
        for f in fn:
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, root).replace(os.sep, '/')
            if rel.startswith(('files/', 'files_disc2/')):
                names.append(rel)
    return sorted(names, key=lambda s: s.encode())


def pack(root, out, split_mb=0, level=6, store_only=False):
    names = gather(root)
    vsize = split_mb * 1024 * 1024
    o = Out(out, vsize)
    o.write(b'\0' * HDR.size)
    entries, chunks = [], []
    name_blob = bytearray()
    raw_total = stored_total = 0
    for i, rel in enumerate(names):
        with open(os.path.join(root, rel), 'rb') as f:
            data = f.read()
        entries.append((len(name_blob), len(data), len(chunks)))
        name_blob += rel.encode() + b'\0'
        for c in range(0, len(data), CHUNK):
            piece = data[c:c + CHUNK]
            flags = 0
            payload = piece
            if not store_only:
                z = zlib.compress(piece, level)
                if len(z) <= len(piece) - len(piece) // 16:
                    payload, flags = z, 1
            chunks.append((o.pos, len(payload), flags))
            o.write(payload)
            raw_total += len(piece)
            stored_total += len(payload)
        if (i + 1) % 100 == 0:
            print(f'  {i + 1}/{len(names)} files, {raw_total >> 20} MB -> {stored_total >> 20} MB', flush=True)
    index_off = o.pos
    idx = bytearray()
    for name_ofs, size, first in entries:
        idx += ENTRY.pack(name_ofs, size, first, 0)
    for off, sz, fl in chunks:
        idx += CHUNKREC.pack(off, sz, fl)
    idx += name_blob
    o.write(bytes(idx))
    nvol = (o.pos + vsize - 1) // vsize if vsize else 1
    header = HDR.pack(MAGIC, 1, CHUNK, len(entries), len(chunks), len(name_blob), nvol, vsize,
                      index_off, len(idx), b'\0' * 8)
    files = o.finish(header)
    print(f'{len(entries)} files, {len(chunks)} chunks: {raw_total / 2**20:.0f} MB -> {o.pos / 2**20:.0f} MB '
          f'in {len(files)} file(s): ' + ', '.join(os.path.basename(f) for f in files))
    return files


class Reader:
    def __init__(self, path):
        self.path = path
        with open(path if os.path.exists(path) else path + '.000', 'rb') as f:
            h = HDR.unpack(f.read(HDR.size))
        (magic, ver, self.chunk, ne, nc, nn, self.nvol, self.vsize, ioff, isz, _) = h
        if magic != MAGIC or ver != 1:
            raise ValueError('not an RE4PAK1 file')
        idx = self._read(ioff, isz)
        self.entries = [ENTRY.unpack_from(idx, i * 16) for i in range(ne)]
        base = ne * 16
        self.chunks = [CHUNKREC.unpack_from(idx, base + i * 16) for i in range(nc)]
        self.names = idx[base + nc * 16:]

    def _read(self, off, n):
        out = bytearray()
        while n:
            if self.vsize:
                v, vo = divmod(off, self.vsize)
                name, take = f'{self.path}.{v:03d}', min(n, self.vsize - vo)
            else:
                name, vo, take = self.path, off, n
            with open(name, 'rb') as f:
                f.seek(vo)
                out += f.read(take)
            off += take
            n -= take
        return bytes(out)

    def name(self, i):
        o = self.entries[i][0]
        return self.names[o:self.names.index(b'\0', o)].decode()

    def data(self, i):
        _, size, first, _ = self.entries[i]
        out = bytearray()
        for c in range(first, first + (size + self.chunk - 1) // self.chunk):
            off, sz, fl = self.chunks[c]
            b = self._read(off, sz)
            out += zlib.decompress(b) if fl & 1 else b
        return bytes(out)


def main():
    a = sys.argv[1:]
    if a and a[0] == '--list':
        r = Reader(a[1])
        for i in range(len(r.entries)):
            print(f'{r.entries[i][1]:>12}  {r.name(i)}')
        return
    if a and a[0] == '--verify':
        root, path = a[1], a[2]
        r = Reader(path)
        bad = 0
        for i in range(len(r.entries)):
            with open(os.path.join(root, r.name(i)), 'rb') as f:
                if f.read() != r.data(i):
                    print('MISMATCH', r.name(i))
                    bad += 1
        print(f'verify: {len(r.entries)} entries, {bad} mismatches')
        sys.exit(1 if bad else 0)
    if len(a) < 2:
        sys.exit(__doc__)
    split = int(a[a.index('--split') + 1]) if '--split' in a else 0
    level = int(a[a.index('--level') + 1]) if '--level' in a else 6
    pack(a[0], a[1], split, level, '--store-only' in a)


if __name__ == '__main__':
    main()
