# RE4 Xbox port — nxdk build (run through build_xbox.sh so NXDK_DIR/PATH are set).
#
#   make                 build bin/default.xbe
#   make RE4_GAME=1      also compile the game units from units.mk (Phase 1; off until CT-03 lands)
#   make RE4_AUDIO_RXDK=1  link the XDK DirectSound objects from xbox_compat/dsound_objs
#
# Conventions follow jfduke3d-xbox/Makefile.nxdk and Starship/xbox/Makefile (docs/06-LESSONS-FROM-PORTS.md):
# include paths in NXDK_CFLAGS (before nxdk's own), project flags only on our objects, nxdk's Makefile last.

XBE_TITLE = Resident Evil 4
.DEFAULT_GOAL := all
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
PY       ?= python3

# --- include paths (NXDK_CFLAGS so they precede nxdk's; explicit nxdk paths because MSYS2 make
# does not propagate NXDK_DIR into nxdk-cc's shell) -------------------------------------------
RE4_INC  = -I$(PORT)/include -I$(COMPAT) -I$(PREP)/include -I$(ROOT)/include -I$(ROOT)/src -I$(ROOT)/src/lib
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
RE4_CXXFLAGS = $(RE4_CFLAGS) -std=c++20 -fpermissive -fno-exceptions -Wno-register -Wno-writable-strings
RE4_CXXFLAGS += -fno-access-control -Wno-address-of-temporary -Wno-c++11-narrowing

# --- sources -----------------------------------------------------------------------------------
PORT_SRCS = \
	$(PORT)/xbox_main.cpp \
	$(PORT)/os/os_shim.cpp \
	$(PORT)/gx/gx_null.cpp \
	$(PORT)/gx/gx_state.cpp \
	$(PORT)/gx/gx_dl.cpp \
	$(PORT)/gx/gx_pipeline.cpp \
	$(PORT)/mtx/mtx_extra.c \
	$(PORT)/mtx/math_ppc.cpp \
	$(PORT)/gx/skin.cpp \
	$(PORT)/os/memfill.c \
	$(PORT)/data/yz2.c

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
GAME_PREP = $(addprefix $(PREP)/,$(GAME_UNITS) $(LIB_UNITS))
$(PREP)/%: $(ROOT)/%
	@mkdir -p $(dir $@)
	$(PY) $(CURDIR)/tools/prep.py $< $@
GAME_SRCS = $(GAME_PREP)
# the headers are prepped too (include/math_sub.h carries an inline asm); one run over the tree
PREP_HEADERS = $(PREP)/include/.stamp
$(PREP_HEADERS): $(wildcard $(ROOT)/include/*.h $(ROOT)/include/*/*.h) $(CURDIR)/tools/prep.py
	$(PY) $(CURDIR)/tools/prep.py --dir $(ROOT)/include $(PREP)/include
	$(PY) $(CURDIR)/tools/prep.py --dir $(ROOT)/src/lib $(PREP)/src/lib
	@echo prepped > $@
$(addsuffix .obj,$(basename $(GAME_PREP))): $(PREP_HEADERS)

# --- REL modules: one prepped copy of each unit per module (shared sources compile once per module),
# compiled by nxdk's rules, then namespaced by tools/isolate_modules.py into build/mod/<m>.lib ---
MODBUILD = $(CURDIR)/build/mod
MODISO   = $(CURDIR)/build/iso
LLVM_BIN ?= C:/Program Files/LLVM/bin
export LLVM_BIN
# $(1) module, $(2) "src:obj" unit. The prepped copy is named <obj>.<source stem> so a source that
# #includes a sibling of the unit's name (st/em_wrap_v2.cpp includes "em_wrap.cpp") cannot include
# itself, and the source's own directory is on the include path for such sibling includes.
U_SRC = $(word 1,$(subst :, ,$(1)))
U_OBJ = $(word 2,$(subst :, ,$(1)))
U_OUT = $(MODBUILD)/$(call U_OBJ,$(1)).$(basename $(notdir $(call U_SRC,$(1))))
define MODULE_UNIT
$(call U_OUT,$(2))$(suffix $(call U_SRC,$(2))): $(ROOT)/src/$(call U_SRC,$(2)) $(CURDIR)/tools/prep.py
	@mkdir -p $$(dir $$@)
	$(PY) $(CURDIR)/tools/prep.py $$< $$@
