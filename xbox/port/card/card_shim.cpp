// card_shim.cpp — the Dolphin CARD API (17 functions, include/card.h) over host/Xbox files (CT-13,
// xbox/docs/04-DATA.md section 6, 05-SYSTEM.md section 6). src/game/card.cpp runs unchanged on top.
//
// Model: slot A (chan 0) is a 59-block card (4 Mbit, sector 0x2000, 127 directory entries), slot B
// (chan 1) is always empty (CARD_RESULT_NOCARD). One directory per slot:
//   Xbox: <xbox_get_writedir()>saves\slotA\      host: <re4_data_dir>/../saves/slotA/
// holding
//   card.dir          the card directory: 127 entries {name[32], length, used} (fileNo = entry index)
//   <name>.dat        the file's bytes (length = blocks * 0x2000, zero filled at create)
//   <name>.stat       its CardStat (banner/icon formats, iconAddr, commentAddr, time)
// A missing card.dir is a freshly formatted card; a corrupt one mounts as CARD_RESULT_BROKEN so the game
// offers to format (cCard::verifyCheck sets 0x10).
//
// Async calls (Create/Delete/Format/Mount/Check/Read/Write/SetStatus) run synchronously at the call but
// are *reported* from re4_card_poll(): until then CARDGetResultCode returns CARD_RESULT_BUSY, as on
// hardware. VIWaitForRetrace (port/os/vi_pad.cpp) must call re4_card_poll() once per frame. As a safety
// net against a missing hook, the fourth consecutive CARDGetResultCode of a pending op completes it.
//
// Sync calls on a channel with a pending async op return CARD_RESULT_BUSY (SDK behaviour); calls on an
// unmounted slot return CARD_RESULT_NOCARD. CARDGetStatus/CARDSetStatusAsync follow the SDK's field rules
// (only bannerFormat/iconAddr/iconFormat/iconSpeed/commentAddr/time are stored; fileName, length,
// gameName, company and the offset* fields are derived on read). A file that never had its status set
// reports commentAddr = iconAddr = 0xFFFFFFFF, which the game treats as corrupt (card.cpp:2608).
#include "re4port/re4_xbox.h"
#include <string.h>
#include <stdio.h>
#ifdef RE4_XBOX
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
// xbox_compat/io.h and fcntl.h declare these without extern "C" (posix_io.c is C): include fcntl.h (no
// includes of its own) inside extern "C" and declare the rest here with C linkage.
extern "C" {
#include "fcntl.h"
int _open(const char* path, int flags, ...);
int _read(int fd, void* buf, unsigned int count);
int _write(int fd, const void* buf, unsigned int count);
int _close(int fd);
long _lseek(int fd, long offset, int whence);
}
#define CARD_SEP "\\"
#define CARD_MODE 0
#elif defined(_WIN32)
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#define CARD_SEP "/"
#define CARD_MODE (_S_IREAD | _S_IWRITE)
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#define _open open
#define _read read
#define _write write
#define _lseek lseek
#define _close close
#define CARD_SEP "/"
#define CARD_MODE 0666
#ifndef O_BINARY
#define O_BINARY 0
#endif
#endif

