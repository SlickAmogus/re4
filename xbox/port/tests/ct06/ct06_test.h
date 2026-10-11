/* ct06_test.h — minimal test harness shared by the CT-06 test programs. */
#ifndef CT06_TEST_H
#define CT06_TEST_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int ct06_fails = 0;
static int ct06_checks = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ct06_checks++;                                                               \
        if (!(cond)) {                                                               \
            ct06_fails++;                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                            \
    } while (0)

#define CHECK_EQ_U32(a, b)                                                                   \
    do {                                                                                     \
        uint32_t _a = (uint32_t) (a), _b = (uint32_t) (b);                                   \
        ct06_checks++;                                                                       \
        if (_a != _b) {                                                                      \
            ct06_fails++;                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%u != %u)\n", __FILE__, __LINE__, #a, #b, \
                    (unsigned) _a, (unsigned) _b);                                           \
        }                                                                                    \
    } while (0)

/* SNR in dB of `test` against `ref` over n samples (inf when identical). */
static double ct06_snr_db(const int16_t* ref, const int16_t* test, uint32_t n)
{
    double sig = 0.0, err = 0.0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        double r = ref[i], d = (double) test[i] - r;
        sig += r * r;
        err += d * d;
    }
    if (err == 0.0) return 999.0;
    if (sig == 0.0) return -999.0;
    return 10.0 * log10(sig / err);
}

/* Fill `out` with a sine of `freq` Hz at `rate` Hz, peak `amp`. */
static void ct06_sine(int16_t* out, uint32_t n, double freq, double rate, double amp)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        double v = amp * sin(2.0 * 3.14159265358979323846 * freq * (double) i / rate);
        out[i] = (int16_t) floor(v + 0.5);
    }
}

static int ct06_finish(const char* name)
{
    printf("%s: %d checks, %d failures -> %s\n", name, ct06_checks, ct06_fails, ct06_fails ? "FAIL" : "OK");
    return ct06_fails ? 1 : 0;
}

#endif /* CT06_TEST_H */
