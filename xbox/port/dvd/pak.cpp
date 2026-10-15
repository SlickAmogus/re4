// pak.cpp — reader for the RE4PAK1 data pack written by xbox/tools/pack_data.py (format documented there).
//
// The whole converted data tree (files/, files_disc2/) can be shipped as one pack, or a few parts
// (re4.pak.000, .001, ...), instead of ~1100 loose files: the Xbox FTP servers handle many files badly.
// dvd_shim.cpp asks this reader first and falls back to loose files when no pack is present.
//
// Entries are stored in 64 KB chunks, each raw or zlib-deflated, so any byte range is readable without
// unpacking the whole file (the game reads archives up to 11 MB and streams movies by offset). One
// decompressed chunk is cached, which covers the sequential reads the game does. Offsets are 64-bit:
// on Xbox reads go through Win32 CreateFile/SetFilePointerEx (nxdk winapi), not the 32-bit _lseek.
#include "re4port/re4_xbox.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <zlib.h>
#ifdef RE4_XBOX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

extern "C" {

typedef unsigned int u32;
typedef unsigned long long u64;

struct PakHeader {
    char magic[8];
    u32 version, chunk_size, entry_count, chunk_count, names_size, volume_count;
    u64 volume_size, index_offset, index_size;
    unsigned char pad[8];
};
static_assert(sizeof(PakHeader) == 64, "pak header");
struct PakEntry { u32 name_ofs, size, first_chunk, pad; };
struct PakChunk { u64 offset; u32 stored, flags; };
static_assert(sizeof(PakEntry) == 16 && sizeof(PakChunk) == 16, "pak index records");

enum { MAX_VOLUMES = 32 };

static PakHeader s_hdr;
static PakEntry* s_entries;
static PakChunk* s_chunks;
static const char* s_names;
static int s_open;
static char s_base[260];
static int s_split;
#ifdef RE4_XBOX
static HANDLE s_vol[MAX_VOLUMES];
#else
static int s_vol[MAX_VOLUMES];
#endif
static unsigned char* s_cbuf;        // decompressed chunk cache
static unsigned char* s_zbuf;        // stored chunk staging
static int s_cached_chunk = -1;

static void vol_name(int v, char* out, unsigned n)
{
    if (s_split) snprintf(out, n, "%s.%03d", s_base, v);
    else snprintf(out, n, "%s", s_base);
}

static int vol_open(int v)
{
    char name[280];
    vol_name(v, name, sizeof name);
#ifdef RE4_XBOX
    if (s_vol[v] && s_vol[v] != INVALID_HANDLE_VALUE) return 1;
    s_vol[v] = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    return s_vol[v] != INVALID_HANDLE_VALUE;
#else
    if (s_vol[v] > 0) return 1;
    s_vol[v] = open(name, O_RDONLY);
    return s_vol[v] > 0;
#endif
}

// read n bytes at an offset of one volume file
static int vol_pread(int v, u64 off, void* dst, u32 n)
{
    if (!vol_open(v)) return 0;
#ifdef RE4_XBOX
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG) off;
    if (!SetFilePointerEx(s_vol[v], li, 0, FILE_BEGIN)) return 0;
    DWORD got = 0;
    if (!ReadFile(s_vol[v], dst, n, &got, 0)) return 0;
    return got == n;
#else
    return pread(s_vol[v], dst, n, (off_t) off) == (ssize_t) n;
#endif
}

// read n bytes at a logical stream offset (may span volumes)
static int stream_read(u64 off, void* dst, u32 n)
{
    unsigned char* d = (unsigned char*) dst;
    while (n) {
        int v = 0;
        u64 vo = off;
        u32 take = n;
        if (s_hdr.volume_size) {
            v = (int) (off / s_hdr.volume_size);
            vo = off % s_hdr.volume_size;
            u64 room = s_hdr.volume_size - vo;
            if (take > room) take = (u32) room;
        }
        if (v >= MAX_VOLUMES || !vol_pread(v, vo, d, take)) return 0;
        off += take;
        d += take;
        n -= take;
    }
    return 1;
}

