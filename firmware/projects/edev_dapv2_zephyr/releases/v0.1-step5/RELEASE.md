# edev_dapv2_zephyr v0.1-step5 — release notes

**Build:** 2026-06-30
**Branch:** `feat/edev_dapv2_zephyr`
**Tip commit:** `652b9cc feat(edev_dapv2_zephyr): nRF5340 step 5 — NRF53_RECOVER (cmd 0x84)`
**Board:** `rpi_pico2/rp2350a/m33` (Raspberry Pi Pico 2)

## What's in this release

CMSIS-DAP v2 probe firmware for the RP2350 with Nordic chip recovery and
APPROTECT unlock primitives exposed as vendor commands (`0x84..0x8B`).
Reaches functional parity with the pico-sdk Python webgui for the
unlock-and-flash workflow.

### New vendor commands

| Cmd | Operation | Bytes returned |
|---|---|---|
| `0x84` | `NRF53_RECOVER`            — full 8-stage unlock (ERASEALL both cores + App UICR + Net UICR via stub) | 19 |
| `0x85` | `NRF53_ERASE`              — CTRL-AP ERASEALL all Nordic CTRL-APs found on the bus                    | 3  |
| `0x8A` | `NRF53_UICR_PROGRAM_APP`   — host-side App UICR.APPROTECT + SECUREAPPROTECT = 0x50FA50FA              | 10 |
| `0x8B` | `NRF53_UICR_PROGRAM_NET`   — on-target Net stub: programs Net UICR.APPROTECT from the Net CPU side    | 10 |

Standard CMSIS-DAP v2 commands continue to work — this is a strict superset
of the upstream Zephyr `subsys/dap` surface.

### Wire format

All vendor command responses follow CMSIS-DAP standard:
```
response[0] = command byte (echo, on success)  OR  0xFF (ID_DAP_INVALID)
response[1] = status (0 = OK, see nrf53_status_t for other codes)
response[2..] = per-command payload (little-endian)
```

For details see `docs/NRF5340_ALGORITHMS.md` (algorithm reference) and
`docs/ESP32_BRIDGE.md` Appendix G.3 (the per-command wire format).

## Artifacts

| File | Size | SHA256 | Use |
|---|---|---|---|
| `zephyr.uf2` | 144,896 B | `c40b52d44ed0f590f264f12c73ca4e9e2da8622f038278f0e54d61ecc5fbc49b` | Drag-drop flashable (BOOTSEL mode) |
| `zephyr.bin` | 72,372 B | `a430b744f774d09221a23767f1bda747c7a87ec2ad53e225001e11587e3957e3` | probe-rs / picotool raw |
| `zephyr.elf` | 2,862,920 B | `228a8025346561a2e137ffd16b8fa5b02b6b6ec304e36ee59e2ecae84b235c98` | GDB debugging |

Memory footprint: 72,372 B FLASH (1.73%) / 24,364 B RAM (4.58%) on RP2350.

## How to flash

### Option A — UF2 drag-drop (no extra tools needed)

1. Hold the BOOTSEL button on the Pico, plug in USB.
2. The Pico mounts as a `RPI-RP2` USB mass-storage device.
3. Drag `zephyr.uf2` onto it.
4. The Pico reboots into the new firmware.

### Option B — 1200-baud trick (no button hold needed)

Run from any host:
```bash
stty -f /dev/cu.usbmodem*  1200          # macOS
stty -F /dev/ttyACM0       1200          # Linux
# Pico re-enumerates as RPI-RP2; drag the uf2.
```

### Option C — probe-rs (when you have another debug probe attached)

```bash
probe-rs download --chip RP2350 --binary-format uf2 zephyr.uf2
```

## How to test against hardware

