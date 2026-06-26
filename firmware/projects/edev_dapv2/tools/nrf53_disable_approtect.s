/* nrf53_disable_approtect.s — Thumb stub that programs UICR.APPROTECT and
 * UICR.SECUREAPPROTECT on both nRF5340 cores.
 *
 * Key design points (learned by disassembling nrfjprog + iterating):
 *   - Full vector table with spin-handlers for every fault slot. Without
 *     these, any fault → vector at 0xFFFFFFFE → lockup.
 *   - NO BKPT instructions anywhere. After CTRL-AP RESET launches the
 *     stub, C_DEBUGEN is clear; BKPT then escalates to HardFault and
 *     a HardFault that double-faults causes lockup.
 *   - Magic-value-to-SRAM at end of stub. Host reads 0x20000000 in a
 *     fresh debug session to confirm the stub ran to completion.
 *
 * Build:
 *   arm-none-eabi-as -mcpu=cortex-m33 -mthumb nrf53_disable_approtect.s \
 *                    -o nrf53_disable_approtect.o
 *   arm-none-eabi-objcopy -O binary nrf53_disable_approtect.o stub.bin
 *
 * Loader (host side): write stub.bin verbatim to App flash @ 0x00000000
 * via App NVMC, then CTRL-AP#2 RESET pulse. CPU boots from our vectors,
 * runs reset_handler, programs both UICRs, writes 0xDEADC0DE to SRAM,
 * spins. Host opens fresh pyocd session, reads 0x20000000 (expect
 * 0xDEADC0DE), reads UICR.APPROTECT cells (expect 0x50FA50FA).
 */

    .syntax unified
    .cpu cortex-m33
    .thumb
    .section .text
    .align  2

/* ─── Vector table ─────────────────────────────────────────────────── */
    .global _vectors
_vectors:
    .word   _stack_top                  /* 0x00: Initial MSP                 */
    .word   reset_handler + 1           /* 0x04: Reset                       */
    .word   fault_handler + 1           /* 0x08: NMI                         */
    .word   fault_handler + 1           /* 0x0C: HardFault                   */
    .word   fault_handler + 1           /* 0x10: MemManage                   */
    .word   fault_handler + 1           /* 0x14: BusFault                    */
    .word   fault_handler + 1           /* 0x18: UsageFault                  */
    .word   fault_handler + 1           /* 0x1C: SecureFault                 */
    .word   0
    .word   0
    .word   0
    .word   fault_handler + 1           /* 0x2C: SVCall                      */
    .word   fault_handler + 1           /* 0x30: DebugMonitor                */
    .word   0
    .word   fault_handler + 1           /* 0x38: PendSV                      */
    .word   fault_handler + 1           /* 0x3C: SysTick                     */

/* ─── Fault handler: stamp fault PC into SRAM, then spin ───────────── */
/* Layout @ 0x20000000+:
 *   0x20000000  done_magic  (0xDEADC0DE if stub completed)
 *   0x20000004  fault_lr    (LR at time of fault — points near offender)
 */
fault_handler:
    /* r4 must be preserved between exception calls. Use scratch r0-r3. */
    ldr  r0, =0x20000004        /* fault_lr slot                              */
    mov  r1, lr
    str  r1, [r0]               /* record LR (return PC into faulting code)   */
    /* Record fault as a special magic (not 0xDEADC0DE).                       */
    ldr  r0, =0x20000000
    ldr  r1, =0xBADF00D5
    str  r1, [r0]
1:  b    1b

/* ─── Reset handler / main work ────────────────────────────────────── */
.equ VMC_RAMBLOCK_PSET,  0x50081604
.equ RESET_RESETREAS,    0x50005400
.equ NETWORK_FORCEOFF,   0x50005614
.equ APP_NVMC_READY,     0x50039400
.equ APP_NVMC_CONFIG,    0x50039504
.equ APP_UICR_APPROT,    0x00FF8000
.equ APP_UICR_SECAPPROT, 0x00FF801C
.equ NET_NVMC_READY,     0x41080400
.equ NET_NVMC_CONFIG,    0x41080504
.equ NET_UICR_APPROT,    0x01FF8000
.equ UNLOCK_MAGIC,       0x50FA50FA
.equ DONE_MAGIC,         0xDEADC0DE
.equ STATE_SRAM,         0x20000000
.equ FFFF,               0xFFFFFFFF

