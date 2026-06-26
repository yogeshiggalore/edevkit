/* nrf53_disable_approtect.s — Thumb stub that programs UICR.APPROTECT and
 * UICR.SECUREAPPROTECT on both nRF5340 cores from the App-core debug view.
 *
 * Purpose: replicates the persistent unlock that nrfjprog --recover does
 * via its 3748-byte App-flash image. Run this via App AHB-AP after a
 * CTRL-AP ERASEALL on both cores, while AHB-AP is still open from the
 * just-completed erase.
 *
 * Why a stub: writing Net UICR via Net AHB-AP from the host runs into
 * a chip-side state limit (Net AHB-AP reads work, writes FAULT or
 * WAIT-storm). Programming from on-chip code is the canonical Nordic
 * approach — both cores' peripherals are reachable in App's address
 * space, so a single App-core stub covers both UICRs.
 *
 * Build:
 *   arm-none-eabi-as -mcpu=cortex-m33 -mthumb nrf53_disable_approtect.s \
 *                    -o nrf53_disable_approtect.o
 *   arm-none-eabi-objcopy -O binary nrf53_disable_approtect.o \
 *                                   nrf53_disable_approtect.bin
 *   xxd -i nrf53_disable_approtect.bin > stub_blob.h
 *
 * Loader (host side):
 *   - Load .bin to App SRAM at 0x20000000 (via App AHB-AP, CSW=0x23000002,
 *     autoincrement)
 *   - Halt App core via DHCSR @ 0xE000EDF0 = 0xA05F0003
 *   - Set R13 (MSP) = 0x20010000 (top of stub's SRAM block)
 *   - Set R15 (PC)  = 0x20000001 (Thumb bit set)
 *   - DEMCR @ 0xE000EDFC bit 0 = 1 (VC_CORERESET — halt on next reset)
 *   - Run via DHCSR = 0xA05F0001
 *   - Poll DHCSR.S_HALT bit 17 = 1 (halted at BKPT)
 *   - Read UICR values via AHB-AP to verify (App UICR @ 0x00FF8000,
 *     Net UICR @ 0x01FF8000)
 *
 * Sequence per Nordic NVMC spec:
 *   1. Wait NVMC.READY = 1
 *   2. Set NVMC.CONFIG = 1 (Wen — write-enable)
 *   3. Wait NVMC.READY = 1
 *   4. Store value at UICR address (memory-mapped write)
 *   5. Wait NVMC.READY = 1
 *   6. Repeat for each UICR cell
 *   7. Set NVMC.CONFIG = 0 (Ren — read-enable)
 *
 * Addresses (App-core view):
 *   App NVMC base   = 0x50039000   READY @ +0x400   CONFIG @ +0x504
 *   App UICR base   = 0x00FF8000   APPROTECT @ +0x000   SECUREAPPROTECT @ +0x01C
 *   Net NVMC base   = 0x41080000   READY @ +0x400   CONFIG @ +0x504
 *   Net UICR base   = 0x01FF8000   APPROTECT @ +0x000
 *   NRF_RESET.NETWORK.FORCEOFF = 0x50005614 (must be 0 for Net peripherals)
 *
 * Magic value: 0x50FA50FA (HwDisabled — permanent unlock on nRF5340).
 */

    .syntax unified
    .cpu cortex-m33
    .thumb
    .text
    .align  2
    .global _start

/* ─── Constants (PC-relative literal pool at end of stub) ─────────── */
.equ APP_NVMC_READY,    0x50039400
.equ APP_NVMC_CONFIG,   0x50039504
.equ APP_UICR_APPROT,   0x00FF8000
.equ APP_UICR_SECAPPROT, 0x00FF801C
.equ NET_NVMC_READY,    0x41080400
.equ NET_NVMC_CONFIG,   0x41080504
.equ NET_UICR_APPROT,   0x01FF8000
.equ NRF_NET_FORCEOFF,  0x50005614
.equ UNLOCK_MAGIC,      0x50FA50FA
.equ NVMC_CONFIG_WEN,   0x00000001
.equ NVMC_CONFIG_REN,   0x00000000

_start:
    /* ── 1. Release Net core from FORCEOFF so Net NVMC clocks. ── */
    ldr  r0, =NRF_NET_FORCEOFF
    movs r1, #0
    str  r1, [r0]

    /* ── 2. App UICR.APPROTECT = 0x50FA50FA via App NVMC ── */
    ldr  r4, =APP_NVMC_READY
    ldr  r5, =APP_NVMC_CONFIG
    ldr  r6, =UNLOCK_MAGIC

    /* Wait NVMC.READY */
1:  ldr  r2, [r4]
    cmp  r2, #1
    bne  1b

    /* NVMC.CONFIG = Wen */
    movs r2, #NVMC_CONFIG_WEN
    str  r2, [r5]

2:  ldr  r2, [r4]
    cmp  r2, #1
    bne  2b

    /* Write App UICR.APPROTECT */
    ldr  r0, =APP_UICR_APPROT
    str  r6, [r0]

3:  ldr  r2, [r4]
    cmp  r2, #1
    bne  3b

    /* Write App UICR.SECUREAPPROTECT */
    ldr  r0, =APP_UICR_SECAPPROT
    str  r6, [r0]

4:  ldr  r2, [r4]
    cmp  r2, #1
    bne  4b

    /* NVMC.CONFIG = Ren */
    movs r2, #NVMC_CONFIG_REN
    str  r2, [r5]

5:  ldr  r2, [r4]
    cmp  r2, #1
    bne  5b

    /* ── 3. Net UICR.APPROTECT = 0x50FA50FA via Net NVMC ── */
    ldr  r4, =NET_NVMC_READY
    ldr  r5, =NET_NVMC_CONFIG

6:  ldr  r2, [r4]
    cmp  r2, #1
    bne  6b

    movs r2, #NVMC_CONFIG_WEN
    str  r2, [r5]

7:  ldr  r2, [r4]
    cmp  r2, #1
    bne  7b

    ldr  r0, =NET_UICR_APPROT
    str  r6, [r0]

8:  ldr  r2, [r4]
    cmp  r2, #1
    bne  8b

    movs r2, #NVMC_CONFIG_REN
    str  r2, [r5]

9:  ldr  r2, [r4]
    cmp  r2, #1
    bne  9b

    /* ── 4. Halt for the debug host. ── */
    bkpt #0

    /* ── Literal pool ── */
    .pool