extern "C" {

typedef int s32;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;

// ---- the game's declarations (include/card.h), repeated so the port does not pull in game headers ----
struct CardFileInfo {
    s32 chan;    // 0x00
    s32 fileNo;  // 0x04
    s32 offset;  // 0x08
    s32 length;  // 0x0C
    u16 iBlock;  // 0x10
    u16 pad_12;
};
struct CardStat {
    char fileName[32];    // 0x00
    u32 length;           // 0x20
    u32 time;             // 0x24
    u8 gameName[4];       // 0x28
    u8 company[2];        // 0x2C
    u8 bannerFormat;      // 0x2E
    u8 pad_2F;
    u32 iconAddr;         // 0x30
    u16 iconFormat;       // 0x34
    u16 iconSpeed;        // 0x36
    u32 commentAddr;      // 0x38
    u32 offsetBanner;     // 0x3C
    u32 offsetBannerTlut; // 0x40
    u32 offsetIcon[8];    // 0x44
    u32 offsetIconTlut;   // 0x64
    u32 offsetData;       // 0x68
};
static_assert(sizeof(CardFileInfo) == 0x14 && sizeof(CardStat) == 0x6C, "CARD struct layout");
typedef void (*CardCallback)(s32 chan, s32 result);

// include/dolphin/card.h
enum {
    CARD_RESULT_READY = 0, CARD_RESULT_BUSY = -1, CARD_RESULT_WRONGDEVICE = -2, CARD_RESULT_NOCARD = -3,
    CARD_RESULT_NOFILE = -4, CARD_RESULT_IOERROR = -5, CARD_RESULT_BROKEN = -6, CARD_RESULT_EXIST = -7,
    CARD_RESULT_NOENT = -8, CARD_RESULT_INSSPACE = -9, CARD_RESULT_NOPERM = -10, CARD_RESULT_LIMIT = -11,
    CARD_RESULT_NAMETOOLONG = -12, CARD_RESULT_ENCODING = -13, CARD_RESULT_CANCELED = -14,
    CARD_RESULT_FATAL_ERROR = -128
};

void CARDInit();
s32 CARDGetResultCode(s32 chan);
s32 CARDCheckAsync(s32 chan, CardCallback callback);
s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed);
s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CardFileInfo* fileInfo, CardCallback callback);
s32 CARDDeleteAsync(s32 chan, const char* fileName, CardCallback callback);
s32 CARDFormatAsync(s32 chan, CardCallback callback);
s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize);
s32 CARDMountAsync(s32 chan, void* workArea, CardCallback detachCallback, CardCallback attachCallback);
s32 CARDUnmount(s32 chan);
s32 CARDGetSerialNo(s32 chan, u64* serialNo);
s32 CARDOpen(s32 chan, const char* fileName, CardFileInfo* fileInfo);
s32 CARDClose(CardFileInfo* fileInfo);
s32 CARDReadAsync(CardFileInfo* fileInfo, void* buf, s32 length, s32 offset, CardCallback callback);
s32 CARDWriteAsync(CardFileInfo* fileInfo, void* buf, s32 length, s32 offset, CardCallback callback);
s32 CARDGetStatus(s32 chan, s32 fileNo, CardStat* stat);
s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CardStat* stat, CardCallback callback);

// port-only entry points
void re4_card_poll(void);                 // VIWaitForRetrace calls this once per frame
void re4_card_set_root(const char* dir);  // override the slot-A directory (tests); must end in a separator

extern const char* re4_data_dir;
#ifdef RE4_XBOX
const char* xbox_get_writedir(void);
#endif

// ---- card geometry -------------------------------------------------------------------------------------
enum {
    CARD_SECTOR = 0x2000,
    CARD_READ_SIZE = 0x200,
    CARD_USER_BLOCKS = 59,       // a "59 block" card (DOL-008, 4 Mbit)
    CARD_MEM_MBIT = 4,
    CARD_MAX_FILE = 127,
    CARD_NAME_MAX = 32,
    CARD_SLOTS = 2,
    CARD_POLL_FALLBACK = 4,      // CARDGetResultCode calls before a pending op completes without a poll
};
static const u64 s_serial[CARD_SLOTS] = { 0x5245345842303141ull /* "RE4XB01A" */, 0x5245345842303142ull };

struct DirEntry { char name[CARD_NAME_MAX]; u32 length; u32 used; };
struct DirHeader { u32 magic; u32 version; u32 count; u32 pad; };
static const u32 DIR_MAGIC = 0x44433452;  // "R4CD"
static_assert(sizeof(DirEntry) == 40 && sizeof(DirHeader) == 16, "card.dir layout");

struct Slot {
    int mounted;
    int broken;                  // card.dir unreadable: mounted as BROKEN until formatted
    DirEntry dir[CARD_MAX_FILE];
    s32 result;                  // what CARDGetResultCode returns
    int pending;                 // an async op finished its work but has not been reported yet
    s32 pendingResult;
    CardCallback pendingCb;
    int busyPolls;
};
static Slot s_slot[CARD_SLOTS];
static char s_root[256];
static int s_init;

