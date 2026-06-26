/* nrf53_disable_approtect.s — Thumb stub that programs UICR.APPROTECT and
 * UICR.SECUREAPPROTECT on both nRF5340 cores from the App-core debug view.
 *
 * Purpose: replicates the persistent unlock that nrfjprog --recover does
 * via its 3748-byte App-flash image. The stub is loaded into App flash at
 * 0x00000000 via App NVMC, then a SYSRESETREQ boots the CPU into our code
 * (because erased flash gives 0xFFFFFFFF reset vector → lockup, but our
 * stub-loaded vectors are valid).
 *
 * Build:
 *   arm-none-eabi-as -mcpu=cortex-m33 -mthumb nrf53_disable_approtect.s \
 *                    -o nrf53_disable_approtect.o
 *   arm-none-eabi-objcopy -O binary nrf53_disable_approtect.o \
 *                                   nrf53_disable_approtect.bin
 *   xxd -i nrf53_disable_approtect.bin > stub_blob.h
 *
 * Loader (host side):
 *   - Write 2-word vector table prefix to App flash @ 0x00000000:
 *       [0x00] = initial MSP  (top of App SRAM, 0x20010000)
 *       [0x04] = reset vector = stub_entry | 1 (Thumb bit)
 *   - Write stub code immediately after at 0x00000008
 *   - SYSRESETREQ → CPU loads SP+PC from flash, runs stub
 *   - Stub does VMC RAM power-on, then UICR.APPROTECT programming, then BKPT
 *   - Host polls DHCSR.S_HALT, reads back UICR values to verify
 *
 * Why VMC first: nRF5340 RAM blocks default to power-off; any stack access
 * (interrupts, BKPT exception entry) will HardFault unless VMC.RAMBLOCK[N]
 * .POWER is set. nrfjprog's trace shows 8 VMC writes (0x50081604/14/.../74
 * = RAMBLOCK[0..7].POWERSET) before any peripheral access. We do the same.
 *
 * Why Net needs explicit FORCEOFF release + delay: CTRL-AP#3 ERASEALL holds
 * Net core in reset (FORCEOFF=1). Writing 0 to NETWORK.FORCEOFF releases
 * it; Net's clocks then take a few cycles to stabilize before its NVMC
 * responds. The stub spin-waits a few hundred cycles after the release.
 */

    .syntax unified
    .cpu cortex-m33
    .thumb
    .text
    .align  2
    .global _start

/* ─── Constants (PC-relative literal pool at end of stub) ─────────── */
.equ VMC_BASE,           0x50081000          /* App VMC base (Secure)         */
.equ VMC_RAMBLOCK_PSET,  0x50081604          /* RAMBLOCK[0].POWERSET          */
.equ RESET_RESETREAS,    0x50005400          /* clear-all-reset-reasons       */
.equ NETWORK_FORCEOFF,   0x50005614          /* App's view of Net force-off   */
.equ APP_NVMC_READY,     0x50039400
.equ APP_NVMC_CONFIG,    0x50039504
.equ APP_UICR_APPROT,    0x00FF8000
.equ APP_UICR_SECAPPROT, 0x00FF801C
.equ NET_NVMC_READY,     0x41080400          /* Net NVMC, App's bridge view   */
.equ NET_NVMC_CONFIG,    0x41080504
.equ NET_UICR_APPROT,    0x01FF8000          /* Net UICR, App's bridge view   */
.equ UNLOCK_MAGIC,       0x50FA50FA
.equ FFFF,               0xFFFFFFFF
.equ NVMC_CONFIG_WEN,    0x00000001
.equ NVMC_CONFIG_REN,    0x00000000

_start:
    /* ──────────────────────────────────────────────────────────────
     * 1. Power on all 8 App-side VMC RAM blocks.
     *    Without this, the CPU's first exception entry (e.g. BKPT)
     *    HardFaults because the stack push hits an unpowered RAM
     *    block. nrfjprog's J-Link trace writes 0xFFFFFFFF to
     *    POWERSET for blocks [0..7] before anything else.
     * ────────────────────────────────────────────────────────────── */
    ldr  r0, =VMC_RAMBLOCK_PSET     /* r0 = 0x50081604 (RAMBLOCK[0].POWERSET) */
    ldr  r1, =FFFF                  /* r1 = 0xFFFFFFFF                        */
    str  r1, [r0, #0x00]            /* RAMBLOCK[0].POWERSET                   */
    str  r1, [r0, #0x10]            /* RAMBLOCK[1].POWERSET                   */
    str  r1, [r0, #0x20]
    str  r1, [r0, #0x30]
    str  r1, [r0, #0x40]
    str  r1, [r0, #0x50]
    str  r1, [r0, #0x60]
    str  r1, [r0, #0x70]            /* RAMBLOCK[7].POWERSET                   */

    /* ──────────────────────────────────────────────────────────────
     * 2. Clear RESETREAS (cosmetic; matches nrfjprog).
     * ────────────────────────────────────────────────────────────── */
    ldr  r0, =RESET_RESETREAS       /* r0 = 0x50005400 */
    str  r1, [r0]                   /* clear all reset reasons by writing 1s  */

    /* ──────────────────────────────────────────────────────────────
     * 3. App UICR.APPROTECT + UICR.SECUREAPPROTECT ← 0x50FA50FA
     * ────────────────────────────────────────────────────────────── */
    ldr  r4, =APP_NVMC_READY
    ldr  r5, =APP_NVMC_CONFIG
    ldr  r6, =UNLOCK_MAGIC

1:  ldr  r2, [r4]
    cmp  r2, #1
    bne  1b
    movs r2, #NVMC_CONFIG_WEN
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

    movs r2, #NVMC_CONFIG_REN
    str  r2, [r5]
5:  ldr  r2, [r4]
    cmp  r2, #1
    bne  5b

    /* ──────────────────────────────────────────────────────────────
     * 4. Release Net core (NETWORK.FORCEOFF = 0) and let it spin up.
     * ────────────────────────────────────────────────────────────── */
    ldr  r0, =NETWORK_FORCEOFF
    movs r2, #0
    str  r2, [r0]

    /* Spin a few thousand cycles to let Net's clocks stabilize. */
    movs r2, #0
    ldr  r3, =0x4000
6:  adds r2, #1
    cmp  r2, r3
    blo  6b

    /* ──────────────────────────────────────────────────────────────
     * 5. Net UICR.APPROTECT ← 0x50FA50FA via Net NVMC (App bridge).
     * ────────────────────────────────────────────────────────────── */
    ldr  r4, =NET_NVMC_READY
    ldr  r5, =NET_NVMC_CONFIG

7:  ldr  r2, [r4]
    cmp  r2, #1
    bne  7b
    movs r2, #NVMC_CONFIG_WEN
    str  r2, [r5]
8:  ldr  r2, [r4]
    cmp  r2, #1
    bne  8b

    ldr  r0, =NET_UICR_APPROT
    str  r6, [r0]
9:  ldr  r2, [r4]
    cmp  r2, #1
    bne  9b

    movs r2, #NVMC_CONFIG_REN
    str  r2, [r5]
10: ldr  r2, [r4]
    cmp  r2, #1
    bne  10b

    /* ──────────────────────────────────────────────────────────────
     * 6. Halt for the debug host.
     * ────────────────────────────────────────────────────────────── */
    bkpt #0

    /* Spin if BKPT somehow returns. */
99: b    99b

    .pool
