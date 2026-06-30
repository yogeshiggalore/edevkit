/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_stubs/net_disable_approtect.s — Net-CORE on-target stub.
 *
 * Lives in Net flash @ 0x01000000. Runs on the Net core after a
 * CTRL-AP#3 RESET pulse. Programs Net UICR.APPROTECT to HwDisabled
 * (0x50FA50FA) so the core stays debug-accessible across subsequent
 * resets.
 *
 * Why on-target: per memory note
 * reference_nrf5340_net_uicr_write_attempts_2026_06_26, nRF5340 Net
 * peripherals are "Selectable Secure" and bare Secure-mode AHB-AP
 * writes fault until the SPU has been configured. Running the
 * programming code on the Net CPU itself sidesteps the SPU issue
 * — the Net core's secure context is set up by silicon defaults.
 *
 * Why every fault vector has a spin handler: post-erase Net flash is
 * all 0xFFFFFFFF. A vector slot containing 0xFFFFFFFE (the fetched
 * value with the Thumb bit stripped) → CPU fetches at 0xFFFFFFFE →
 * fault → tries the corresponding fault handler → also 0xFFFFFFFE
 * → double fault → LOCKUP. Symptom from earlier iterations was
 * PC=0xEFFFFFFE in DHCSR with S_LOCKUP=1. The fix is install a real
 * (spin) handler in every slot. Same pattern nrfjprog uses — see
 * reference_nrfjprog_stub_disassembly_2026_06_26.
 *
 * Host-side build (the C code embeds the resulting blob as a const
 * uint8_t array; this is the source of truth):
 *   arm-zephyr-eabi-as -mcpu=cortex-m33 -mthumb \
 *                      net_disable_approtect.s \
 *                      -o net_disable_approtect.o
 *   arm-zephyr-eabi-objcopy -O binary net_disable_approtect.o stub.bin
 *
 * SRAM progress markers written by the stub (host reads these after
 * the CTRL-AP RESET to confirm the stub ran):
 *   0x21000000  stage         0x11111111 entered → 0x22222222 Wen
 *                             acknowledged → 0xDEADC0DE complete
 *   0x21000004  0xBADF00D5    if the fault_handler fired
 *   0x21000008  faulted PC
 *   0x2100000C  faulted xPSR
 *
 * Net peripheral addresses (in Net's own address space):
 *   NVMC.READY     @ 0x41080400   (bit 0 = 1 when idle)
 *   NVMC.CONFIG    @ 0x41080504   (0=Ren, 1=Wen)
 *   UICR.APPROTECT @ 0x01FF8000   (write 0x50FA50FA = HwDisabled)
 */

    .syntax unified
    .cpu cortex-m33
    .thumb
    .section .text
    .align  2

/* ─── Vector table (16 entries; every populated slot points at a spin handler) ── */
    .global _vectors
_vectors:
    .word   _stack_top                  /* 0x00: Initial MSP */
    .word   reset_handler + 1           /* 0x04: Reset (thumb bit) */
    .word   fault_handler + 1           /* 0x08: NMI */
    .word   fault_handler + 1           /* 0x0C: HardFault */
    .word   fault_handler + 1           /* 0x10: MemManage */
    .word   fault_handler + 1           /* 0x14: BusFault */
    .word   fault_handler + 1           /* 0x18: UsageFault */
    .word   fault_handler + 1           /* 0x1C: SecureFault */
    .word   0
    .word   0
    .word   0
    .word   fault_handler + 1           /* 0x2C: SVCall */
    .word   fault_handler + 1           /* 0x30: DebugMonitor */
    .word   0
    .word   fault_handler + 1           /* 0x38: PendSV */
    .word   fault_handler + 1           /* 0x3C: SysTick */

/* ─── fault_handler — record PC + xPSR, then spin ──────────────────────── */
fault_handler:
    ldr  r0, =0xBADF00D5
    ldr  r1, =0x21000004
    str  r0, [r1]
    mrs  r0, msp
    ldr  r2, [r0, #24]              /* stacked PC */
    ldr  r1, =0x21000008
    str  r2, [r1]
    ldr  r2, [r0, #28]              /* stacked xPSR */
    ldr  r1, =0x2100000C
    str  r2, [r1]
1:  b    1b

/* ─── reset_handler — program Net UICR.APPROTECT ──────────────────────── */
.equ NET_VMC_RAMBLOCK_PSET, 0x41081604   /* first RAMBLOCK POWERSET; 0x10 stride */
.equ NET_NVMC_READY,    0x41080400
.equ NET_NVMC_CONFIG,   0x41080504
.equ NET_UICR_APPROT,   0x01FF8000
.equ UNLOCK_MAGIC,      0x50FA50FA
.equ DONE_MAGIC,        0xDEADC0DE
.equ STATE_SRAM,        0x21000000
.equ FFFF,              0xFFFFFFFF

reset_handler:
    /* Power on all 8 Net VMC RAMBLOCKs BEFORE any SRAM access. Per
     * nrfjprog stub disassembly (reference_nrfjprog_stub_disassembly_
     * 2026_06_26) the App stub does the same with App VMC at
     * 0x50081604; missing this prologue caused S_LOCKUP=1 on bench
     * nRF5340 DK silicon — first `str r1, [r0]` to STATE_SRAM faulted
     * (RAM block 0 not powered), fault_handler re-faulted on its own
     * store, double-fault → lockup. Verified 2026-06-30. */
    ldr  r0, =NET_VMC_RAMBLOCK_PSET
    ldr  r1, =FFFF
    str  r1, [r0, #0x00]
    str  r1, [r0, #0x10]
    str  r1, [r0, #0x20]
    str  r1, [r0, #0x30]
    str  r1, [r0, #0x40]
    str  r1, [r0, #0x50]
    str  r1, [r0, #0x60]
    str  r1, [r0, #0x70]

    /* marker: "entered" — now SRAM is accessible */
    ldr  r0, =STATE_SRAM
    ldr  r1, =0x11111111
    str  r1, [r0]

    /* Net's NVMC is in the Network domain — directly accessible from
       the Net CPU without SPU configuration. */

    ldr  r4, =NET_NVMC_READY
    ldr  r5, =NET_NVMC_CONFIG
    ldr  r6, =UNLOCK_MAGIC

    /* Wait READY */
1:  ldr  r2, [r4]
    cmp  r2, #1
    bne  1b

    /* CONFIG = Wen */
    movs r2, #1
    str  r2, [r5]

2:  ldr  r2, [r4]
    cmp  r2, #1
    bne  2b

    /* marker: "Wen set" */
    ldr  r0, =STATE_SRAM
    ldr  r1, =0x22222222
    str  r1, [r0]

    /* UICR.APPROTECT = 0x50FA50FA */
    ldr  r0, =NET_UICR_APPROT
    str  r6, [r0]

3:  ldr  r2, [r4]
    cmp  r2, #1
    bne  3b

    /* CONFIG = Ren */
    movs r2, #0
    str  r2, [r5]

4:  ldr  r2, [r4]
    cmp  r2, #1
    bne  4b

    /* marker: "DONE" */
    ldr  r0, =STATE_SRAM
    ldr  r1, =DONE_MAGIC
    str  r1, [r0]

    /* Spin forever — NOT BKPT (would HardFault without C_DEBUGEN set). */
5:  b    5b

    .pool

    /* On bench nRF5340 DK silicon (verified 2026-06-30), only Net
     * SRAM RAMBLOCK[0] is powered by default after CTRL-AP RESET —
     * 0x2100FFFC (top of 64 KB Net SRAM) reads FAULT from the host
     * AHB-AP. Setting MSP to 0x21010000 (top of full SRAM) causes the
     * Net CPU's first exception-entry PUSH to fault on unpowered
     * RAMBLOCK[7] → double fault → lockup (DHCSR S_LOCKUP=1).
     *
     * Fix: keep the stack inside RAMBLOCK[0] (lower 4 KB of Net SRAM).
     * 0x21001000 = 4 KB in, gives the stub ~3.95 KB of stack which
     * is overkill — actual usage is one exception frame at most. */
    .equ _stack_top, 0x21001000     /* top of RAMBLOCK[0] = 4 KB into Net SRAM */
