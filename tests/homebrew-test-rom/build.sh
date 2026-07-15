#!/usr/bin/env bash
# Compiles arm9.c/arm7.c with a bare-metal ARM cross-compiler and packs
# them into test.nds. See README.md in this directory for what this is
# for and how it was used.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

: "${CROSS_PREFIX:=arm-none-eabi-}"

"${CROSS_PREFIX}gcc" -mcpu=arm946e-s -marm -nostdlib -nostartfiles -ffreestanding -O0 \
    -Wl,--entry=_start -Wl,-Ttext=0x02004000 -o arm9.elf arm9.c
"${CROSS_PREFIX}objcopy" -O binary arm9.elf arm9.bin

"${CROSS_PREFIX}gcc" -mcpu=arm7tdmi -marm -nostdlib -nostartfiles -ffreestanding -O0 \
    -Wl,--entry=_start -Wl,-Ttext=0x02380000 -o arm7.elf arm7.c
"${CROSS_PREFIX}objcopy" -O binary arm7.elf arm7.bin

python3 build_rom.py

echo "Built test.nds. Run it with the patched melonDS, e.g.:"
echo "  MELONDS_REMOTE_ENABLE=1 ./melonDS --boot always $(pwd)/test.nds"