1. Flash the firmware (Option A/B/C above).
2. Connect SWD wires from the Pico to a Nordic target:
   - GPIO2 (Pico) → SWCLK (target)
   - GPIO3 (Pico) → SWDIO (target)
   - GND ← → GND
   - Power the target (USB / battery — keep it on charger so the debug
     domain doesn't power down mid-test).
3. Run the host-side test harness:
   ```bash
   pip install pyusb
   python3 ../tools/test_nrf53_vendor_cmds.py
   ```

   Expected output on a working nRF5340 target:
   ```
   Probe: VID:PID 2e8a:000c  bus=… addr=…
     bulk OUT EP=0x01  IN EP=0x82

   === NRF53_ERASE (0x85) — CTRL-AP ERASEALL ===
     raw: 85 00 02
     status=0 (OK)  ap_count=2
   PASS  (2 CTRL-AP(s) erased)

   … etc for each test, ending with the summary line ✓✓✓✓✓
   ```

   On nRF52840 expect `ap_count=1` (only the App CTRL-AP exists), and
   the `uicr-net` / `recover` tests can be skipped (or they'll FAIL —
   Net is nRF5340-specific).

4. The full `RECOVER` test (cmd `0x84`) is destructive — it wipes the
   target's flash. Don't run against hardware whose firmware you want
   to keep.

## Known limitations (still-pending steps)

This release stops at step 5 of the 10-step plan in
`docs/NRF5340_ALGORITHMS.md` §9. Still to come:

- Step 6 — `NRF53_FLASH_WRITE_NET` (0x86): probe-side Net flash
  programming. Today the host has to drive Net flash writes itself
  via standard `DAP_Transfer` after `RECOVER`.
- Step 7 — `NRF53_FLASH_WRITE_APP` (0x87): App flash via RAM-loaded
  CMSIS Flash Algorithm.
- Step 8 — `NRF53_READ_MEM` (0x88) with `DEMCR.VC_CORERESET` clear.
  Fixes the App-readback-0xFF post-reset bug (Bug 6a in the
  algorithm doc).
- Step 9 — progress notifications for long ops (fix for the WAIT-ACK
  vs USB transfer-timeout bug, 6c).
- Step 10 — end-to-end smoke test on Ring Pro 351 hardware with
  md5-match against `uhocd dump-flash`.

For App+Net flash writes today (using this firmware): do `RECOVER`,
then use any standard CMSIS-DAP host tool (pyocd, probe-rs, uhocd) to
write the merged hex via `DAP_Transfer`. The unlock the chip received
during recover persists across reset.

## What the ESP32 bridge author needs from this release

This firmware is the target for the ESP32-S3 USB-host BLE bridge
described in `docs/ESP32_BRIDGE.md`. Key facts for the bridge:

- USB VID = 0x2E8A, PID = 0x000C, bcdDevice = 0x0220
- Vendor (0xFF) interface with bulk OUT + bulk IN endpoints, 512 B MPS
- DAP_Info reports the standard caps + identity strings
- Vendor commands `0x84..0x8B` are the high-level operations the bridge
  forwards as opaque packets (one bulk write + one bulk read, no
  intermediate orchestration)

See `docs/ESP32_BRIDGE.md` Appendix G for the full quirks list.

## Build provenance

Built with:
- Zephyr 4.4.1 at `/Users/yogesh/projects/temp/zephyrproject/zephyr`
- Zephyr SDK 1.0.1 (arm-zephyr-eabi-gcc)
- Three vendored patches applied (see `zephyr-patches/`):
  - `0001-swdp_ll_pin-add-RP2040-RP2350-elif.patch`
  - `0002-swdp_ll_pin_rpi_pico-fast-SIO-bit-bang.patch`
  - `0003-cmsis_dap-weak-vendor-cmd-handler.patch` (new in this release)

To reproduce:
```bash
cd firmware/projects/edev_dapv2_zephyr
../../zephyr-patches/apply.sh                # idempotent
west build -b rpi_pico2/rp2350a/m33 --pristine
# build/zephyr/zephyr.uf2 should match SHA256SUMS in this dir
```
