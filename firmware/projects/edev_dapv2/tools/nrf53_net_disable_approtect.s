/* nrf53_net_disable_approtect.s — Net-CORE stub.
 *
 * Lives in Net flash @ 0x01000000. Runs on the Net core after a CTRL-AP
 * RESET. Programs Net UICR.APPROTECT to HwDisabled (0x50FA50FA) so the
 * core stays debug-accessible across subsequent resets.
 *
 * Separate from the App-side stub (nrf53_disable_approtect.s) — each
 * core's stub runs on its OWN cpu, using its OWN NVMC and UICR. nrfjprog
 * works this way; trying to do it all from App + cross-bridge access
 * faulted at PC=0xEE because the SPU bootstrap is more involved.
 *
 * Build:
 *   arm-none-eabi-as -mcpu=cortex-m33 -mthumb \
 *                    nrf53_net_disable_approtect.s \
 *                    -o nrf53_net_disable_approtect.o
 *   arm-none-eabi-objcopy -O binary nrf53_net_disable_approtect.o stub.bin
 *
 * Net's NVMC and UICR addresses (in NET's address space — same as App's
 * view via the bridge, since the bridge is transparent):
 *   NVMC.READY    @ 0x41080400  (bit 0 = 1 when idle)
 *   NVMC.CONFIG   @ 0x41080504  (0=Ren, 1=Wen, 2=Een)
 *   UICR.APPROTECT @ 0x01FF8000  (write 0x50FA50FA = HwDisabled)
 *
 * Net core: Cortex-M33, has secure-only access (no TrustZone), no SPU,
 * no separate cache. Much simpler than App side.
 */

    .syntax unified
    .cpu cortex-m33
    .thumb
    .section .text
    .align  2

/* ─── Vector table (16 entries, all exception handlers spin) ───────── */
    .global _vectors
_vectors:
    .word   _stack_top                  /* 0x00: Initial MSP                  */
    .word   reset_handler + 1           /* 0x04: Reset                        */
    .word   fault_handler + 1           /* 0x08: NMI                          */
    .word   fault_handler + 1           /* 0x0C: HardFault                    */
    .word   fault_handler + 1           /* 0x10: MemManage                    */
    .word   fault_handler + 1           /* 0x14: BusFault                     */
    .word   fault_handler + 1           /* 0x18: UsageFault                   */
    .word   fault_handler + 1           /* 0x1C: SecureFault (Net has no TZ
                                                 but slot still defined)      */
    .word   0
    .word   0
    .word   0
    .word   fault_handler + 1           /* 0x2C: SVCall                       */
    .word   fault_handler + 1           /* 0x30: DebugMonitor                 */
    .word   0
    .word   fault_handler + 1           /* 0x38: PendSV                       */
    .word   fault_handler + 1           /* 0x3C: SysTick                      */

/* ─── Fault handler — stamp fault info in Net RAM, then spin ────────── */
/*  Net RAM lives at 0x21000000..0x21010000 (64 KB)
 *  We stamp diagnostics at 0x21000000:
 *    0x21000000  stage progress (set by reset_handler at milestones)
 *    0x21000004  0xBADF00D5  (fault sentinel)
 *    0x21000008  stacked PC at fault
 *    0x2100000C  stacked xPSR at fault
 */
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

/* ─── Reset handler — program Net UICR.APPROTECT ────────────────────── */
.equ NET_NVMC_READY,    0x41080400
.equ NET_NVMC_CONFIG,   0x41080504
.equ NET_UICR_APPROT,   0x01FF8000
.equ UNLOCK_MAGIC,      0x50FA50FA
.equ DONE_MAGIC,        0xDEADC0DE
.equ STATE_SRAM,        0x21000000

reset_handler:
    /* Stage: "started" */
    ldr  r0, =STATE_SRAM
    ldr  r1, =0x11111111
    str  r1, [r0]

    /* Net's NVMC is in the Network domain — directly accessible without
       SPU configuration (we're on the Net CPU itself; no cross-domain). */

    /* Wait NVMC.READY = 1 */
    ldr  r4, =NET_NVMC_READY
    ldr  r5, =NET_NVMC_CONFIG
    ldr  r6, =UNLOCK_MAGIC

1:  ldr  r2, [r4]
    cmp  r2, #1
    bne  1b

    /* CONFIG = Wen */
    movs r2, #1
    str  r2, [r5]

2:  ldr  r2, [r4]
    cmp  r2, #1
    bne  2b

    /* Stage: "Wen set" */
    ldr  r0, =STATE_SRAM
    ldr  r1, =0x22222222
    str  r1, [r0]

    /* Write UICR.APPROTECT = 0x50FA50FA */
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

    /* Stage: "DONE" */
    ldr  r0, =STATE_SRAM
    ldr  r1, =DONE_MAGIC
    str  r1, [r0]

    /* Spin forever — no BKPT (would HardFault without C_DEBUGEN). */
5:  b    5b

    .pool

    .equ _stack_top, 0x21010000     /* top of Net RAM */