$(call U_OUT,$(2)).obj: CXXFLAGS += -I$(dir $(ROOT)/src/$(call U_SRC,$(2)))
$(call U_OUT,$(2)).obj: CFLAGS += -I$(dir $(ROOT)/src/$(call U_SRC,$(2)))
MODULE_OBJS_$(1) += $(call U_OUT,$(2)).obj
endef
define MODULE_LIB
$(foreach u,$(MODULE_$(1)),$(eval $(call MODULE_UNIT,$(1),$(u))))
$$(MODULE_OBJS_$(1)): $(PREP_HEADERS)
$(MODBUILD)/$(1).lib: $$(MODULE_OBJS_$(1)) $(CURDIR)/tools/isolate_modules.py
	@echo "[ ISOLATE  ] $(1)"
	$$(VE)$$(LIB) -out:'$$@' $$$$($(PY) $(CURDIR)/tools/isolate_modules.py $(1) $(MODISO) $$(MODULE_OBJS_$(1)))
MODULE_LIBS += $(MODBUILD)/$(1).lib
MODULE_ALL_OBJS += $$(MODULE_OBJS_$(1))
endef
$(foreach m,$(MODULE_NAMES),$(eval $(call MODULE_LIB,$(m))))
$(MODULE_ALL_OBJS): CFLAGS += $(RE4_CFLAGS)
$(MODULE_ALL_OBJS): CXXFLAGS += $(RE4_CXXFLAGS)
main.exe: $(MODULE_LIBS)
PORT_SRCS += $(PORT)/os/module_table.cpp $(PORT)/os/alias_thunks.cpp $(CURDIR)/build/link_stubs.c
PORT_SRCS += $(PORT)/os/os_thread.cpp $(PORT)/os/os_misc.cpp $(PORT)/os/vi_pad.cpp $(PORT)/dvd/dvd_shim.cpp
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

# --- link closure (tools/link_resolve.py): a probe link lists what is still undefined; the resolver
# binds module imports / type-mismatched declarations with /alternatename and sends the rest to the
# catch-all stubs in build/link_stubs.c (regenerated each build, doubles as the SDK to-do list) ---
ifeq ($(RE4_GAME),1)
RE4_RSP     = $(CURDIR)/build/link_aliases.rsp
RE4_STUBS_C = $(CURDIR)/build/link_stubs.c
RE4_PROBE_OBJS = $(filter-out %/link_stubs.obj,$(OBJS))
$(RE4_RSP): $(RE4_PROBE_OBJS) $(MODULE_LIBS) $(CURDIR)/tools/link_resolve.py
	@echo "[ RESOLVE  ] $@"
	-$(VE)$(LD) $(NXDK_LDFLAGS) /errorlimit:0 /demangle:no -out:'$(CURDIR)/build/probe.exe' $(RE4_PROBE_OBJS) $(MODULE_LIBS) $(NXDK_DIR)/lib/xboxkrnl/libxboxkrnl.lib > $(CURDIR)/build/probe.log 2>&1
	$(VE)sed -n 's/.*undefined symbol: //p' $(CURDIR)/build/probe.log | sort -u > $(CURDIR)/build/undefined.txt
	$(VE)$(PY) $(CURDIR)/tools/link_resolve.py $(CURDIR)/build/undefined.txt $@ $(RE4_STUBS_C) $(RE4_PROBE_OBJS) $(MODULE_LIBS) $(NXDK_DIR)/lib
$(RE4_STUBS_C): $(RE4_RSP)
main.exe: LDFLAGS += @$(RE4_RSP)
endif
