/* nrf53_disable_approtect.s — Thumb stub that programs UICR.APPROTECT and
 * UICR.SECUREAPPROTECT on both nRF5340 cores.
 *
 * KEY: installs a full exception vector table with spin-handlers for every
 * fault. Without this, any fault while the stub is running causes the CPU
 * to dispatch to vector slots that point to 0xFFFFFFFE (erased flash) →
 * double fault → lockup. That was the bug in earlier iterations — the
 * test chip's PC=0xEFFFFFFE after the stub timed out was lockup-after-
 * peripheral-fault, not the stub itself faulting.
 *
 * nrfjprog's 3748-byte recover image installs the same fault-handler
 * vectors (verified by reading App flash[0x00..0x40] post-recover). The
 * actual UICR work in nrfjprog is at offset 0x448; our stub puts it
 * inline right after the vectors.
 *
 * Build:
 *   arm-none-eabi-as -mcpu=cortex-m33 -mthumb nrf53_disable_approtect.s \
 *                    -o nrf53_disable_approtect.o
 *   arm-none-eabi-objcopy -O binary nrf53_disable_approtect.o stub.bin
 *
 * Loader (host side): write stub.bin to App flash @ 0x00000000 via App
 * NVMC, then SYSRESETREQ. CPU loads SP+PC from the vector table at the
 * start of our binary, runs reset_handler, programs UICRs, BKPTs.
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
    .word   0                           /* 0x20: reserved                    */
    .word   0                           /* 0x24: reserved                    */
    .word   0                           /* 0x28: reserved                    */
    .word   fault_handler + 1           /* 0x2C: SVCall                      */
    .word   fault_handler + 1           /* 0x30: DebugMonitor                */
    .word   0                           /* 0x34: reserved                    */
    .word   fault_handler + 1           /* 0x38: PendSV                      */
    .word   fault_handler + 1           /* 0x3C: SysTick                     */
    /* (External interrupts left as 0; we never enable any.)                 */

/* ─── Fault handler: BKPT so host sees we faulted, then spin ────────── */
fault_handler:
    bkpt    #1                          /* #1 = "we faulted" (vs main #0)    */
1:  b       1b

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
.equ FFFF,               0xFFFFFFFF

reset_handler:
    /* Power-on all 8 App VMC RAM blocks (POWERSET = 0xFFFFFFFF). */
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

    /* Cosmetic: clear RESETREAS. */
    ldr  r0, =RESET_RESETREAS
    str  r1, [r0]

    /* App UICR.APPROTECT + UICR.SECUREAPPROTECT ← 0x50FA50FA via App NVMC. */
    ldr  r4, =APP_NVMC_READY
    ldr  r5, =APP_NVMC_CONFIG
    ldr  r6, =UNLOCK_MAGIC

1:  ldr  r2, [r4]
    cmp  r2, #1
    bne  1b
    movs r2, #1
    str  r2, [r5]                       /* CONFIG = Wen                       */
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
    str  r2, [r5]                       /* CONFIG = Ren                       */
5:  ldr  r2, [r4]
    cmp  r2, #1
    bne  5b

    /* Release Net core. */
    ldr  r0, =NETWORK_FORCEOFF
    movs r2, #0
    str  r2, [r0]

    /* Spin a few thousand cycles for Net's clocks. */
    movs r2, #0
    ldr  r3, =0x4000
6:  adds r2, #1
    cmp  r2, r3
    blo  6b

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

    /* Halt for the debug host (BKPT #0 = success). */
    bkpt #0
11: b   11b

    .pool

/* ─── Stack at top of App SRAM ──────────────────────────────────────── */
    .equ    _stack_top, 0x20010000
