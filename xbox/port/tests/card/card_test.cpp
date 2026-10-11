// card_test.cpp — CT-13: port/card/card_shim.cpp driven the way src/game/card.cpp drives the CARD API.
// 32-bit, no game data. Usage: card_test [scratch-dir]   (files the test creates there are removed)
// (no -include of re4_xbox.h here: it renames main; card_shim.cpp includes it itself)
//   Windows: clang++ -m32 -fuse-ld=lld -std=c++20 -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS
//            -I../../include card_test.cpp ../../card/card_shim.cpp -o card_test.exe
//   Linux:   g++ -m32 -std=gnu++14 -I../../include card_test.cpp ../../card/card_shim.cpp -o card_test
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

extern "C" {
void re4_log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void re4_log_once(const char*, const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void re4_fatal(const char* f, int l, const char* m) { printf("FATAL %s:%d %s\n", f, l, m); exit(2); }
unsigned char* re4_mem1_base;
unsigned char* re4_aram_base;
const char* re4_data_dir = "data";

typedef int s32;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;
struct CardFileInfo { s32 chan, fileNo, offset, length; u16 iBlock, pad_12; };
struct CardStat {
    char fileName[32]; u32 length; u32 time; u8 gameName[4]; u8 company[2]; u8 bannerFormat; u8 pad_2F;
    u32 iconAddr; u16 iconFormat; u16 iconSpeed; u32 commentAddr; u32 offsetBanner; u32 offsetBannerTlut;
    u32 offsetIcon[8]; u32 offsetIconTlut; u32 offsetData;
};
typedef void (*CardCallback)(s32 chan, s32 result);
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
void re4_card_poll(void);
void re4_card_set_root(const char* dir);
}
static_assert(sizeof(CardStat) == 0x6C && sizeof(CardFileInfo) == 0x14, "layout");

enum { READY = 0, BUSY = -1, NOCARD = -3, NOFILE = -4, BROKEN = -6, EXIST = -7, INSSPACE = -9, LIMIT = -11,
       NAMETOOLONG = -12, FATAL = -128 };

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long) (a), _b = (long long) (b); g_checks++; \
    if (_a != _b) { g_fail++; printf("FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static int g_cbCount;
static s32 g_cbChan, g_cbResult;
static void cb(s32 chan, s32 result) { g_cbCount++; g_cbChan = chan; g_cbResult = result; }

// The game's pattern: start, then poll CARDGetResultCode once per frame until it is not BUSY.
static s32 wait(s32 chan)
{
    CHECK_EQ(CARDGetResultCode(chan), BUSY);
    re4_card_poll();  // VIWaitForRetrace
    return CARDGetResultCode(chan);
}

static void wipe(const char* root)
{
    static const char* names[] = { "card.dir", "bh4_data00.dat", "bh4_data00.stat", "bh4_system.dat",
                                   "bh4_system.stat", "big.dat", "big.stat", "one.dat", "one.stat",
                                   "a%20b.dat", "a%20b.stat" };
    char p[400];
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(p, sizeof p, "%s%s", root, names[i]);
        remove(p);
    }
}

static s32 free_blocks(s32* files = 0)
{
    s32 bytes = -1, f = -1;
    CHECK_EQ(CARDFreeBlocks(0, &bytes, &f), READY);
    if (files) *files = f;
    return bytes / 0x2000;
}

static void mount0(s32 expect = READY)
{
    CHECK_EQ(CARDMountAsync(0, 0, 0, cb), READY);
    CHECK_EQ(wait(0), expect);
}

int main(int argc, char** argv)
{
    char root[300];
    snprintf(root, sizeof root, "%s/", argc > 1 ? argv[1] : "card_test_tmp");
    {
        char d[300];
        snprintf(d, sizeof d, "%s", root);
        d[strlen(d) - 1] = 0;
#ifdef _WIN32
        _mkdir(d);
#else
        mkdir(d, 0777);
#endif
    }
    wipe(root);
    re4_card_set_root(root);
    CARDInit();

    // ---- probe / slot B / not mounted
    s32 mem = 0, sec = 0;
    CHECK_EQ(CARDProbeEx(0, &mem, &sec), READY);
    CHECK_EQ(mem, 4);
    CHECK_EQ(sec, 0x2000);
    CHECK_EQ(CARDProbeEx(0, 0, 0), READY);
    CHECK_EQ(CARDProbeEx(1, &mem, &sec), NOCARD);
    CHECK_EQ(CARDMountAsync(1, 0, 0, 0), NOCARD);
    CHECK_EQ(CARDGetResultCode(1), NOCARD);
    CHECK_EQ(CARDUnmount(1), NOCARD);
    CHECK_EQ(CARDProbeEx(5, 0, 0), FATAL);
    CardFileInfo fi;
    CHECK_EQ(CARDOpen(0, "bh4_data00", &fi), NOCARD);
    CHECK_EQ(CARDFreeBlocks(0, 0, 0), NOCARD);

    // ---- mount (fresh card: no card.dir yet), callback from the poll
    CHECK_EQ(CARDUnmount(0), READY);
    g_cbCount = 0;
    mount0();
    CHECK_EQ(g_cbCount, 1);
    CHECK_EQ(g_cbChan, 0);
    CHECK_EQ(g_cbResult, READY);
    CHECK_EQ(CARDCheckAsync(0, 0), READY);
    CHECK_EQ(wait(0), READY);
    u64 serial = 0, serial2 = 0;
    CHECK_EQ(CARDGetSerialNo(0, &serial), READY);
    CHECK(serial != 0);
    s32 files = 0;
    CHECK_EQ(free_blocks(&files), 59);
    CHECK_EQ(files, 127);

    // ---- saveFileCheck: absent file is NOFILE
    CHECK_EQ(CARDOpen(0, "bh4_data00", &fi), NOFILE);
    CHECK_EQ(fi.chan, -1);

    // ---- fileCreate: 8 blocks
    const int N = 8;
    CHECK_EQ(CARDCreateAsync(0, "bh4_data00", N << 13, &fi, 0), READY);
    CardFileInfo fiBusy;
    CHECK_EQ(CARDOpen(0, "bh4_data00", &fiBusy), BUSY);  // sync call while the async op is in flight
    CHECK_EQ(wait(0), READY);
    CHECK_EQ(fi.chan, 0);
    s32 fileNo = fi.fileNo;
    CHECK_EQ(free_blocks(&files), 59 - N);
    CHECK_EQ(files, 126);
    CardFileInfo fi2;
    CHECK_EQ(CARDCreateAsync(0, "bh4_data00", 0x2000, &fi2, 0), EXIST);
    CHECK_EQ(CARDGetResultCode(0), EXIST);
    CHECK_EQ(CARDCreateAsync(0, "x", 0x1000, &fi2, 0), FATAL);
    CHECK_EQ(CARDCreateAsync(0, "123456789012345678901234567890123", 0x2000, &fi2, 0), NAMETOOLONG);

    // ---- a file whose status was never set reads as "corrupt" (commentAddr -1, card.cpp:2608)
    CardStat st;
    CHECK_EQ(CARDGetStatus(0, fileNo, &st), READY);
    CHECK_EQ(st.commentAddr, 0xFFFFFFFFu);
    CHECK_EQ(st.length, (u32) (N << 13));
    CHECK(strcmp(st.fileName, "bh4_data00") == 0);
    CHECK(memcmp(st.gameName, "G4BE", 4) == 0 && memcmp(st.company, "08", 2) == 0);

    // ---- fileWrite: N blocks from offset 0
    static u8 buf[N << 13], back[N << 13];
    for (int i = 0; i < (N << 13); i++) buf[i] = (u8) (i * 7 + (i >> 13));
    CHECK_EQ(CARDWriteAsync(&fi, buf, N << 13, 0, cb), READY);
    g_cbCount = 0;
    CHECK_EQ(wait(0), READY);
    CHECK_EQ(g_cbCount, 1);
    CHECK_EQ(CARDWriteAsync(&fi, buf, 0x200, 0, 0), FATAL);           // writes are whole sectors
    CHECK_EQ(CARDWriteAsync(&fi, buf, 0x2000, N << 13, 0), LIMIT);    // past the end
    CHECK_EQ(CARDClose(&fi), READY);
    CHECK_EQ(fi.chan, -1);

    // ---- makeCardStatus + CARDSetStatusAsync (card.cpp:1905, 1014)
    CHECK_EQ(CARDOpen(0, "bh4_data00", &fi), READY);
    CHECK_EQ(fi.fileNo, fileNo);
    CHECK_EQ(CARDGetStatus(0, fi.fileNo, &st), READY);
    st.bannerFormat = (u8) (((st.bannerFormat & ~3) | 2) | 4);  // RGB5A3 banner
    st.commentAddr = 0;
    st.iconAddr = 0x40;
    st.iconFormat = 0x0001;   // one C8 icon
    st.iconSpeed = 0x0003;
    CHECK_EQ(CARDSetStatusAsync(0, fi.fileNo, &st, 0), READY);
    CHECK_EQ(wait(0), READY);
    CHECK_EQ(st.offsetBanner, 0x40u);
    CardStat st2;
    CHECK_EQ(CARDGetStatus(0, fi.fileNo, &st2), READY);
    CHECK_EQ(st2.commentAddr, 0u);
    CHECK_EQ(st2.iconAddr, 0x40u);
    CHECK_EQ(st2.bannerFormat, 6);
    CHECK_EQ(st2.iconFormat, 1);
    CHECK_EQ(st2.iconSpeed, 3);
    CHECK_EQ(st2.offsetBanner, 0x40u);
    CHECK_EQ(st2.offsetBannerTlut, 0xFFFFFFFFu);
    CHECK_EQ(st2.offsetIcon[0], 0x40u + 0x1800);
    CHECK_EQ(st2.offsetIcon[1], 0xFFFFFFFFu);
    CHECK_EQ(st2.offsetIconTlut, 0x40u + 0x1800 + 0x400);
    CHECK_EQ(st2.offsetData, 0x40u + 0x1800 + 0x400 + 0x200);
    CHECK(st2.time != 0);
    st2.iconAddr = 0x200;  // icon must start inside the first 0x200 bytes
    CHECK_EQ(CARDSetStatusAsync(0, fi.fileNo, &st2, 0), FATAL);

    // ---- fileRead: the 0x200 header at 0x2000 (saveFileCheck), then everything
    memset(back, 0, sizeof back);
    CHECK_EQ(CARDReadAsync(&fi, back, 0x200, 0x2000, 0), READY);
    CHECK_EQ(wait(0), READY);
    CHECK(memcmp(back, buf + 0x2000, 0x200) == 0);
    CHECK_EQ(CARDReadAsync(&fi, back, N << 13, 0, 0), READY);
    CHECK_EQ(wait(0), READY);
    CHECK(memcmp(back, buf, N << 13) == 0);
    CHECK_EQ(CARDReadAsync(&fi, back, 0x100, 0, 0), FATAL);
    CHECK_EQ(CARDReadAsync(&fi, back, 0x200, N << 13, 0), LIMIT);
    CHECK_EQ(CARDClose(&fi), READY);

    // ---- persistence across unmount / mount (reloads card.dir)
    CHECK_EQ(CARDUnmount(0), READY);
    CHECK_EQ(CARDOpen(0, "bh4_data00", &fi), NOCARD);
    mount0();
    CHECK_EQ(CARDGetSerialNo(0, &serial2), READY);
    CHECK_EQ(serial2, serial);
    CHECK_EQ(free_blocks(), 59 - N);
    CHECK_EQ(CARDOpen(0, "bh4_data00", &fi), READY);
    memset(back, 0, sizeof back);
    CHECK_EQ(CARDReadAsync(&fi, back, N << 13, 0, 0), READY);
    CHECK_EQ(wait(0), READY);
    CHECK(memcmp(back, buf, N << 13) == 0);
    CHECK_EQ(CARDGetStatus(0, fi.fileNo, &st2), READY);
    CHECK_EQ(st2.commentAddr, 0u);
    CHECK_EQ(CARDClose(&fi), READY);

    // ---- free blocks accounting and INSSPACE
    CHECK_EQ(CARDCreateAsync(0, "bh4_system", 0x2000, &fi, 0), READY);
    CHECK_EQ(wait(0), READY);
    CHECK_EQ(free_blocks(&files), 59 - N - 1);
    CHECK_EQ(files, 125);
    CHECK_EQ(CARDCreateAsync(0, "big", (59 - N - 1 + 1) << 13, &fi, 0), INSSPACE);
    CHECK_EQ(CARDGetResultCode(0), INSSPACE);
    CHECK_EQ(CARDCreateAsync(0, "big", (59 - N - 1) << 13, &fi, 0), READY);
    CHECK_EQ(wait(0), READY);
    CHECK_EQ(free_blocks(), 0);
    CHECK_EQ(CARDCreateAsync(0, "one", 0x2000, &fi, 0), INSSPACE);
    // names outside [A-Za-z0-9_-] are escaped on the host
    CHECK_EQ(CARDDeleteAsync(0, "big", 0), READY);
    CHECK_EQ(wait(0), READY);
    CHECK_EQ(CARDCreateAsync(0, "a b", 0x2000, &fi, 0), READY);
    CHECK_EQ(wait(0), READY);
    CHECK_EQ(CARDOpen(0, "a b", &fi), READY);
    CHECK_EQ(CARDClose(&fi), READY);

    // ---- delete -> NOFILE
    CHECK_EQ(CARDDeleteAsync(0, "bh4_data00", 0), READY);
    CHECK_EQ(wait(0), READY);
    CHECK_EQ(CARDOpen(0, "bh4_data00", &fi), NOFILE);
    CHECK_EQ(CARDDeleteAsync(0, "bh4_data00", 0), NOFILE);
    CHECK_EQ(CARDGetStatus(0, fileNo, &st2), NOFILE);
    CHECK_EQ(free_blocks(), 59 - 1 - 1);

    // ---- safety net: no poll at all, the 4th CARDGetResultCode completes the op
    CHECK_EQ(CARDCheckAsync(0, 0), READY);
    CHECK_EQ(CARDGetResultCode(0), BUSY);
    CHECK_EQ(CARDGetResultCode(0), BUSY);
    CHECK_EQ(CARDGetResultCode(0), BUSY);
    CHECK_EQ(CARDGetResultCode(0), READY);

    // ---- format
    CHECK_EQ(CARDFormatAsync(0, 0), READY);
    CHECK_EQ(wait(0), READY);
    CHECK_EQ(free_blocks(&files), 59);
    CHECK_EQ(files, 127);
    CHECK_EQ(CARDOpen(0, "bh4_system", &fi), NOFILE);

    // ---- corrupt card.dir: mounts BROKEN, check BROKEN (game offers format), format repairs it
    CHECK_EQ(CARDUnmount(0), READY);
    {
        char p[400];
        snprintf(p, sizeof p, "%scard.dir", root);
        FILE* f = fopen(p, "wb");
        if (f) {
            fputs("garbage", f);
            fclose(f);
        }
    }
    mount0(BROKEN);
    CHECK_EQ(CARDCheckAsync(0, 0), READY);
    CHECK_EQ(wait(0), BROKEN);
    CHECK_EQ(CARDFreeBlocks(0, 0, 0), BROKEN);
    CHECK_EQ(CARDFormatAsync(0, 0), READY);
    CHECK_EQ(wait(0), READY);
    CHECK_EQ(CARDCheckAsync(0, 0), READY);
    CHECK_EQ(wait(0), READY);
    CHECK_EQ(free_blocks(), 59);

    CHECK_EQ(CARDUnmount(0), READY);
    wipe(root);
    printf("card_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
