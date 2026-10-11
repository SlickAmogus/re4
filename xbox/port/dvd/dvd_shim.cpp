// dvd_shim.cpp — DVD* and AR/ARQ (ARAM) over files and a RAM block (xbox/docs/04-DATA.md, 05-SYSTEM.md).
//
// Files: the converted asset tree <data>/files/... (both discs merged). Entry numbers index the table
// built from port/dvd/filenames.inc (CT-01): every name the game can request maps to its exact retail
// path, so lookups need no case folding on the console; unknown names fall back to a case-insensitive
// scan of the table. Sizes come from the files themselves (conversion changes them).
//
// Reads complete synchronously into the caller's buffer, but are *reported* from re4_dvd_poll(), which
// the port calls at the game's own polling points (DVDGetCommandBlockStatus, VIWaitForRetrace): the
// game's read queue (dvd.cpp cDvdQueue) expects BUSY first and the callback later, as on hardware.
//
// ARAM: a 16 MB block (re4_aram_base); ARQPostRequest is a memcpy reported on the next poll the same way.
#include "re4port/re4_xbox.h"
#include <string.h>
#include <stdio.h>
#ifdef RE4_XBOX
#include "io.h"
#include "fcntl.h"
#else
#include <fcntl.h>
#include <unistd.h>
#define _open open
#define _read read
#define _lseek lseek
#define _close close
#ifndef O_BINARY
#define O_BINARY 0
#endif
#endif