reset_handler:
    /* Initialize the state slot to a sentinel so the host can tell
       the stub started.  */
    ldr  r0, =STATE_SRAM
    ldr  r1, =0x11111111
    str  r1, [r0]

    /* Power on all 8 App VMC RAM blocks (POWERSET = 0xFFFFFFFF). */
    ldr  r0, =VMC_RAMBLOCK_PSET
    ldr  r1, =FFFF
    str  r1, [r0, #0x00]
    str  r1, [r0, #0x10]
    str  r1, [r0, #0x20]
    str  r1, [r0, #0x30]
    str  r1, [r0, #0x40]
    str  r1, [r0, #0x50]
    str  r1, [r0, #0x60]
    str  r1, [r0, #0x70]

    /* State: "VMC done". */
    ldr  r0, =STATE_SRAM
    ldr  r1, =0x22222222
    str  r1, [r0]

    /* Cosmetic: clear RESETREAS. */
    ldr  r0, =RESET_RESETREAS
    ldr  r1, =FFFF
    str  r1, [r0]

    /* App UICR.APPROTECT + UICR.SECUREAPPROTECT ← 0x50FA50FA via App NVMC. */
    ldr  r4, =APP_NVMC_READY
    ldr  r5, =APP_NVMC_CONFIG
    ldr  r6, =UNLOCK_MAGIC

1:  ldr  r2, [r4]
    cmp  r2, #1
    bne  1b
    movs r2, #1
    str  r2, [r5]
2:  ldr  r2, [r4]
    cmp  r2, #1
    bne  2b

    ldr  r0, =APP_UICR_APPROT
    str  r6, [r0]
3:  ldr  r2, [r4]
    cmp  r2, #1
    bne  3b

    ldr  r0, =APP_UICR_SECAPPROT
    str  r6, [r0]
4:  ldr  r2, [r4]
    cmp  r2, #1
    bne  4b

    movs r2, #0
    str  r2, [r5]
5:  ldr  r2, [r4]
    cmp  r2, #1
    bne  5b

    /* State: "App UICR done". */
    ldr  r0, =STATE_SRAM
    ldr  r1, =0x33333333
    str  r1, [r0]

    /* Release Net core. */
    ldr  r0, =NETWORK_FORCEOFF
    movs r2, #0
    str  r2, [r0]

    /* Spin a few thousand cycles for Net's clocks to settle. */
    movs r2, #0
    ldr  r3, =0x8000
6:  adds r2, #1
    cmp  r2, r3
    blo  6b

    /* State: "Net release done". */
    ldr  r0, =STATE_SRAM
    ldr  r1, =0x44444444
    str  r1, [r0]

    /* Net UICR.APPROTECT ← 0x50FA50FA via Net NVMC. */
    ldr  r4, =NET_NVMC_READY
    ldr  r5, =NET_NVMC_CONFIG

7:  ldr  r2, [r4]
    cmp  r2, #1
    bne  7b
    movs r2, #1
    str  r2, [r5]
8:  ldr  r2, [r4]
    cmp  r2, #1
    bne  8b

    ldr  r0, =NET_UICR_APPROT
    str  r6, [r0]
9:  ldr  r2, [r4]
    cmp  r2, #1
    bne  9b

    movs r2, #0
    str  r2, [r5]
10: ldr  r2, [r4]
    cmp  r2, #1
    bne  10b

    /* State: "DONE". Host reads this to confirm full execution. */
    ldr  r0, =STATE_SRAM
    ldr  r1, =DONE_MAGIC
    str  r1, [r0]

    /* Spin forever — NO BKPT. Without C_DEBUGEN, BKPT escalates to
       HardFault and double-faults into lockup. A plain branch loop
       is harmless. */
11: b   11b

    .pool

    .equ    _stack_top, 0x20010000