// ---- paths and host file helpers -----------------------------------------------------------------------
static void make_dir(const char* path)
{
#ifdef RE4_XBOX
    CreateDirectoryA(path, NULL);
#elif defined(_WIN32)
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

static void root_init(void)
{
    if (s_root[0]) return;
    char base[200];
#ifdef RE4_XBOX
    snprintf(base, sizeof base, "%ssaves", xbox_get_writedir());
#else
    snprintf(base, sizeof base, "%s/../saves", re4_data_dir ? re4_data_dir : "data");
#endif
    make_dir(base);
    snprintf(s_root, sizeof s_root, "%s" CARD_SEP "slotA" CARD_SEP, base);
    // make_dir wants the path without the trailing separator
    char d[256];
    snprintf(d, sizeof d, "%s", s_root);
    d[strlen(d) - 1] = 0;
    make_dir(d);
}

void re4_card_set_root(const char* dir)
{
    snprintf(s_root, sizeof s_root, "%s", dir ? dir : "");
}

// CARD names are up to 32 bytes of anything; host names keep [A-Za-z0-9_-] and escape the rest as %XX
// (FATX allows 42 characters, so the game's "bh4_data00" / "bh4_system" stay readable).
static void host_path(const char* cardName, const char* ext, char* out, unsigned n)
{
    char nm[CARD_NAME_MAX * 3 + 1];
    unsigned k = 0;
    for (int i = 0; i < CARD_NAME_MAX && cardName[i]; i++) {
        unsigned char c = (unsigned char) cardName[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            nm[k++] = (char) c;
        } else {
            static const char hx[] = "0123456789ABCDEF";
            nm[k++] = '%';
            nm[k++] = hx[c >> 4];
            nm[k++] = hx[c & 15];
        }
    }
    nm[k] = 0;
    snprintf(out, n, "%s%s%s", s_root, nm, ext);
}

static void dir_path(char* out, unsigned n) { snprintf(out, n, "%scard.dir", s_root); }

static int file_write_all(const char* path, const void* buf, u32 len, u32 zeroFill)
{
    int fd = _open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, CARD_MODE);
    if (fd < 0) return -1;
    int ok = 1;
    if (buf && len) ok = _write(fd, buf, len) == (int) len;
    if (ok && zeroFill) {
        static u8 zero[CARD_SECTOR];
        for (u32 done = 0; ok && done < zeroFill; done += CARD_SECTOR) ok = _write(fd, zero, CARD_SECTOR) == CARD_SECTOR;
    }
    _close(fd);
    return ok ? 0 : -1;
}

static int file_read_at(const char* path, void* buf, u32 len, u32 ofs)
{
    int fd = _open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return -1;
    int ok = _lseek(fd, (long) ofs, SEEK_SET) == (long) ofs && _read(fd, buf, len) == (int) len;
    _close(fd);
    return ok ? 0 : -1;
}

static int file_write_at(const char* path, const void* buf, u32 len, u32 ofs)
{
    int fd = _open(path, O_RDWR | O_BINARY);
    if (fd < 0) return -1;
    int ok = _lseek(fd, (long) ofs, SEEK_SET) == (long) ofs && _write(fd, buf, len) == (int) len;
    _close(fd);
    return ok ? 0 : -1;
}

static void file_delete(const char* path)
{
#ifdef RE4_XBOX
    DeleteFileA(path);
#else
    remove(path);
#endif
}

// seconds since 2000-01-01 (the CARD epoch)
static u32 card_time(void)
{
#ifdef RE4_XBOX
    LARGE_INTEGER t;
    KeQuerySystemTime(&t);  // 100 ns since 1601-01-01
    unsigned long long s = (unsigned long long) t.QuadPart / 10000000ull;
    const unsigned long long epoch2000 = 12591158400ull;  // 1601 -> 2000
    return s > epoch2000 ? (u32) (s - epoch2000) : 0;
#else
    long long s = (long long) time(NULL) - 946684800ll;
    return s > 0 ? (u32) s : 0;
#endif
}

// ---- directory ------------------------------------------------------------------------------------------
static int dir_save(Slot* sl)
{
    struct { DirHeader h; DirEntry e[CARD_MAX_FILE]; } img;
    memset(&img, 0, sizeof img);
    img.h.magic = DIR_MAGIC;
    img.h.version = 1;
    img.h.count = CARD_MAX_FILE;
    memcpy(img.e, sl->dir, sizeof img.e);
    char p[300];
    dir_path(p, sizeof p);
    return file_write_all(p, &img, sizeof img, 0);
}

