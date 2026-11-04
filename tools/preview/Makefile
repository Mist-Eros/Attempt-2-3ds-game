#---------------------------------------------------------------------------------
# Elemental Defense - HOST (Windows PC) preview harness
#
# Builds `preview.exe`: a windowless scripted driver that runs the game's
# platform-independent App/render/UI code against the software gfx.h backend in
# plat_host.c and dumps both 3DS screens as PNG files.  Layout verification
# only - this is NOT part of the 3DS build and never links citro2d.
#
# Usage (from this directory):
#   <make> CC="D:/tools/zig/zig.exe cc"              # uses bundled zig as C compiler
#   ./preview.exe --scenario all --out out
#
# or from the repository root:
#   <make> -C tools/preview CC="D:/tools/zig/zig.exe cc"
#
# The 3DS Makefile at the repo root is untouched; `source/plat_3ds.c` is never
# compiled here.
#---------------------------------------------------------------------------------

CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -O2
CPPFLAGS += -I../../include -I../../source -I.
LDLIBS ?= -lm

# Portable front-end + simulation sources compiled for the host.  Only the
# 3DS-specific backend (source/plat_3ds.c) is excluded.  Explicit list first
# (contract), wildcard for the core so newly added core/*.c are picked up.
GAME_SRC := $(wildcard ../../source/app.c) \
            $(wildcard ../../source/audio.c) \
            $(wildcard ../../source/render.c) \
            $(wildcard ../../source/ui.c) \
            $(wildcard ../../source/core/*.c)

PREVIEW_SRC := main.c plat_host.c
PREVIEW_HDR := host.h stb_image_write.h \
               ../../source/app.h ../../source/gfx/font8x8_basic.h

.PHONY: all clean

all: preview.exe

#---------------------------------------------------------------------------------
# main deliverable
#---------------------------------------------------------------------------------
preview.exe: $(PREVIEW_SRC) $(GAME_SRC) $(PREVIEW_HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(PREVIEW_SRC) $(GAME_SRC) $(LDLIBS)

clean:
	rm -f preview.exe
	rm -rf out