extern "C" {

typedef int s32;
typedef unsigned int u32;
typedef unsigned char u8;

// ---- SDK structures (include/dolphin/dvd.h, ar.h, arq.h) ------------------------------------------
struct DVDDiskID { char gameName[4]; char company[2]; u8 diskNumber; u8 gameVersion; u8 streaming; u8 streamingBufSize; u8 padding[22]; };
struct DVDCommandBlock;
typedef void (*DVDCBCallback)(s32 result, DVDCommandBlock* block);
struct DVDCommandBlock {
    DVDCommandBlock* next; DVDCommandBlock* prev; u32 command; s32 state; u32 offset; u32 length;
    void* addr; u32 currTransferSize; u32 transferredSize; DVDDiskID* id; DVDCBCallback callback; void* userData;
};
struct DVDFileInfo;
typedef void (*DVDCallback)(s32 result, DVDFileInfo* fi);
struct DVDFileInfo { DVDCommandBlock cb; u32 startAddr; u32 length; DVDCallback callback; };
static_assert(sizeof(DVDCommandBlock) == 0x30 && sizeof(DVDFileInfo) == 0x3C, "DVD struct layout");

enum { DVD_STATE_END = 0, DVD_STATE_BUSY = 1, DVD_STATE_CANCELED = 10 };
enum { DVD_RESULT_FATAL = -1, DVD_RESULT_CANCELED = -3 };

// ---- file table --------------------------------------------------------------------------------
struct Entry { const char* path; };
struct Alias { const char* req; int entry; };

#define RE4_FILE(req, path, disc, size, kind) { req, path },
static const struct { const char* req; const char* path; } s_names[] = {
#include "filenames.inc"
};
#undef RE4_FILE

enum { MAX_ENTRIES = 1400 };
static const char* s_entry_path[MAX_ENTRIES];   // unique retail paths; index = entry number
static int s_nentries;
static int s_name_entry[sizeof s_names / sizeof s_names[0]];
static char s_root[256];
static int s_init;

extern const char* re4_data_dir;     // host: --data; Xbox: set by xbox_main (D:\data)

static int ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

static void table_init(void)
{
    if (s_init) return;
    s_init = 1;
    snprintf(s_root, sizeof s_root, "%s", re4_data_dir ? re4_data_dir : "data");
    for (unsigned i = 0; i < sizeof s_names / sizeof s_names[0]; i++) {
        int e = -1;
        for (int j = 0; j < s_nentries; j++) {
            if (strcmp(s_entry_path[j], s_names[i].path) == 0) { e = j; break; }
        }
        if (e < 0 && s_nentries < MAX_ENTRIES) {
            e = s_nentries++;
            s_entry_path[e] = s_names[i].path;
        }
        s_name_entry[i] = e;
    }
    re4_log("dvd: %d entries, root %s\n", s_nentries, s_root);
}

static int s_disc_index;   // 0 = disc 1, 1 = disc 2 (DVDDiskID.diskNumber of the last disc change)

static void make_path(const char* tree, int entry, char* out, unsigned n)
{
#ifdef RE4_XBOX
    snprintf(out, n, "%s\\%s\\%s", s_root, tree, s_entry_path[entry]);
    for (char* p = out; *p; p++) if (*p == '/') *p = '\\';
#else
    snprintf(out, n, "%s/%s/%s", s_root, tree, s_entry_path[entry]);
#endif
}

// Files present on both discs with different contents (Sscrn.rel, st2_4.rel, ...) are converted twice:
// files/<path> holds the disc-1 copy, files_disc2/<path> the disc-2 copy (re4conv). While the game is on
// disc 2, prefer the latter.
static void full_path(int entry, char* out, unsigned n)
{
    if (s_disc_index == 1) {
        make_path("files_disc2", entry, out, n);
        int fd = _open(out, O_RDONLY | O_BINARY);
        if (fd >= 0) {
            _close(fd);
            return;
        }
    }
    make_path("files", entry, out, n);
}

s32 DVDConvertPathToEntrynum(const char* path)
{
    table_init();
    while (*path == '/') path++;
    for (unsigned i = 0; i < sizeof s_names / sizeof s_names[0]; i++) {
        if (strcmp(s_names[i].req, path) == 0) return s_name_entry[i];
    }
    for (int j = 0; j < s_nentries; j++) {
        if (ieq(s_entry_path[j], path)) return j;
    }
    return -1;
}

static int file_size(int entry)
{
    char p[512];
    full_path(entry, p, sizeof p);
    int fd = _open(p, O_RDONLY | O_BINARY);
    if (fd < 0) return -1;
    int size = (int) _lseek(fd, 0, SEEK_END);
    _close(fd);
    return size;
}

int DVDFastOpen(s32 entry, DVDFileInfo* fi)
{
    table_init();
    if (entry < 0 || entry >= s_nentries) return 0;
    int size = file_size(entry);
    if (size < 0) {
        char p[512];
        full_path(entry, p, sizeof p);
        re4_log("dvd: missing file %s\n", p);
        return 0;
    }
    memset(fi, 0, sizeof *fi);
    fi->startAddr = (u32) entry;     // the game never interprets startAddr; keep the entry number there
    fi->length = (u32) size;
    fi->cb.state = DVD_STATE_END;
    return 1;
}

int DVDOpen(const char* name, DVDFileInfo* fi) { return DVDFastOpen(DVDConvertPathToEntrynum(name), fi); }
int DVDClose(DVDFileInfo*) { return 1; }

// pending completions, reported by re4_dvd_poll()
enum { MAX_PENDING = 64 };
struct Pending { DVDFileInfo* fi; DVDCommandBlock* cb; s32 result; int is_cb; };
static Pending s_pending[MAX_PENDING];
static int s_npending;

static void queue_done(DVDFileInfo* fi, DVDCommandBlock* cb, s32 result, int is_cb)
{
    if (s_npending >= MAX_PENDING) {
        re4_log("dvd: completion queue full\n");
        return;
    }
    s_pending[s_npending].fi = fi;
    s_pending[s_npending].cb = cb;
    s_pending[s_npending].result = result;
    s_pending[s_npending].is_cb = is_cb;
    s_npending++;
}

int DVDReadAsyncPrio(DVDFileInfo* fi, void* addr, s32 length, s32 offset, DVDCallback cb, s32 prio)
{
    (void) prio;
    char p[512];
    full_path((int) fi->startAddr, p, sizeof p);
    fi->callback = cb;
    fi->cb.state = DVD_STATE_BUSY;
    fi->cb.addr = addr;
    fi->cb.offset = (u32) offset;
    fi->cb.length = (u32) length;
    fi->cb.transferredSize = 0;
    s32 got = DVD_RESULT_FATAL;
    int fd = _open(p, O_RDONLY | O_BINARY);
    if (fd >= 0) {
        _lseek(fd, offset, SEEK_SET);
        // the game reads 32-byte rounded lengths: the tail past EOF stays as it was (zero on hardware)
        int n = _read(fd, addr, (unsigned) length);
        _close(fd);
        if (n >= 0) {
            if (n < length) memset((u8*) addr + n, 0, (unsigned) (length - n));
            got = length;
        }
    }
    if (got < 0) re4_log("dvd: read failed %s ofs 0x%x len 0x%x\n", p, offset, length);
    fi->cb.transferredSize = got > 0 ? (u32) got : 0;
    fi->cb.currTransferSize = fi->cb.transferredSize;
    queue_done(fi, &fi->cb, got, 0);
    return 1;
}

int DVDReadPrio(DVDFileInfo* fi, void* addr, s32 length, s32 offset, s32 prio)
{
    DVDReadAsyncPrio(fi, addr, length, offset, 0, prio);
    extern void re4_dvd_poll(void);
    re4_dvd_poll();
    return fi->cb.transferredSize;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* cb)
{
    extern void re4_dvd_poll(void);
    re4_dvd_poll();
    return cb->state;
}

s32 DVDGetDriveStatus(void) { return DVD_STATE_END; }
s32 DVDGetTransferredSize(DVDFileInfo* fi) { return (s32) fi->cb.transferredSize; }

int DVDCancelAsync(DVDCommandBlock* cb, DVDCBCallback callback)
{
    for (int i = 0; i < s_npending; i++) {
        if (s_pending[i].cb == cb) {
            s_pending[i] = s_pending[--s_npending];
            break;
        }
    }
    cb->state = DVD_STATE_CANCELED;
    if (callback) callback(0, cb);
    return 1;
}

void DVDCancelAll(void) { s_npending = 0; }

// both discs are merged: the current disc is whichever the game asked for last
static DVDDiskID s_disk = { {'G', '4', 'B', 'E'}, {'0', '8'}, 0, 0, 0, 0, {0} };
DVDDiskID* DVDGetCurrentDiskID(void) { return &s_disk; }
int DVDCompareDiskID(const DVDDiskID*, const DVDDiskID*) { return 1; }

DVDDiskID* DVDGenerateDiskID(DVDDiskID* id, const char* game, const char* company, u8 disk, u8 version)
{
    memset(id, 0, sizeof *id);
    if (game) memcpy(id->gameName, game, 4);
    if (company) memcpy(id->company, company, 2);
    id->diskNumber = disk;
    id->gameVersion = version;
    return id;
}

int DVDChangeDiskAsync(DVDCommandBlock* cb, DVDDiskID* id, DVDCBCallback callback)
{
    if (id) {
        s_disk.diskNumber = id->diskNumber;
        s_disc_index = id->diskNumber ? 1 : 0;
        re4_log("dvd: switched to disc %d\n", s_disc_index + 1);
    }
    cb->state = DVD_STATE_BUSY;
    cb->callback = callback;
    queue_done(0, cb, 0, 1);
    return 1;
}

void* DVDGetFSTLocation(void) { return GC_ADDR(0x817F4000); }   // dvd.cpp only prints it and its size

// CRI ADX's DVD file system hook (movies, CT-07); nothing to set up for file-backed reads.
void ADXGC_SetupDvdFs(void*) { }

// ---- ARAM ---------------------------------------------------------------------------------------
struct ARQRequest {                 // include/dolphin/arq.h
    ARQRequest* next; u32 owner; u32 type; u32 priority; u32 source; u32 dest; u32 length;
    void (*callback)(u32 pointerToARQRequest);
};
enum { ARQ_TYPE_MRAM_TO_ARAM = 0, ARQ_TYPE_ARAM_TO_MRAM = 1 };

static u32 s_aram_alloc;
static const u32 ARAM_BASE = 0x4000;   // the SDK reserves the first 16 KB

u32 ARInit(u32*, u32) { s_aram_alloc = ARAM_BASE; return ARAM_BASE; }
u32 ARGetBaseAddress(void) { return ARAM_BASE; }
u32 ARGetSize(void) { return RE4_ARAM_SIZE; }
u32 ARAlloc(u32 length)
{
    u32 a = s_aram_alloc;
    s_aram_alloc += (length + 31) & ~31u;
    if (s_aram_alloc > RE4_ARAM_SIZE) re4_log("ARAlloc: out of ARAM (0x%x)\n", s_aram_alloc);
    return a;
}
void ARQInit(void) { }
void ARQFlushQueue(void) { }

static unsigned char* aram_ptr(u32 a) { return re4_aram_base + (a & (RE4_ARAM_SIZE - 1)); }
// MRAM side of an ARQ request: a pointer the game computed (heap or relocated data) — on the GameCube a
// physical or cached address; the game passes cached (0x8xxxxxxx) addresses, which are real pointers here.
static unsigned char* mram_ptr(u32 a) { return (unsigned char*) a; }

enum { MAX_ARQ = 64 };
static ARQRequest* s_arq_done[MAX_ARQ];
static int s_narq;

void ARQPostRequest(ARQRequest* r, u32 owner, u32 type, u32 prio, u32 source, u32 dest, u32 length,
                    void (*callback)(u32))
{
    r->owner = owner; r->type = type; r->priority = prio; r->source = source; r->dest = dest;
    r->length = length; r->callback = callback;
    if (type == ARQ_TYPE_MRAM_TO_ARAM) memmove(aram_ptr(dest), mram_ptr(source), length);
    else memmove(mram_ptr(dest), aram_ptr(source), length);
    if (s_narq < MAX_ARQ) s_arq_done[s_narq++] = r;
}

// ---- completion pump --------------------------------------------------------------------------------
void re4_dvd_poll(void)
{
    static int in_poll;
    if (in_poll) return;            // callbacks may issue new reads; they complete on the next poll
    in_poll = 1;
    int n = s_npending;
    for (int i = 0; i < n && i < s_npending; i++) {
        Pending p = s_pending[i];
        if (p.is_cb) {
            p.cb->state = DVD_STATE_END;
            if (p.cb->callback) p.cb->callback(p.result, p.cb);
        } else {
            p.fi->cb.state = p.result < 0 ? DVD_RESULT_FATAL : DVD_STATE_END;
            if (p.fi->callback) p.fi->callback(p.result, p.fi);
        }
    }
    memmove(s_pending, s_pending + n, (unsigned) (s_npending - n) * sizeof s_pending[0]);
    s_npending -= n;
    int m = s_narq;
    for (int i = 0; i < m; i++) {
        ARQRequest* r = s_arq_done[i];
        if (r->callback) r->callback((u32) r);
    }
    memmove(s_arq_done, s_arq_done + m, (unsigned) (s_narq - m) * sizeof s_arq_done[0]);
    s_narq -= m;
    in_poll = 0;
}

} // extern "C"
