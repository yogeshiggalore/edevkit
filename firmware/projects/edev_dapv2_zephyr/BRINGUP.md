# BRINGUP — edev_dapv2 (Zephyr port) first-power-on

Hardware: **Raspberry Pi Pico 2** (RP2350A, Cortex-M33 cores). No add-on board needed for this checkout — just the bare Pico 2 and a USB-C cable.

Optional: a second nRF52840 / nRF5340 / Pico 2 target board wired to GPIO2 (SWCLK) + GPIO3 (SWDIO) for the SWD-side validation in step 5.

---

## 1. Build

```bash
cd firmware/projects/edev_dapv2_zephyr

export ZEPHYR_BASE=/Users/yogesh/projects/temp/zephyrproject/zephyr
export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-1.0.1
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

west build -b rpi_pico2/rp2350a/m33 --pristine
```

Expected: clean build, `build/zephyr/zephyr.uf2` ≈ 130 KB, no warnings beyond Kconfig `EXPERIMENTAL` notes.

If pioasm is not on `$PATH` or at `~/.pico-sdk/tools/.../pioasm`, the build falls back to the checked-in `pio/probe_swd.pio.h` and prints a warning. To force fresh assembly: `PIOASM=/path/to/pioasm west build --pristine`.

## 2. Flash via BOOTSEL

1. Hold the **BOOTSEL** button on the Pico 2 and plug in USB.
2. The board enumerates as a USB mass-storage device labelled `RP2350`.
3. Copy the UF2:
   ```bash
   cp build/zephyr/zephyr.uf2 /Volumes/RP2350/
   ```
4. The board reboots automatically into the new firmware. The on-board LED (GPIO25) should start blinking at 1 Hz — first sign of life.

## 3. Verify USB enumeration

The probe should expose **two USB interfaces**: CMSIS-DAP v2 (vendor 0xFF, bulk IN/OUT) + CDC ACM (debug log).

### macOS / Linux

```bash
system_profiler SPUSBDataType | grep -A 12 "edev_dapv2"   # macOS
lsusb -v -d 2e8a:000c                                      # Linux
```

Expected:
| Field | Expected value |
|---|---|
| `idVendor` | `0x2e8a` (Raspberry Pi Trading) |
| `idProduct` | `0x000c` (matches pico-sdk firmware so host allowlists keep working) |
| `bcdDevice` | `>= 0x0220` ← **probe-rs gate; must be ≥ this** |
| `iManufacturer` | `Edevkit` |
| `iProduct` | `edev_dapv2 (Zephyr)` |
| `iSerialNumber` | hex chip-id (from Zephyr `hwinfo`) |
| Interface 0 | class `0xff`, subclass `0x00`, protocol `0x00` (vendor / CMSIS-DAP) |
| Interface 1/2 | CDC ACM (one ACM channel) |

### Windows

The MS OS 2.0 BOS chain advertises `{CDB3B5AD-293B-4663-AA36-1AAE46463776}` (the standard CMSIS-DAP v2 device-interface GUID). Windows should auto-bind WINUSB without any `.inf` file. Confirm via Device Manager → the CMSIS-DAP interface should show with no yellow exclamation. If the GUID is wrong, the interface will show as unknown.

## 4. Verify CDC console

