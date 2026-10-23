API ?= 30
OUTDIR ?= build
TARGET := r9q-G990BXXSLIZH2

TARGET_HEADER := src/targets/$(TARGET)/target.h
TARGET_INCLUDE := targets/$(TARGET)/target.h
TOOLCHAIN := $(ANDROID_NDK_HOME)/toolchains/llvm/prebuilt/linux-x86_64/bin
CC := $(TOOLCHAIN)/aarch64-linux-android$(API)-clang
CC32 := $(TOOLCHAIN)/armv7a-linux-androideabi$(API)-clang

ifeq ($(wildcard $(CC)),)
$(error set ANDROID_NDK_HOME to an Android NDK containing $(CC))
endif

EXPLOIT := $(OUTDIR)/ghostlock-sm-g990b
ROOT_HELPER := $(OUTDIR)/ghostlock-root
WAITER32 := $(OUTDIR)/ghostlock-waiter32

EXPLOIT_SRCS := \
  src/main.c \
  src/util.c \
  src/slide.c \
  src/fops.c \
  src/pipe.c \
  src/root.c \
  src/preload.c \
  src/overlay_asm.S

CFLAGS := \
  -O2 -g0 -Wall -Wextra \
  -Wno-unused-parameter -Wno-sign-compare \
  -Isrc -DTARGET_HEADER='"$(TARGET_INCLUDE)"'

.DEFAULT_GOAL := all
.PHONY: all clean legacy

all: $(EXPLOIT) $(ROOT_HELPER)

legacy: $(WAITER32)

$(OUTDIR):
	mkdir -p $@

$(EXPLOIT): $(EXPLOIT_SRCS) $(TARGET_HEADER) src/common.h src/offset.h src/kernelsnitch/*.h | $(OUTDIR)
	$(CC) -fPIC $(CFLAGS) $(EXPLOIT_SRCS) -shared -pthread -o $@

$(ROOT_HELPER): src/su_daemon.c | $(OUTDIR)
	$(CC) -fPIE -pie -O2 -g0 -Wall -Wextra $< -ldl -o $@

$(WAITER32): src/waiter32.c | $(OUTDIR)
	$(CC32) -static -O2 -g0 -Wall -Wextra \
	  -Wno-unused-parameter -Wno-sign-compare $< -o $@

clean:
	rm -rf $(OUTDIR)
