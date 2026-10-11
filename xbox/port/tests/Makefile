# xbox/port/tests — headless tests of the port layer (no game data, no GPU). 32-bit like the port.
#
#   make -C xbox/port/tests gx && ./xbox/port/tests/gx
#
# TEV_CASES: the CT-05 catalogue replayed by the gx test; defaults to xbox/port/gx/tests/tev_cases.json
# when it exists, else the hand-written sample in data/.

CXX ?= clang++
PY  ?= python3

PORT     = ..
ROOT     = ../../..
CXXFLAGS = -m32 -O1 -g -fno-strict-aliasing -DRE4_PORT=1 -include $(PORT)/include/re4port/re4_xbox.h \
           -I$(PORT)/include -I$(ROOT)/include -std=gnu++14 -fpermissive \
           -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
LDLIBS   = -lm

GX_SRCS  = $(PORT)/gx/gx_state.cpp $(PORT)/gx/gx_dl.cpp $(PORT)/gx/gx_pipeline.cpp $(PORT)/gx/gx_null.cpp \
           $(PORT)/os/os_shim.cpp
GX_HDRS  = $(PORT)/gx/gx_state.h $(PORT)/gx/gx_pipeline.h $(PORT)/gx/gx_vertex.h \
           $(PORT)/include/re4port/gx_shim.h $(PORT)/include/re4port/gx_fifo.h \
           $(PORT)/include/re4port/gx_null_log.h $(PORT)/include/re4port/re4_xbox.h

REAL_CASES = $(PORT)/gx/tests/tev_cases.json
TEV_CASES ?= $(if $(wildcard $(REAL_CASES)),$(REAL_CASES),data/tev_cases_sample.json)
SCHEMA     = $(PORT)/gx/tests/tev_cases.schema.json

all: gx

tev_cases_gen.cpp: gen_tev_cases.py $(TEV_CASES) $(SCHEMA)
	$(PY) gen_tev_cases.py $(TEV_CASES) $@ --schema $(SCHEMA)

gx: gx_test.cpp gx_test_cases.h tev_cases_gen.cpp $(GX_SRCS) $(GX_HDRS)
	$(CXX) $(CXXFLAGS) -DGX_TEST_HAVE_TEV_CASES=1 -I$(PORT)/gx -o $@ gx_test.cpp tev_cases_gen.cpp $(GX_SRCS) $(LDLIBS)

# the same build with the other compiler, to catch GCC-/clang-isms
gx-gcc:
	$(MAKE) CXX=g++ gx

test: gx
	./gx

clean:
	rm -f gx tev_cases_gen.cpp

.PHONY: all gx-gcc test clean
