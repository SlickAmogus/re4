# xbox/port/tests/ax — CT-10 AX shim + software mixer tests (host-native, 32-bit, no game data).
# Separate from ../Makefile.
#
#   make -C xbox/port/tests/ax test                                   (Linux: g++/gcc-multilib or clang)
#   make -C xbox/port/tests/ax test CXX="C:/Program Files/LLVM/bin/clang++.exe" CC="C:/Program Files/LLVM/bin/clang.exe"
#
# Output WAVs: xbox/port/tests/out/ct10_*.wav

CXX ?= clang++
CC  ?= clang
ROOT = ../../../..
PORT = ../..
SND  = $(PORT)/snd
OUT  = ../out
INC  = -I$(PORT)/include -I$(ROOT)/include -I$(SND) -include $(PORT)/include/re4port/re4_xbox.h
CXXFLAGS = -m32 -std=c++20 -O1 -g -Wall -Wno-unused-function $(INC) -DOUT_DIR=\"$(OUT)\" -D_USE_MATH_DEFINES -D_CRT_SECURE_NO_WARNINGS
CFLAGS   = -m32 -std=c99 -O1 -g -Wall -I$(SND)
LDFLAGS  = -m32
LDLIBS   = -lm -lpthread
EXE :=
ifneq ($(filter Windows_NT,$(OS))$(filter MINGW% MSYS% CYGWIN%,$(shell uname -s 2>/dev/null)),)
EXE := .exe
LDFLAGS += -fuse-ld=lld
LDLIBS :=
endif

OBJS = ax_test.o ax_shim.o snd_soft.o dspadpcm.o dspadpcm_ref_enc.o

all: ax_test$(EXE)

ax_test.o: ax_test.cpp $(PORT)/include/re4port/ax_shim.h
	"$(CXX)" $(CXXFLAGS) -c -o $@ ax_test.cpp
ax_shim.o: $(SND)/ax_shim.cpp $(PORT)/include/re4port/ax_shim.h
	"$(CXX)" $(CXXFLAGS) -c -o $@ $(SND)/ax_shim.cpp
snd_soft.o: $(SND)/snd_soft.cpp $(PORT)/include/re4port/ax_shim.h
	"$(CXX)" $(CXXFLAGS) -c -o $@ $(SND)/snd_soft.cpp
dspadpcm.o: $(SND)/dspadpcm.c $(SND)/dspadpcm.h
	"$(CC)" $(CFLAGS) -c -o $@ $(SND)/dspadpcm.c
dspadpcm_ref_enc.o: ../ct06/dspadpcm_ref_enc.c
	"$(CC)" $(CFLAGS) -c -o $@ ../ct06/dspadpcm_ref_enc.c

ax_test$(EXE): $(OBJS)
	"$(CXX)" $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

test: all
	mkdir -p $(OUT)
	./ax_test$(EXE)

clean:
	rm -f ax_test ax_test.exe *.o *.pdb *.ilk

.PHONY: all test clean
