#!/bin/sh
# Build the M1 test program with the PS2SDK toolchain in its container, using
# the same flags as the SDK's own Makefile.eeglobal (the container has no make).
# Usage: tests/m1/build.sh [outdir]      (default: build/m1)
# Then:  cmake -B build -DPS2_TEST_PROGRAM=build/m1/m1.elf && ninja -C build
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/build/m1}
mkdir -p "$out"
out=$(cd "$out" && pwd)
docker run --rm -u "$(id -u):$(id -g)" \
    -v "$here:/src:ro" -v "$out:/out" -w /out \
    ps2dev/ps2dev:latest \
    sh -euc '
        CC=mips64r5900el-ps2-elf-gcc
        $CC -D_EE -G0 -O2 -Wall -I$PS2SDK/ee/include -I$PS2SDK/common/include \
            -c /src/main.c -o main.o
        $CC -T$PS2SDK/ee/startup/linkfile -O2 -o m1.debug.elf main.o \
            -L$PS2SDK/ee/lib -Wl,-zmax-page-size=128
        # The stored copy carries no debug sections: the whole file crosses
        # the SIF at boot. m1.debug.elf keeps them for a debugger.
        mips64r5900el-ps2-elf-strip --strip-debug -o m1.elf m1.debug.elf
        ls -l m1.elf m1.debug.elf'
