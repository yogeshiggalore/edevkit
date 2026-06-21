#!/bin/bash
# Build the flash-read Thumb-2 algorithm.
#
# Output:
#   cortex_m_read.bin   — raw bytes (16 bytes for the current algo)
#   cortex_m_read.lst   — disassembly listing for verification
#
# After editing cortex_m_read.S, run this script then re-embed
# cortex_m_read.bin into cortex_m_read.py as the ALGO_THUMB bytes.

set -euo pipefail
cd "$(dirname "$0")"

AS=arm-none-eabi-as
OBJCOPY=arm-none-eabi-objcopy
OBJDUMP=arm-none-eabi-objdump

$AS -mcpu=cortex-m33 -mthumb cortex_m_read.S -o cortex_m_read.o
$OBJCOPY -O binary cortex_m_read.o cortex_m_read.bin
$OBJDUMP -d cortex_m_read.o > cortex_m_read.lst

echo "─── disassembly ─────────────"
cat cortex_m_read.lst
echo "─── raw bytes ───────────────"
xxd cortex_m_read.bin

# Emit the Python byte literal for paste into cortex_m_read.py.
echo "─── python literal ─────────"
python3 - <<EOF
with open('cortex_m_read.bin', 'rb') as f:
    b = f.read()
print(f'# {len(b)} bytes')
print('ALGO_THUMB = bytes([')
for i in range(0, len(b), 8):
    row = ', '.join(f'0x{x:02x}' for x in b[i:i+8])
    print(f'    {row},')
print('])')
EOF
