// gx_test_cases.h — table shape produced by gen_tev_cases.py and consumed by gx_test.cpp (CT-08).
#ifndef RE4PORT_GX_TEST_CASES_H
#define RE4PORT_GX_TEST_CASES_H

struct GxTevCase {
    const char* id;
    int (*replay)(void);   // replays the case into the GX state; returns 1 when approximate
    const char* file;
    int approximate;
    int fits_8_stages;
    int textures;
};

extern const GxTevCase gx_tev_cases[];
extern const int gx_tev_case_count;

#endif
