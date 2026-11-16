# e2 backend build.
#
# The interpreter core is plain C11 with no platform dependencies so it can be
# compiled and tested on the host. The iOS layer only supplies syscalls and the
# rootfs, which keeps simulator round trips out of the instruction-debug loop.

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -Iinclude
BUILD := build
SRC := src/memory.c src/exec.c src/syscall.c src/loader.c
TEST_SRC := tests/main.c
TEST_BIN := $(BUILD)/e2-tests
GUEST_ELF := tests/guest/hello.elf tests/guest/fib.elf tests/guest/memset.elf tests/guest/cprog.elf

# Cross-toolchain pieces for building the guest test binary. clang handles the
# aarch64 target and lld links it statically, so no libc is needed.
CLANG ?= clang
LLD ?= ld.lld

.PHONY: all test clean

all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(TEST_BIN): $(SRC) $(TEST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(SRC) $(TEST_SRC) -o $@

tests/guest/hello.elf: tests/guest/hello.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/hello-guest.o
	$(LLD) -static -o $@ $(BUILD)/hello-guest.o

tests/guest/fib.elf: tests/guest/fib.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/fib-guest.o
	$(LLD) -static -o $@ $(BUILD)/fib-guest.o

tests/guest/memset.elf: tests/guest/memset.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/memset-guest.o
	$(LLD) -static -o $@ $(BUILD)/memset-guest.o

tests/guest/cprog.elf: tests/guest/cprog.c | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -O2 -ffreestanding -nostdlib -static -c $< -o $(BUILD)/cprog-guest.o
	$(LLD) -static -o $@ $(BUILD)/cprog-guest.o

test: $(TEST_BIN) $(GUEST_ELF)
	@./$(TEST_BIN)

clean:
	rm -rf $(BUILD)