// 0 ok (also: no card.dir = blank card), -1 corrupt
static int dir_load(Slot* sl)
{
    memset(sl->dir, 0, sizeof sl->dir);
    char p[300];
    dir_path(p, sizeof p);
    int fd = _open(p, O_RDONLY | O_BINARY);
    if (fd < 0) return dir_save(sl) == 0 ? 0 : -1;
    struct { DirHeader h; DirEntry e[CARD_MAX_FILE]; } img;
    int n = _read(fd, &img, sizeof img);
    _close(fd);
    if (n != (int) sizeof img || img.h.magic != DIR_MAGIC || img.h.version != 1 || img.h.count != CARD_MAX_FILE) return -1;
    u32 blocks = 0;
    for (int i = 0; i < CARD_MAX_FILE; i++) {
        DirEntry* e = &img.e[i];
        if (!e->used) continue;
        if (e->length == 0 || e->length % CARD_SECTOR) return -1;
        blocks += e->length / CARD_SECTOR;
    }
    if (blocks > CARD_USER_BLOCKS) return -1;
    memcpy(sl->dir, img.e, sizeof sl->dir);
    return 0;
}

static int name_eq(const char* a, const char* b)
{
    return strncmp(a, b, CARD_NAME_MAX) == 0;
}

static int dir_find(Slot* sl, const char* name)
{
    for (int i = 0; i < CARD_MAX_FILE; i++) {
        if (sl->dir[i].used && name_eq(sl->dir[i].name, name)) return i;
    }
    return -1;
}

static u32 used_blocks(Slot* sl)
{
    u32 b = 0;
    for (int i = 0; i < CARD_MAX_FILE; i++) if (sl->dir[i].used) b += sl->dir[i].length / CARD_SECTOR;
    return b;
}

static void stat_default(CardStat* st)
{
    memset(st, 0, sizeof *st);
    st->iconAddr = 0xFFFFFFFF;
    st->commentAddr = 0xFFFFFFFF;
}

static int stat_load(Slot* sl, int fileNo, CardStat* st)
{
    char p[300];
    host_path(sl->dir[fileNo].name, ".stat", p, sizeof p);
    if (file_read_at(p, st, sizeof *st, 0) != 0) {
        stat_default(st);
        return -1;
    }
    return 0;
}

// ---- channel bookkeeping ---------------------------------------------------------------------------------
static void card_init_once(void)
{
    if (s_init) return;
    s_init = 1;
    memset(s_slot, 0, sizeof s_slot);
    s_slot[1].result = CARD_RESULT_NOCARD;
    root_init();
}

// Validates `chan` for any call: FATAL for out of range, NOCARD for slot B, BUSY while an op is pending.
static s32 chan_check(s32 chan)
{
    card_init_once();
    if (chan < 0 || chan >= CARD_SLOTS) return CARD_RESULT_FATAL_ERROR;
    if (chan == 1) return CARD_RESULT_NOCARD;
    if (s_slot[chan].pending) return CARD_RESULT_BUSY;
    return CARD_RESULT_READY;
}

static s32 mounted_check(s32 chan)
{
    s32 r = chan_check(chan);
    if (r != CARD_RESULT_READY) return r;
    if (!s_slot[chan].mounted) return CARD_RESULT_NOCARD;
    if (s_slot[chan].broken) return CARD_RESULT_BROKEN;
    return CARD_RESULT_READY;
}

// Async start: on immediate failure the call returns the error and the result code holds it (no callback,
// as the SDK does); otherwise the op's outcome is reported on the next poll.
static s32 async_finish(s32 chan, s32 result, CardCallback cb)
{
    Slot* sl = &s_slot[chan];
    sl->pending = 1;
    sl->pendingResult = result;
    sl->pendingCb = cb;
    sl->busyPolls = 0;
    sl->result = CARD_RESULT_BUSY;
    return CARD_RESULT_READY;
}

