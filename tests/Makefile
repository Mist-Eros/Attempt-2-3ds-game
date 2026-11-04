# Host unit tests for the portable Elemental Defense core.
#
# The tests are plain C99 and only need a compiler with a C library. On this
# machine the portable host compiler is Zig's clang driver, so:
#
#   make -C tests CC="D:/tools/zig/zig.exe cc"
#   make -C tests CC="D:/tools/zig/zig.exe cc" test    # build + run
#
# Any normal gcc/clang works too ("make -C tests" with gcc on PATH).

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

TARGET := test_core$(EXE)

CORE_SRC := ../source/core/data.c ../source/core/grid.c ../source/core/game.c
HEADERS  := ../include/game.h ../source/core/core_internal.h

.PHONY: all test run clean

all: $(TARGET)

$(TARGET): test_core.c $(CORE_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $(TARGET) test_core.c $(CORE_SRC) $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

test: run

clean:
	rm -f $(TARGET) *.o
