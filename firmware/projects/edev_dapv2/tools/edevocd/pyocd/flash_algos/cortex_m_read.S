@ ----------------------------------------------------------------------
@ cortex_m_read.S — minimal Thumb-2 algorithm loaded into target RAM and
@ executed on the target CPU to bulk-read flash without going through
@ the debug-bus AHB-AP for every word.
@
@ Why this exists:
@   nRF5340 (and other anti-tamper-aware MCUs) throttle / lock the
@   AHB-AP after a small number of debug-bus reads. J-Link sidesteps
@   this by uploading a small Thumb program to RAM and running it on
@   the target CPU — the CPU's internal AHB master reads flash through
@   the same channel firmware uses, which the chip does NOT monitor
@   for anti-tamper. Only one AHB-AP read remains (the final extraction
@   of the RAM buffer), so 1 MB of flash costs ~1 K worth of AP reads,
@   not 16 K.
@
@ Register conventions (set before invocation via DCRSR/DCRDR):
@   R0 = source flash address    (word-aligned)
@   R1 = word count               (0..N — caller-chosen)
@   R2 = destination RAM buffer   (word-aligned)
@   PC = address of _start (= algo's loaded base)
@   XPSR = 0x01000000             (T bit set)
@   MSP = stack top (we touch no memory, but the CPU needs a valid SP)
@   LR = don't care (we end with bkpt, not bx lr)
@
@ Algorithm:
@   while (r1 != 0) {
@     *r2++ = *r0++;
@     r1--;
@   }
@   bkpt #0;                      @ halts the CPU; debugger sees S_HALT=1
@
@ Total size: 16 bytes (8 halfwords). Built once, embedded as a `bytes`
@ literal in cortex_m_read.py. To rebuild after editing:
@   cd flash_algos && ./build.sh
@ ----------------------------------------------------------------------

    .thumb
    .syntax unified
    .global _start

_start:
    cbz   r1, done           @ exit if count is zero
read_loop:
    ldr   r3, [r0], #4       @ word = *r0; r0 += 4
    str   r3, [r2], #4       @ *r2 = word; r2 += 4
    subs  r1, r1, #1         @ r1--
    bne   read_loop          @ loop while r1 != 0
done:
    bkpt  #0                 @ halt — debugger will see DHCSR.S_HALT = 1