static s32 async_fail(s32 chan, s32 result)
{
    if (chan >= 0 && chan < CARD_SLOTS && !s_slot[chan].pending) s_slot[chan].result = result;
    return result;
}

static void complete(Slot* sl, s32 chan)
{
    CardCallback cb = sl->pendingCb;
    s32 r = sl->pendingResult;
    sl->pending = 0;
    sl->pendingCb = 0;
    sl->result = r;
    if (cb) cb(chan, r);
}

void re4_card_poll(void)
{
    if (!s_init) return;
    for (s32 c = 0; c < CARD_SLOTS; c++) {
        if (s_slot[c].pending) complete(&s_slot[c], c);
    }
}

// ---- the API -------------------------------------------------------------------------------------------------
void CARDInit()
{
    card_init_once();
}

s32 CARDGetResultCode(s32 chan)
{
    card_init_once();
    if (chan < 0 || chan >= CARD_SLOTS) return CARD_RESULT_FATAL_ERROR;
    Slot* sl = &s_slot[chan];
    if (sl->pending && ++sl->busyPolls >= CARD_POLL_FALLBACK) {
        re4_log_once("card_poll", "card: op completed from CARDGetResultCode; VIWaitForRetrace should call re4_card_poll\n");
        complete(sl, chan);
    }
    return sl->result;
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
{
    card_init_once();
    if (chan < 0 || chan >= CARD_SLOTS) return CARD_RESULT_FATAL_ERROR;
    if (chan == 1) return CARD_RESULT_NOCARD;
    if (memSize) *memSize = CARD_MEM_MBIT;
    if (sectorSize) *sectorSize = CARD_SECTOR;
    return CARD_RESULT_READY;
}

s32 CARDMountAsync(s32 chan, void* workArea, CardCallback detachCallback, CardCallback attachCallback)
{
    s32 r = chan_check(chan);
    if (r != CARD_RESULT_READY) return async_fail(chan, r);
    Slot* sl = &s_slot[chan];
    sl->broken = dir_load(sl) != 0;
    sl->mounted = 1;
    if (sl->broken) re4_log("card: slot %c card.dir is corrupt, mounting as BROKEN\n", 'A' + chan);
    return async_finish(chan, sl->broken ? CARD_RESULT_BROKEN : CARD_RESULT_READY, attachCallback);
}

s32 CARDUnmount(s32 chan)
{
    card_init_once();
    if (chan < 0 || chan >= CARD_SLOTS) return CARD_RESULT_FATAL_ERROR;
    if (chan == 1) return CARD_RESULT_NOCARD;
    Slot* sl = &s_slot[chan];
    sl->pending = 0;   // the SDK cancels an operation in flight
    sl->pendingCb = 0;
    sl->mounted = 0;
    sl->result = CARD_RESULT_NOCARD;
    return CARD_RESULT_READY;
}

s32 CARDCheckAsync(s32 chan, CardCallback callback)
{
    s32 r = chan_check(chan);
    if (r != CARD_RESULT_READY) return async_fail(chan, r);
    Slot* sl = &s_slot[chan];
    if (!sl->mounted) return async_fail(chan, CARD_RESULT_NOCARD);
    return async_finish(chan, sl->broken ? CARD_RESULT_BROKEN : CARD_RESULT_READY, callback);
}

s32 CARDFormatAsync(s32 chan, CardCallback callback)
{
    s32 r = chan_check(chan);
    if (r != CARD_RESULT_READY) return async_fail(chan, r);
    Slot* sl = &s_slot[chan];
    if (!sl->mounted) return async_fail(chan, CARD_RESULT_NOCARD);
    char p[300];
    if (!sl->broken) {
        for (int i = 0; i < CARD_MAX_FILE; i++) {
            if (!sl->dir[i].used) continue;
            host_path(sl->dir[i].name, ".dat", p, sizeof p);
            file_delete(p);
            host_path(sl->dir[i].name, ".stat", p, sizeof p);
            file_delete(p);
        }
    }
    memset(sl->dir, 0, sizeof sl->dir);
    s32 res = dir_save(sl) == 0 ? CARD_RESULT_READY : CARD_RESULT_IOERROR;
    if (res == CARD_RESULT_READY) sl->broken = 0;
    return async_finish(chan, res, callback);
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed)
{
    s32 r = mounted_check(chan);
    if (r != CARD_RESULT_READY) return r;
    Slot* sl = &s_slot[chan];
    s32 files = 0;
    for (int i = 0; i < CARD_MAX_FILE; i++) if (!sl->dir[i].used) files++;
    if (byteNotUsed) *byteNotUsed = (s32) ((CARD_USER_BLOCKS - used_blocks(sl)) * CARD_SECTOR);
    if (filesNotUsed) *filesNotUsed = files;
    return CARD_RESULT_READY;
}

s32 CARDGetSerialNo(s32 chan, u64* serialNo)
{
    s32 r = mounted_check(chan);
    if (r != CARD_RESULT_READY && r != CARD_RESULT_BROKEN) return r;
    if (serialNo) *serialNo = s_serial[chan];
    return CARD_RESULT_READY;
}

static void fill_info(CardFileInfo* fi, s32 chan, int fileNo)
{
    if (!fi) return;
    fi->chan = chan;
    fi->fileNo = fileNo;
    fi->offset = 0;
    fi->length = 0;
    fi->iBlock = 0;
}

s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CardFileInfo* fileInfo, CardCallback callback)
{
    s32 r = mounted_check(chan);
    if (r != CARD_RESULT_READY) return async_fail(chan, r);
    Slot* sl = &s_slot[chan];
    if (!fileName || !fileName[0]) return async_fail(chan, CARD_RESULT_NOFILE);
    if (strlen(fileName) > CARD_NAME_MAX) return async_fail(chan, CARD_RESULT_NAMETOOLONG);
    if (size == 0 || size % CARD_SECTOR) return async_fail(chan, CARD_RESULT_FATAL_ERROR);
    if (dir_find(sl, fileName) >= 0) return async_fail(chan, CARD_RESULT_EXIST);
    int slot = -1;
    for (int i = 0; i < CARD_MAX_FILE; i++) if (!sl->dir[i].used) { slot = i; break; }
    if (slot < 0) return async_fail(chan, CARD_RESULT_NOENT);
    if (used_blocks(sl) + size / CARD_SECTOR > CARD_USER_BLOCKS) return async_fail(chan, CARD_RESULT_INSSPACE);

    DirEntry* e = &sl->dir[slot];
    memset(e, 0, sizeof *e);
    memcpy(e->name, fileName, strlen(fileName));
    e->length = size;
    e->used = 1;
    char p[300];
    CardStat st;
    stat_default(&st);
    st.time = card_time();
    host_path(e->name, ".dat", p, sizeof p);
    s32 res = CARD_RESULT_READY;
    if (file_write_all(p, 0, 0, size) != 0) res = CARD_RESULT_IOERROR;
    host_path(e->name, ".stat", p, sizeof p);
    if (res == CARD_RESULT_READY && file_write_all(p, &st, sizeof st, 0) != 0) res = CARD_RESULT_IOERROR;
    if (res == CARD_RESULT_READY && dir_save(sl) != 0) res = CARD_RESULT_IOERROR;
    if (res != CARD_RESULT_READY) {
        e->used = 0;
        return async_fail(chan, res);
    }
    fill_info(fileInfo, chan, slot);
    return async_finish(chan, CARD_RESULT_READY, callback);
}