// Opens <base> or <base>.000 (split). Returns 1 when a pack is available.
int re4_pak_open(const char* base)
{
    if (s_open) return 1;
    snprintf(s_base, sizeof s_base, "%s", base);
    char name[280];
    s_split = 0;
    vol_name(0, name, sizeof name);
    if (!vol_open(0)) {
        s_split = 1;
        if (!vol_open(0)) return 0;
    }
    if (!vol_pread(0, 0, &s_hdr, sizeof s_hdr) || memcmp(s_hdr.magic, "RE4PAK1", 8) != 0 || s_hdr.version != 1) {
        re4_log("pak: %s is not an RE4PAK1 file\n", base);
        return 0;
    }
    if (s_hdr.volume_count > MAX_VOLUMES || s_hdr.index_size > 0x4000000u) {
        re4_log("pak: bad header (volumes %u, index %u)\n", s_hdr.volume_count, (u32) s_hdr.index_size);
        return 0;
    }
    unsigned char* idx = (unsigned char*) malloc((size_t) s_hdr.index_size);
    s_cbuf = (unsigned char*) malloc(s_hdr.chunk_size);
    s_zbuf = (unsigned char*) malloc(s_hdr.chunk_size + s_hdr.chunk_size / 8 + 64);
    if (!idx || !s_cbuf || !s_zbuf || !stream_read(s_hdr.index_offset, idx, (u32) s_hdr.index_size)) {
        re4_log("pak: cannot read the index (%u bytes)\n", (u32) s_hdr.index_size);
        return 0;
    }
    s_entries = (PakEntry*) idx;
    s_chunks = (PakChunk*) (idx + s_hdr.entry_count * sizeof(PakEntry));
    s_names = (const char*) (idx + s_hdr.entry_count * sizeof(PakEntry) + s_hdr.chunk_count * sizeof(PakChunk));
    s_open = 1;
    re4_log("pak: %s%s, %u files, %u chunks, %u volume(s)\n", base, s_split ? ".000.." : "", s_hdr.entry_count,
            s_hdr.chunk_count, s_hdr.volume_count);
    return 1;
}

int re4_pak_is_open(void) { return s_open; }

static int name_cmp(const char* a, const char* b) { return strcmp(a, b); }

static int lower_eq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

// Entry index of "files/em/em10.drs" style names (exact first, binary search; then case-insensitive).
int re4_pak_find(const char* name)
{
    if (!s_open) return -1;
    int lo = 0, hi = (int) s_hdr.entry_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = name_cmp(s_names + s_entries[mid].name_ofs, name);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    for (u32 i = 0; i < s_hdr.entry_count; i++) {
        if (lower_eq(s_names + s_entries[i].name_ofs, name)) return (int) i;
    }
    return -1;
}

int re4_pak_size(int e) { return (e >= 0 && (u32) e < s_hdr.entry_count) ? (int) s_entries[e].size : -1; }

// nxdk builds zlib with Z_SOLO: no uncompress() and no default allocator, so inflate directly.
static voidpf z_alloc(voidpf, uInt items, uInt size) { return malloc((size_t) items * size); }
static void z_free(voidpf, voidpf p) { free(p); }

static int inflate_chunk(unsigned char* dst, u32 dst_len, const unsigned char* src, u32 src_len)
{
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    zs.zalloc = z_alloc;
    zs.zfree = z_free;
    if (inflateInit(&zs) != Z_OK) return 0;
    zs.next_in = (Bytef*) src;
    zs.avail_in = src_len;
    zs.next_out = dst;
    zs.avail_out = dst_len;
    int r = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    return r == Z_STREAM_END;
}

static const unsigned char* load_chunk(u32 c)
{
    if ((int) c == s_cached_chunk) return s_cbuf;
    const PakChunk* ch = &s_chunks[c];
    if (ch->flags & 1) {
        if (ch->stored > s_hdr.chunk_size + s_hdr.chunk_size / 8 + 64) return 0;
        if (!stream_read(ch->offset, s_zbuf, ch->stored)) return 0;
        if (!inflate_chunk(s_cbuf, s_hdr.chunk_size, s_zbuf, ch->stored)) {
            re4_log("pak: chunk %u fails to inflate\n", c);
            return 0;
        }
    } else {
        if (ch->stored > s_hdr.chunk_size || !stream_read(ch->offset, s_cbuf, ch->stored)) return 0;
    }
    s_cached_chunk = (int) c;
    return s_cbuf;
}

// Copies len bytes from offset ofs of entry e into dst; returns the bytes copied (short at end of file).
int re4_pak_read(int e, u32 ofs, void* dst, u32 len)
{
    if (re4_pak_size(e) < 0) return -1;
    const PakEntry* en = &s_entries[e];
    if (ofs >= en->size) return 0;
    if (len > en->size - ofs) len = en->size - ofs;
    unsigned char* d = (unsigned char*) dst;
    u32 cs = s_hdr.chunk_size, done = 0;
    while (done < len) {
        u32 pos = ofs + done;
        u32 c = en->first_chunk + pos / cs;
        u32 in = pos % cs;
        u32 avail = cs - in;
        u32 chunk_len = (pos / cs == (en->size - 1) / cs) ? en->size - (pos / cs) * cs : cs;
        if (avail > chunk_len - in) avail = chunk_len - in;
        u32 take = len - done < avail ? len - done : avail;
        // a whole raw chunk going straight to the destination needs no cache copy
        if (in == 0 && take == chunk_len && !(s_chunks[c].flags & 1)) {
            if (!stream_read(s_chunks[c].offset, d + done, take)) return (int) done;
        } else {
            const unsigned char* src = load_chunk(c);
            if (!src) return (int) done;
            memcpy(d + done, src + in, take);
        }
        done += take;
    }
    return (int) done;
}

} // extern "C"
