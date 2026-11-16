# e2 backend build.
#
# The interpreter core is plain C11 with no platform dependencies so it can be
# compiled and tested on the host. The iOS layer only supplies syscalls and the
# rootfs, which keeps simulator round trips out of the instruction-debug loop.

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -Iinclude
BUILD := build
SRC := src/memory.c src/exec.c src/syscall.c
TEST_SRC := tests/main.c
TEST_BIN := $(BUILD)/e2-tests

.PHONY: all test clean

all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(TEST_BIN): $(SRC) $(TEST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(SRC) $(TEST_SRC) -o $@

test: $(TEST_BIN)
	@./$(TEST_BIN)

clean:
	rm -rf $(BUILD)
