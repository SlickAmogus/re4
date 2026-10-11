# xbox/port/tests/ct06 — CT-06 codec tests (host-native, no game data). Separate from ../Makefile.
#
#   make -C xbox/port/tests/ct06 test                        (Linux: gcc/clang)
#   make -C xbox/port/tests/ct06 test CC="C:/Program Files/LLVM/bin/clang.exe"   (Windows, MSYS2 make)
#   py -3 xbox/port/tests/ct06/test_adpcm.py                 (Python wrapper test; or pytest on it)

CC ?= clang
SND = ../../snd
CFLAGS = -std=c99 -O1 -g -Wall -Wextra -Wno-unused-function -I$(SND)
LDLIBS = -lm
EXE :=
ifeq ($(OS),Windows_NT)
EXE := .exe
endif

all: ct06_dsp$(EXE) ct06_xbox$(EXE)

ct06_dsp$(EXE): test_dspadpcm.c dspadpcm_ref_enc.c $(SND)/dspadpcm.c $(SND)/dspadpcm.h dspadpcm_ref_enc.h ct06_test.h
	"$(CC)" $(CFLAGS) -o $@ test_dspadpcm.c dspadpcm_ref_enc.c $(SND)/dspadpcm.c $(LDLIBS)

ct06_xbox$(EXE): test_xboxadpcm.c $(SND)/xboxadpcm.c $(SND)/xboxadpcm.h ct06_test.h
	"$(CC)" $(CFLAGS) -o $@ test_xboxadpcm.c $(SND)/xboxadpcm.c $(LDLIBS)

test: all
	./ct06_dsp$(EXE)
	./ct06_xbox$(EXE)

clean:
	rm -f ct06_dsp ct06_dsp.exe ct06_xbox ct06_xbox.exe *.o *.pdb *.ilk

.PHONY: all test clean