s32 CARDDeleteAsync(s32 chan, const char* fileName, CardCallback callback)
{
    s32 r = mounted_check(chan);
    if (r != CARD_RESULT_READY) return async_fail(chan, r);
    Slot* sl = &s_slot[chan];
    if (!fileName) return async_fail(chan, CARD_RESULT_NOFILE);
    if (strlen(fileName) > CARD_NAME_MAX) return async_fail(chan, CARD_RESULT_NAMETOOLONG);
    int i = dir_find(sl, fileName);
    if (i < 0) return async_fail(chan, CARD_RESULT_NOFILE);
    char p[300];
    host_path(sl->dir[i].name, ".dat", p, sizeof p);
    file_delete(p);
    host_path(sl->dir[i].name, ".stat", p, sizeof p);
    file_delete(p);
    memset(&sl->dir[i], 0, sizeof sl->dir[i]);
    s32 res = dir_save(sl) == 0 ? CARD_RESULT_READY : CARD_RESULT_IOERROR;
    return async_finish(chan, res, callback);
}

s32 CARDOpen(s32 chan, const char* fileName, CardFileInfo* fileInfo)
{
    if (fileInfo) fileInfo->chan = -1;
    s32 r = mounted_check(chan);
    if (r != CARD_RESULT_READY) return r;
    if (!fileName) return CARD_RESULT_NOFILE;
    if (strlen(fileName) > CARD_NAME_MAX) return CARD_RESULT_NAMETOOLONG;
    int i = dir_find(&s_slot[chan], fileName);
    if (i < 0) return CARD_RESULT_NOFILE;
    fill_info(fileInfo, chan, i);
    return CARD_RESULT_READY;
}

