// pak_test.cpp — checks port/dvd/pak.cpp against the loose files it was packed from.
//   pak_test <re4.pak> <data dir>   (data dir holds files/ and files_disc2/)
// Built natively on Windows with -D_XBOX so pak.cpp takes its Xbox path (CreateFileA / SetFilePointerEx /
// ReadFile, 64-bit offsets) and nxdk's zlib sources with -DZ_SOLO, exactly as on the console.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <vector>
#include <string>

extern "C" {
unsigned char* re4_mem1_base;
unsigned char* re4_aram_base;
void re4_log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void re4_log_once(const char*, const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void re4_fatal(const char* f, int l, const char* m) { printf("FATAL %s:%d %s\n", f, l, m); exit(2); }
int re4_pak_open(const char* base);
int re4_pak_find(const char* name);
int re4_pak_size(int e);
int re4_pak_read(int e, unsigned ofs, void* dst, unsigned len);
}

static std::vector<unsigned char> load(const std::string& p)
{
    std::vector<unsigned char> v;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    v.resize((size_t) n);
    if (n) fread(v.data(), 1, (size_t) n, f);
    fclose(f);
    return v;
}

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: pak_test <re4.pak> <data dir> [file ...]\n"); return 2; }
    if (!re4_pak_open(argv[1])) { printf("open failed\n"); return 1; }
    const char* names[] = {
        "files/em/em10.drs", "files/em/pl00.drs", "files/etc/core.das", "files/St1/r100.das", "files/St1/r100_00.dat",
        "files/Evd/r100s00.evd", "files/Rel/st1_0.rel", "files/Movie/demo0.sfd", "files/bgm/bio4str.hed",
        "files/bgm/bio4bgm.sbb", "files_disc2/Rel/st3_0.rel", "files/ss/Eng/option.dat",
    };
    int fails = 0, checked = 0;
    srand(1234);
    std::vector<const char*> list(names, names + sizeof names / sizeof names[0]);
    if (argc > 3) list.assign(argv + 3, argv + argc);
    for (const char* n : list) {
        int e = re4_pak_find(n);
        std::vector<unsigned char> ref = load(std::string(argv[2]) + "/" + n);
        if (e < 0) { if (!ref.empty()) { printf("MISSING in pak: %s\n", n); fails++; } continue; }
        if ((size_t) re4_pak_size(e) != ref.size()) { printf("SIZE %s: %d vs %u\n", n, re4_pak_size(e), (unsigned) ref.size()); fails++; continue; }
        std::vector<unsigned char> got(ref.size());
        if (re4_pak_read(e, 0, got.data(), (unsigned) got.size()) != (int) got.size() || got != ref) { printf("WHOLE-READ MISMATCH %s\n", n); fails++; }
        for (int k = 0; k < 200 && !ref.empty(); k++) {   // random ranges incl. chunk-crossing and tails
            unsigned ofs = (unsigned) (((unsigned long long) rand() * rand()) % ref.size());
            unsigned len = 1 + (unsigned) (rand() % 200000);
            if (len > ref.size() - ofs) len = (unsigned) (ref.size() - ofs);
            std::vector<unsigned char> b(len);
            if (re4_pak_read(e, ofs, b.data(), len) != (int) len || memcmp(b.data(), ref.data() + ofs, len)) {
                printf("RANGE MISMATCH %s ofs %u len %u\n", n, ofs, len);
                fails++;
                break;
            }
        }
        checked++;
        printf("ok %-30s %10u bytes\n", n, (unsigned) ref.size());
    }
    if (re4_pak_find("FILES/EM/EM10.DRS") < 0) { printf("case-insensitive lookup failed\n"); fails++; }
    printf("%s: %d files checked, %d failures\n", fails ? "FAILED" : "OK", checked, fails);
    return fails != 0;
}
