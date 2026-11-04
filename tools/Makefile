# Headless auto-play / balance sim for the portable game core.
#
#   make -C tools CC="D:/tools/zig/zig.exe cc"        # build
#   make -C tools CC="D:/tools/zig/zig.exe cc" run    # build + run
#
# Any normal gcc/clang works too ("make -C tools" with gcc on PATH).

CC      ?= gcc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -pedantic
CFLAGS  += -I../include -I../source/core
LDFLAGS ?=
LDLIBS  ?= -lm

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

TARGET := sim$(EXE)
CORE_SRC := ../source/core/data.c ../source/core/grid.c ../source/core/game.c
HEADERS  := ../include/game.h ../source/core/core_internal.h

.PHONY: all run clean

all: $(TARGET)

$(TARGET): sim.c $(CORE_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $(TARGET) sim.c $(CORE_SRC) $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o