s32 CARDClose(CardFileInfo* fileInfo)
{
    if (!fileInfo) return CARD_RESULT_FATAL_ERROR;
    s32 r = chan_check(fileInfo->chan);
    if (r != CARD_RESULT_READY) return r;
    fileInfo->chan = -1;
    return CARD_RESULT_READY;
}

// Common checks for Read/Write: an open file on a mounted slot, `length` bytes at `offset` inside it.
static s32 io_check(CardFileInfo* fi, s32 length, s32 offset, u32 align)
{
    if (!fi) return CARD_RESULT_FATAL_ERROR;
    s32 r = mounted_check(fi->chan);
    if (r != CARD_RESULT_READY) return r;
    if (fi->fileNo < 0 || fi->fileNo >= CARD_MAX_FILE || !s_slot[fi->chan].dir[fi->fileNo].used) return CARD_RESULT_NOFILE;
    if (length <= 0 || offset < 0 || (u32) length % align || (u32) offset % align) return CARD_RESULT_FATAL_ERROR;
    if ((u32) offset + (u32) length > s_slot[fi->chan].dir[fi->fileNo].length) return CARD_RESULT_LIMIT;
    return CARD_RESULT_READY;
}

s32 CARDReadAsync(CardFileInfo* fileInfo, void* buf, s32 length, s32 offset, CardCallback callback)
{
    s32 r = io_check(fileInfo, length, offset, CARD_READ_SIZE);
    if (r != CARD_RESULT_READY) return async_fail(fileInfo ? fileInfo->chan : -1, r);
    s32 chan = fileInfo->chan;
    char p[300];
    host_path(s_slot[chan].dir[fileInfo->fileNo].name, ".dat", p, sizeof p);
    s32 res = file_read_at(p, buf, (u32) length, (u32) offset) == 0 ? CARD_RESULT_READY : CARD_RESULT_IOERROR;
    if (res == CARD_RESULT_READY) {
        fileInfo->offset = offset + length;
        fileInfo->length = 0;
    }
    return async_finish(chan, res, callback);
}

s32 CARDWriteAsync(CardFileInfo* fileInfo, void* buf, s32 length, s32 offset, CardCallback callback)
{
    s32 r = io_check(fileInfo, length, offset, CARD_SECTOR);
    if (r != CARD_RESULT_READY) return async_fail(fileInfo ? fileInfo->chan : -1, r);
    s32 chan = fileInfo->chan;
    Slot* sl = &s_slot[chan];
    char p[300];
    host_path(sl->dir[fileInfo->fileNo].name, ".dat", p, sizeof p);
    s32 res = file_write_at(p, buf, (u32) length, (u32) offset) == 0 ? CARD_RESULT_READY : CARD_RESULT_IOERROR;
    if (res == CARD_RESULT_READY) {
        // the SDK stamps the directory entry's time on every write
        CardStat st;
        stat_load(sl, fileInfo->fileNo, &st);
        st.time = card_time();
        host_path(sl->dir[fileInfo->fileNo].name, ".stat", p, sizeof p);
        file_write_all(p, &st, sizeof st, 0);
        fileInfo->offset = offset + length;
        fileInfo->length = 0;
    }
    return async_finish(chan, res, callback);
}

