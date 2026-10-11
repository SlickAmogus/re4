# RE4 Xbox port — nxdk build (run through build_xbox.sh so NXDK_DIR/PATH are set).
#
#   make                 build bin/default.xbe
#   make RE4_GAME=1      also compile the game units from units.mk (Phase 1; off until CT-03 lands)
#   make RE4_AUDIO_RXDK=1  link the XDK DirectSound objects from xbox_compat/dsound_objs
#
# Conventions follow jfduke3d-xbox/Makefile.nxdk and Starship/xbox/Makefile (docs/06-LESSONS-FROM-PORTS.md):
# include paths in NXDK_CFLAGS (before nxdk's own), project flags only on our objects, nxdk's Makefile last.

XBE_TITLE = Resident Evil 4
NXDK_DIR ?= $(CURDIR)/../../../nxdk
NXDK_SDL  = y
NXDK_CXX  = y
export NXDK_DIR

ROOT      = $(CURDIR)/..
PORT      = $(CURDIR)/port
COMPAT    = $(CURDIR)/xbox_compat
PREP      = $(CURDIR)/build/prep
RE4_GAME ?= 0
RE4_AUDIO_RXDK ?= 0

# --- include paths (NXDK_CFLAGS so they precede nxdk's; explicit nxdk paths because MSYS2 make
# does not propagate NXDK_DIR into nxdk-cc's shell) -------------------------------------------
RE4_INC  = -I$(PORT)/include -I$(COMPAT) -I$(ROOT)/include
RE4_INC += -I$(NXDK_DIR)/lib -I$(NXDK_DIR)/lib/winapi -I$(NXDK_DIR)/lib/xboxrt/libc_extensions
RE4_INC += -isystem $(NXDK_DIR)/lib/pdclib/include -I$(NXDK_DIR)/lib/pdclib/platform/xbox/include
RE4_INC += -I$(NXDK_DIR)/lib/xboxrt/vcruntime -I$(NXDK_DIR)/lib/sdl/SDL2/include
NXDK_CFLAGS   += $(RE4_INC)
NXDK_CXXFLAGS += $(RE4_INC)

# --- project flags, only on our objects ------------------------------------------------------
RE4_CFLAGS  = -DRE4_PORT=1 -D_XBOX=1 -DXBOX -include $(PORT)/include/re4port/re4_xbox.h
RE4_CFLAGS += -fno-strict-aliasing -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
RE4_CFLAGS += -Wno-parentheses -Wno-missing-braces -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
RE4_CFLAGS += -Wno-implicit-function-declaration -Wno-macro-redefined
RE4_CXXFLAGS = $(RE4_CFLAGS) -std=c++20 -fpermissive -fexceptions -Wno-register -Wno-writable-strings

# --- sources -----------------------------------------------------------------------------------
PORT_SRCS = \
	$(PORT)/xbox_main.cpp \
	$(PORT)/os/os_shim.cpp \
	$(PORT)/gx/gx_null.cpp \
	$(PORT)/gx/gx_stubs.cpp

COMPAT_SRCS = \
	$(COMPAT)/posix_io.c \
	$(COMPAT)/xbox_startup.c \
	$(COMPAT)/xbox_stubs.c \
	$(COMPAT)/msvc_compat.c

ifeq ($(RE4_AUDIO_RXDK),1)
COMPAT_SRCS += $(COMPAT)/dsound_bridge.c
DSOUND_OBJS  = $(wildcard $(COMPAT)/dsound_objs/*.obj)
NXDK_LDFLAGS += $(DSOUND_OBJS)
$(COMPAT)/dsound_bridge.obj: CFLAGS += -I$(COMPAT)/rxdk_include
endif

ifeq ($(RE4_GAME),1)
include $(CURDIR)/units.mk
# Game units are preprocessed into build/prep/ (asm pins stripped) before compiling.
GAME_PREP = $(addprefix $(PREP)/,$(GAME_UNITS) $(LIB_UNITS) $(foreach m,$(MODULE_NAMES),$(MODULE_$(m)_UNITS)))
$(PREP)/%: $(ROOT)/%
	@mkdir -p $(dir $@)
	python3 $(CURDIR)/tools/prep.py $< $@
GAME_SRCS = $(GAME_PREP)
$(foreach m,$(MODULE_NAMES),$(eval $(addsuffix .obj,$(basename $(addprefix $(PREP)/,$(MODULE_$(m)_UNITS)))): CFLAGS += -D_prolog=$(m)_prolog -D_epilog=$(m)_epilog -D_unresolved=$(m)_unresolved))
else
PORT_SRCS += $(PORT)/stub_game.cpp
GAME_SRCS =
endif

SRCS = $(PORT_SRCS) $(COMPAT_SRCS) $(GAME_SRCS)

RE4_OBJS = $(addsuffix .obj,$(basename $(SRCS)))
$(RE4_OBJS): CFLAGS += $(RE4_CFLAGS)
$(RE4_OBJS): CXXFLAGS += $(RE4_CXXFLAGS)

# Deep C++ call chains: the 64 KB default stack is far too small (Starship).
NXDK_LDFLAGS += -stack:524288
NXDK_LDFLAGS += /safeseh:no

OUTPUT_DIR = $(CURDIR)/bin

include $(NXDK_DIR)/Makefile