Open a serial monitor on the new CDC ACM port (`/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on Linux, `COMn` on Windows). Baud rate doesn't matter — CDC ACM ignores it.

Expected on connect (after DTR assertion):

```
[00:00:00.123,000] <inf> edev_dapv2: edev_dapv2 (Zephyr port) — boot ok
[00:00:00.123,001] <inf> edev_dapv2: Built: Jun 29 2026 14:32:18
[00:00:00.123,002] <inf> swdp_pio_rpi: SWDP-PIO ready on pio1 sm=0 offset=0
```

If the SWDP-PIO line doesn't appear, the PIO driver init failed — most likely cause is that `&pio1` in the overlay is not `status = "okay"`. Re-check the board overlay.

## 5. Verify SWD with probe-rs (no target needed)

```bash
probe-rs list
```

Expected: one entry, e.g.

```
The following debug probes were found:
[0]: edev_dapv2 (Zephyr) -- 2e8a:000c:<chip-id-hex> (CMSIS-DAP)
```

If `probe-rs list` shows nothing despite `lsusb` finding the device, the most common causes are:
1. **bcdDevice < 0x0220** — re-check `usbd_device_set_bcd_device()` ran successfully (look at CDC log).
2. **MS OS 2.0 BOS missing on Windows** — `usbd_add_descriptor(ctx, &bos_vreq_msosv2)` returned an error.
3. **Wrong interface class** — `subsys/dap/dap_backend_usb.c` should advertise class 0xFF; if Zephyr is reporting CDC-ACM as interface 0, the IAD wrap is broken.

## 6. End-to-end SWD test (target required)

With an SWD-equipped target wired to GPIO2 (SWCLK) and GPIO3 (SWDIO), plus GND common, run:

```bash
probe-rs info --probe 2e8a:000c
```

Expected for a Pico 2 target: `ARM Chip with debug port: ... ARMv8-M`. For nRF52840: DPIDR `0x2ba01477`. For nRF5340: App AP-0 + Net AP-1 + CTRL-AP-4 enumerated.

Performance sanity check — read 1 MB from an nRF52840:

```bash
probe-rs read --chip nRF52840_xxAA b32 0x0 262144 > /tmp/zephyr_dump.bin
md5 /tmp/zephyr_dump.bin
# compare to the pico-sdk firmware's dump on the same chip — should match byte-for-byte
```

Expected timing: **~16 seconds** for 1 MB (same as the pico-sdk PIO speed). If it takes 60+ seconds, the PIO clock divider is wrong or the SM is wedging between transactions — check `probe-rs read --probe 2e8a:000c -v` output for retry warnings.

## 7. Side-by-side with the pico-sdk firmware

To validate parity, keep both probes plugged in and run the same `probe-rs info` against each — they should return identical chip IDs and ROM table contents.

```bash
# Filter by serial number (shown in `probe-rs list`)
probe-rs info --probe 2e8a:000c:<pico-sdk-serial>   > /tmp/picosdk.txt
probe-rs info --probe 2e8a:000c:<zephyr-serial>     > /tmp/zephyr.txt
diff /tmp/picosdk.txt /tmp/zephyr.txt
```

Differences in the "Probe ID" line are expected (different firmware build). Differences anywhere else in the ROM walk indicate a SWD-level regression that needs to be tracked to one of the five fixes documented in NOTES.md.

## 8. Recovery — if the board enumerates but won't accept SWD

The Zephyr firmware is **not yet** validated against a hung-state target. The pico-sdk firmware has the auto-BOOTSEL HardFault recovery and the 1200-baud CDC trick that reboots into BOOTSEL on demand. Neither is ported yet. To recover from a wedged Zephyr firmware:

1. Hold BOOTSEL, plug in USB → board re-enters mass-storage mode.
2. Drag-drop a known-good UF2 (the previous Zephyr build, or the pico-sdk `edev_dapv2.uf2`).

A future task ports the auto-BOOTSEL recovery + 1200-baud trick. For first-power-on of this Zephyr port, the BOOTSEL button is the recovery path.

---

## What this BRINGUP does NOT cover

- Pico 2 W (Wi-Fi/Bluetooth chip variant) — bring-up assumes plain Pico 2. CYW43 init is not in this firmware.
- nRF5340 recover/flash flow — the host-side webgui (`firmware/projects/edev_dapv2/tools/webgui/`) talks to the pico-sdk firmware. The Zephyr port should be USB-protocol-compatible (same VID/PID, same DAP commands) so the webgui should "just work", but this has not been tested.
- Performance with the SWCLK keep-alive feature — neither port has it. See NOTES.md.

**Sign-off criteria** for this port: steps 1-6 pass with the same byte-for-byte md5 as the pico-sdk firmware on the same target chip.