// __CARDUpdateIconOffsets (SDK CARDStat.c)
static void update_offsets(CardStat* st)
{
    u32 offset = st->iconAddr;
    if (offset == 0xFFFFFFFF) {
        st->bannerFormat = 0;
        st->iconFormat = 0;
        st->iconSpeed = 0;
        offset = 0;
    }
    switch (st->bannerFormat & 3) {
    case 1:  // C8
        st->offsetBanner = offset;
        offset += 96 * 32;
        st->offsetBannerTlut = offset;
        offset += 2 * 256;
        break;
    case 2:  // RGB5A3
        st->offsetBanner = offset;
        offset += 2 * 96 * 32;
        st->offsetBannerTlut = 0xFFFFFFFF;
        break;
    default:
        st->offsetBanner = 0xFFFFFFFF;
        st->offsetBannerTlut = 0xFFFFFFFF;
        break;
    }
    int tlut = 0;
    for (int i = 0; i < 8; i++) {
        switch ((st->iconFormat >> (2 * i)) & 3) {
        case 1:
            st->offsetIcon[i] = offset;
            offset += 32 * 32;
            tlut = 1;
            break;
        case 2:
            st->offsetIcon[i] = offset;
            offset += 2 * 32 * 32;
            break;
        default:
            st->offsetIcon[i] = 0xFFFFFFFF;
            break;
        }
    }
    if (tlut) {
        st->offsetIconTlut = offset;
        offset += 2 * 256;
    } else {
        st->offsetIconTlut = 0xFFFFFFFF;
    }
    st->offsetData = offset;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CardStat* stat)
{
    s32 r = mounted_check(chan);
    if (r != CARD_RESULT_READY) return r;
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE || !stat) return CARD_RESULT_FATAL_ERROR;
    Slot* sl = &s_slot[chan];
    if (!sl->dir[fileNo].used) return CARD_RESULT_NOFILE;
    CardStat st;
    stat_load(sl, fileNo, &st);
    memset(stat, 0, sizeof *stat);
    memcpy(stat->fileName, sl->dir[fileNo].name, CARD_NAME_MAX);
    stat->length = sl->dir[fileNo].length;
    stat->time = st.time;
    memcpy(stat->gameName, "G4BE", 4);
    memcpy(stat->company, "08", 2);
    stat->bannerFormat = st.bannerFormat;
    stat->iconAddr = st.iconAddr;
    stat->iconFormat = st.iconFormat;
    stat->iconSpeed = st.iconSpeed;
    stat->commentAddr = st.commentAddr;
    update_offsets(stat);
    return CARD_RESULT_READY;
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CardStat* stat, CardCallback callback)
{
    s32 r = mounted_check(chan);
    if (r != CARD_RESULT_READY) return async_fail(chan, r);
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE || !stat) return async_fail(chan, CARD_RESULT_FATAL_ERROR);
    Slot* sl = &s_slot[chan];
    if (!sl->dir[fileNo].used) return async_fail(chan, CARD_RESULT_NOFILE);
    // SDK limits: icon data starts inside the first 0x200 bytes, the 64-byte comment fits in a block
    if ((stat->iconAddr != 0xFFFFFFFF && stat->iconAddr >= CARD_READ_SIZE)
        || (stat->commentAddr != 0xFFFFFFFF && stat->commentAddr % CARD_SECTOR > CARD_SECTOR - 64)) {
        return async_fail(chan, CARD_RESULT_FATAL_ERROR);
    }
    CardStat st;
    stat_default(&st);
    st.bannerFormat = stat->bannerFormat;
    st.iconAddr = stat->iconAddr;
    st.iconFormat = stat->iconFormat;
    st.iconSpeed = stat->iconSpeed;
    st.commentAddr = stat->commentAddr;
    st.time = card_time();
    char p[300];
    host_path(sl->dir[fileNo].name, ".stat", p, sizeof p);
    s32 res = file_write_all(p, &st, sizeof st, 0) == 0 ? CARD_RESULT_READY : CARD_RESULT_IOERROR;
    if (res == CARD_RESULT_READY) {
        // the SDK writes the derived fields back into the caller's struct
        memcpy(stat->fileName, sl->dir[fileNo].name, CARD_NAME_MAX);
        stat->length = sl->dir[fileNo].length;
        stat->time = st.time;
        update_offsets(stat);
    }
    return async_finish(chan, res, callback);
}

}  // extern "C"
