# xbox/port/tests/ct02 -- CT-02 tests (asm -> C replacements). Pure functions, no game headers.
#
#   make -C xbox/port/tests/ct02 && ./xbox/port/tests/ct02/ct02 [r120.das r120.ref.bin]
#   make -C xbox/port/tests/ct02 M=-m32          # 32-bit (Linux, gcc-multilib)
#   make -C xbox/port/tests/ct02 CC=clang CXX=clang++ LDLIBS=   # Windows host LLVM (no libm)
#
# The yz2 file check takes a real stream + the Python oracle's output:
#   py -3 -I tools/motion/yz2.py <disc1/files/St1/r120.das> r120.ref.bin

CC     ?= cc
CXX    ?= c++
M      ?=
PORT    = ../..
OPT     = $(M) -O2 -g -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter \
          -I$(PORT)/include -I$(PORT)/mtx -I$(PORT)/os -I$(PORT)/data
CFLAGS   = $(OPT) -std=c99
CXXFLAGS = $(OPT) -std=c++14
LDLIBS  ?= -lm
EXE    ?= ct02

OBJS = skin.o math_ppc.o memfill.o yz2.o ct02_test.o

all: $(EXE)

$(EXE): $(OBJS)
	"$(CXX)" $(M) -o $@ $(OBJS) $(LDLIBS)

skin.o: $(PORT)/gx/skin.cpp $(PORT)/include/re4port/skin.h
	"$(CXX)" $(CXXFLAGS) -c -o $@ $<
math_ppc.o: $(PORT)/mtx/math_ppc.cpp $(PORT)/mtx/math_ppc.h
	"$(CXX)" $(CXXFLAGS) -c -o $@ $<
memfill.o: $(PORT)/os/memfill.c $(PORT)/os/memfill.h
	"$(CC)" $(CFLAGS) -c -o $@ $<
yz2.o: $(PORT)/data/yz2.c $(PORT)/data/yz2.h
	"$(CC)" $(CFLAGS) -c -o $@ $<
ct02_test.o: ct02_test.cpp
	"$(CXX)" $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) ct02 ct02.exe

.PHONY: all clean
