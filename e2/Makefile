# e2 backend build.
#
# The interpreter core is plain C11 with no platform dependencies so it can be
# compiled and tested on the host. The iOS layer only supplies syscalls and the
# rootfs, which keeps simulator round trips out of the instruction-debug loop.

CC ?= cc
LDLIBS ?= -lm
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -Iinclude
BUILD := build
SRC := src/memory.c src/exec.c src/syscall.c src/loader.c
HDR := $(wildcard include/e2/*.h)
TEST_SRC := tests/main.c
TEST_BIN := $(BUILD)/e2-tests
GUEST_ELF := tests/guest/jump.elf tests/guest/auxv.elf tests/guest/loop.elf tests/guest/ldp.elf tests/guest/bitfield.elf tests/guest/hello.elf tests/guest/fib.elf tests/guest/memset.elf tests/guest/cprog.elf tests/guest/calls.elf tests/guest/mul.elf tests/guest/shll.elf tests/guest/bitops.elf tests/guest/simdext.elf tests/guest/permute.elf tests/guest/divshift.elf tests/guest/loadsig.elf tests/guest/ldrsize.elf

# Cross-toolchain pieces for building the guest test binary. clang handles the
# aarch64 target and lld links it statically, so no libc is needed.
CLANG ?= clang
LLD ?= ld.lld

.PHONY: all test clean harness

# `all` builds the harness too. Building it needs nothing beyond a C compiler - only
# running it needs a root filesystem - so a fresh checkout that runs `make` gets
# everything the sweep and diff scripts require. Leaving it out meant ./tools/verify.sh
# stopped at "e2/tools/rune missing" with no hint that a separate make target existed.
all: test harness

$(BUILD):
	@mkdir -p $(BUILD)

$(TEST_BIN): $(SRC) $(TEST_SRC) $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(SRC) $(TEST_SRC) -o $@ $(LDLIBS)

tests/guest/ldrsize.elf: tests/guest/ldrsize.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/ldrsize-guest.o
	$(LLD) -static -o $@ $(BUILD)/ldrsize-guest.o

tests/guest/loadsig.elf: tests/guest/loadsig.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/loadsig-guest.o
	$(LLD) -static -o $@ $(BUILD)/loadsig-guest.o

tests/guest/divshift.elf: tests/guest/divshift.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/divshift-guest.o
	$(LLD) -static -o $@ $(BUILD)/divshift-guest.o

tests/guest/permute.elf: tests/guest/permute.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/permute-guest.o
	$(LLD) -static -o $@ $(BUILD)/permute-guest.o

tests/guest/simdext.elf: tests/guest/simdext.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/simdext-guest.o
	$(LLD) -static -o $@ $(BUILD)/simdext-guest.o

tests/guest/bitops.elf: tests/guest/bitops.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/bitops-guest.o
	$(LLD) -static -o $@ $(BUILD)/bitops-guest.o

tests/guest/jump.elf: tests/guest/jump.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/jump-guest.o
	$(LLD) -static -o $@ $(BUILD)/jump-guest.o

tests/guest/auxv.elf: tests/guest/auxv.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/auxv-guest.o
	$(LLD) -static -o $@ $(BUILD)/auxv-guest.o

tests/guest/loop.elf: tests/guest/loop.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/loop-guest.o
	$(LLD) -static -o $@ $(BUILD)/loop-guest.o

tests/guest/ldp.elf: tests/guest/ldp.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/ldp-guest.o
	$(LLD) -static -o $@ $(BUILD)/ldp-guest.o

tests/guest/bitfield.elf: tests/guest/bitfield.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/bitfield-guest.o
	$(LLD) -static -o $@ $(BUILD)/bitfield-guest.o

tests/guest/hello.elf: tests/guest/hello.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/hello-guest.o
	$(LLD) -static -o $@ $(BUILD)/hello-guest.o

tests/guest/fib.elf: tests/guest/fib.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/fib-guest.o
	$(LLD) -static -o $@ $(BUILD)/fib-guest.o

tests/guest/memset.elf: tests/guest/memset.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/memset-guest.o
	$(LLD) -static -o $@ $(BUILD)/memset-guest.o

tests/guest/mul.elf: tests/guest/mul.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/mul-guest.o
	$(LLD) -static -o $@ $(BUILD)/mul-guest.o

tests/guest/calls.elf: tests/guest/calls.s | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -c $< -o $(BUILD)/calls-guest.o
	$(LLD) -static -o $@ $(BUILD)/calls-guest.o

# Full optimisation, including auto-vectorisation: a SIMD bug in the byte-count
# of vector loads was hiding behind a scope reduction that silently copied 32
# bytes for a 16-byte access.
tests/guest/cprog.elf: tests/guest/cprog.c | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -O2 -ffreestanding -nostdlib -static -c $< -o $(BUILD)/cprog-guest.o
	$(LLD) -static -o $@ $(BUILD)/cprog-guest.o

test: $(TEST_BIN) $(GUEST_ELF)
	@./$(TEST_BIN)

clean:
	rm -rf $(BUILD)

# Decode-rule overlap check: needs real binaries, so it only runs when they are
# present. Catches the "one prefix, several families" bug class before it turns
# into a debugging session.
overlaps: 
	@if [ -f /tmp/alpine-root/lib/ld-musl-aarch64.so.1 ]; then \
		python3 tools/check-decode-overlaps.py \
			/tmp/alpine-root/lib/ld-musl-aarch64.so.1 /tmp/alpine-root/bin/busybox; \
	else \
		echo "skip: no reference binaries"; \
	fi

tests/guest/shll.elf: tests/guest/shll.c | $(BUILD)
	$(CLANG) --target=aarch64-linux-gnu -O2 -ffreestanding -nostdlib -static -c $< -o $(BUILD)/shll-guest.o
	$(LLD) -static -o $@ $(BUILD)/shll-guest.o

# Host harness. Running it needs a root filesystem to point at, so it is not part of
# `make test` - but building it does not, so `all` includes it.
harness: tools/rune

tools/rune: tools/rune.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) tools/rune.c -o $@ $(LDLIBS)
