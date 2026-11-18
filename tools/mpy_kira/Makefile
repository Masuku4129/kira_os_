# MicroPython для Киры (wasm32-wasi). Внешний каталог MicroPython: MPY_TOP (по умолчанию build-wasm/mpy/src, см. ../mp_wasm.sh), sysroot: KIRA_WASI.
TOP ?= $(MPY_TOP)
include $(TOP)/py/mkenv.mk
CROSS = 0
QSTR_DEFS = qstrdefsport.h
MICROPY_ROM_TEXT_COMPRESSION ?= 1
include $(TOP)/py/py.mk
include $(TOP)/extmod/extmod.mk

SYSROOT ?= $(KIRA_WASI)/sysroot
CC = clang
LD = wasm-ld
CFLAGS += --target=wasm32-wasip1 --sysroot=$(SYSROOT) -I./include -I. -I$(TOP) -I$(BUILD) -std=gnu99 -Os -DNDEBUG -fdata-sections -ffunction-sections \
          -Wall -Wno-unused-parameter -Wno-error -fno-strict-aliasing -D_WASI_EMULATED_SIGNAL
LDFLAGS += -z stack-size=65536 --initial-memory=$(INITMEM) --max-memory=$(INITMEM) --gc-sections --no-entry --export=_start -L$(SYSROOT)/lib/wasm32-wasip1
INITMEM ?= 8388608
LIBS = $(SYSROOT)/lib/wasm32-wasip1/crt1-command.o -lc $(SYSROOT)/lib/clang_rt/libclang_rt.builtins-wasm32.a

SRC_C = main.c \
	shared/runtime/stdout_helpers.c

OBJ = $(PY_O) $(addprefix $(BUILD)/, $(SRC_C:.c=.o))

all: $(BUILD)/python.wasm
$(BUILD)/python.wasm: $(OBJ)
	$(ECHO) "LINK $@"
	$(Q)$(LD) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)
include $(TOP)/py/mkrules.mk
