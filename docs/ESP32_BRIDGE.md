# ESP32 ⇄ Pico 2 W BLE bridge — implementation guide

> Detailed implementation guide for the ESP32 firmware that turns a Pico 2 W
> running edevkit CMSIS-DAP firmware into a **wireless** SWD debug probe / flash
> programmer. The ESP32 is a **USB host** that talks CMSIS-DAP v2 over its native
> USB-OTG port to the Pico 2 W, and exposes the probe's capabilities to a phone /
> fleet tool as **semantic commands over BLE**.

This document is the firmware spec for the ESP32 side. The Pico 2 W side does
not change — it keeps running the existing `edev_dapv2_zephyr` (or pico-sdk
`edev_dapv2`) firmware that already exposes a USB CMSIS-DAP v2 endpoint.

A complete reference implementation already exists at
`/Users/yogesh/projects/uh_firmware/uh_firmware/platformio/dap_ble_bridge` —
this guide explains how to bring it up against the Pico 2 W and extend it with
the partial / full flash read / write / erase / recovery commands required for
edevkit's app-side feature set.

---

## Pico-side firmware status (read this first)

The bridge talks to a Pico running the `edev_dapv2_zephyr` firmware. The
currently-shipping release is **`v0.1.8`** (`feat/edev_dapv2_zephyr`
branch, 2026-06-30). Develop against this — the test harness
(`firmware/projects/edev_dapv2_zephyr/tools/test_nrf53_vendor_cmds.py`)
and the wire-format expectations below all assume v0.1.8 semantics.

> **v0.1.8 is a docs + tools release** — firmware binaries are
> byte-identical to v0.1.7. A v0.1.7 probe IS a v0.1.8 probe; you
> don't need to re-flash. v0.1.8 ships this doc (Appendices L + M,
> the Net flash @ 0x01000000 gotcha) and two new bench tools
> (`ring_pro_351_acceptance.py`, `_flash_ops_sequence.py`).
> See `releases/v0.1.8/RELEASE.md` for the full change log.

Bench-validated:
- **nRF52840** (Cortex-M4, DPv1): 5/5 PASS via v0.1.1-step5
- **nRF5340 DK** (Cortex-M33, DPv2): 11/11 PASS + 5/5 PASS on
  info→read→erase→recover→write sequence via v0.1.8 — full RECOVER +
  flash write + UICR programming all work end-to-end

Older releases (`v0.1-step5` had `nrf53_dp_full_wake` bugs that wedged
nRF52840 — fixed in v0.1.1; subsequent point releases add features but
v0.1.1 still works fine for nRF52840). Don't flash anything older than
v0.1.1 on the bridge's companion Pico.

### Per-chip support — vendor commands shipped in v0.1.8 (complete; unchanged from v0.1.7)

| Cmd | Op | nRF52840 | nRF5340 |
|---|---|---|---|
| `0x84` | `NRF53_RECOVER` (full unlock) | n/a (use `0x85`) | ✅ HW |
| `0x85` | `NRF53_ERASE` (CTRL-AP ERASEALL) | ✅ HW | ✅ HW |
| `0x86` | `NRF53_FLASH_WRITE_NET` | n/a (no Net) | ✅ HW |
| `0x87` | `NRF53_FLASH_WRITE_APP` (family-aware via NVMC base param) | algo | ✅ HW |
| `0x88` | `NRF53_READ_MEM` (chip-agnostic AHB-AP read) | ✅ HW | ✅ HW |
| `0x89` | `NRF53_TARGET_INFO` (single-call DPIDR+AP_IDR+CPUID) | algo | ✅ HW |
| `0x8A` | `NRF53_UICR_PROGRAM_APP` | n/a (nRF52 UICR differs) | ✅ HW |
| `0x8B` | `NRF53_UICR_PROGRAM_NET` (on-target stub) | n/a | ✅ HW |
| `0x8C` | `NRF53_WRITE_MEM` (chip-agnostic AHB-AP write) | ✅ HW | ✅ HW |

`HW` = bench-validated against real silicon. `algo` = code is the same
path as the HW-validated case (only NVMC base differs).

**For nRF52 UICR.APPROTECT specifically** (when the bridge needs to
unlock-then-flash on nRF52): the address is `0x10001208` (single
register, not two like nRF5340's APPROTECT + SECUREAPPROTECT). Write
sequence: `NVMC.CONFIG=Wen` (0x4001E504 ← 1, then poll READY at
0x4001E400) → write the UICR word → `NVMC.CONFIG=Ren`. The bridge
composes this from `0x8C WRITE_MEM` + `0x88 READ_MEM` calls today;
~5 round-trips total. A dedicated nRF52-family vendor command is
deferred (the composed path is fast enough).

**For full nRF52840 development recipes** (all six edevkit operations
mapped onto concrete address tables + DAP_Transfer sequences), see
**Appendix K** below — the dedicated nRF52840 cookbook (now updated
with `0x86`/`0x87`).

**What works today on v0.1-step5:**

- Full standard CMSIS-DAP v2 surface (DAP_Info, Connect, Transfer,
  Block, SWJ_*, WriteAbort, …) — unchanged from upstream Zephyr
- Vendor cmd `0x84` `NRF53_RECOVER` — full Nordic unlock
- Vendor cmd `0x85` `NRF53_ERASE` — CTRL-AP ERASEALL
- Vendor cmd `0x8A` `NRF53_UICR_PROGRAM_APP` — host-side App UICR
- Vendor cmd `0x8B` `NRF53_UICR_PROGRAM_NET` — on-target Net stub UICR

**Not yet implemented (return `ID_DAP_INVALID = 0xFF`):**

- Vendor cmd `0x86` `NRF53_FLASH_WRITE_NET` — pending step 6
- Vendor cmd `0x87` `NRF53_FLASH_WRITE_APP` — pending step 7
- Vendor cmd `0x88` `NRF53_READ_MEM` (with VC_CORERESET fix) — pending step 8
- Vendor cmd `0x89` `NRF53_TARGET_INFO` — pending
- The pico-sdk **legacy** vendor commands described in older drafts
  (CORTEX_M_HALT 0x80, CORTEX_M_DUMP 0x8A, VENDOR_NRF_RESET 0x83,
  SWCLK_KEEPALIVE_ON/OFF 0x90/0x91) — **none of these are in the
  Zephyr port**; the doc's older Appendix G.3 entries are historical.
  See the corrected G.3 below.

**Implication for the bridge:**

- Phase 2 (probe + target info), Phase 4 (erase + recover): fully
  doable today against v0.1-step5 using vendor cmds `0x84`/`0x85` +
  standard DAP_Info / DAP_Transfer.
- Phase 3 (flash read): doable today using standard DAP_Transfer +
  DAP_TransferBlock, with the ESP32 owning the AHB-AP-13-op batching
  workaround (§15.6). Migrates to vendor cmd `0x88` when step 8 ships.
- Phase 5 (flash write): two paths — (a) wait for vendor cmds
  `0x86`/`0x87`, or (b) drive a CMSIS Flash Algo RAM loader from the
  ESP32 today using standard DAP_Transfer. The latter is more work
  on the ESP32 side but works against v0.1-step5.

The full release notes live at
`firmware/projects/edev_dapv2_zephyr/releases/v0.1.1-step5/RELEASE.md`.

---

## 0. Quick start (zero → BLE advertising in ~15 minutes)

If you just want to get hands-on first and read the architecture later, do
this end-to-end. Everything else in the doc explains *why*.

```bash
# 1.  Install PlatformIO Core if you don't have it (one-time)
python3 -m pip install --user platformio

# 2.  Copy the reference implementation as your starting point
cp -R /Users/yogesh/projects/uh_firmware/uh_firmware/platformio/dap_ble_bridge \
      ~/projects/edev_bridge
cd ~/projects/edev_bridge

# 3.  Rename to make it yours (optional but clearer)
git init && git add -A && git commit -m "import dap_ble_bridge as baseline"

# 4.  Plug the ESP32-S3-DevKitC-1 into your dev machine via the USB-UART port
#     (the one with the CP2102 / CH343 silkscreened next to it — NOT the OTG port).
#     Confirm it shows up:
ls /dev/cu.usbserial-* /dev/cu.usbmodem* 2>/dev/null   # macOS
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null               # Linux

# 5.  Build + flash (first build pulls NimBLE; ~3 min)
pio run -e esp32-s3-devkitc-1 -t upload

# 6.  Open the monitor
pio device monitor -b 921600

# 7.  Expect this banner within 2 seconds:
#     # dap_ble_bridge — TLV up on serial + BLE 'DP_xxxxxxxxxxxx' (tools/dap_cli.py)

# 8.  On a phone, install "nRF Connect for Mobile" (Nordic, free).
#     Scan — confirm DP_<MAC> shows up. Connect, expand the services list,
#     and confirm both 180A (DIS) and 86f6d000-… (DAP service) are present.

# 9.  Now plug a Pico 2 W (running edev_dapv2_zephyr) into the ESP32's
#     OTHER USB-C port (the OTG one, the one without the silkscreen UART label).
#     The serial monitor should log probe enumeration:
#       # uh_usbdap: probe enumerated VID=0x2E8A PID=0x000C
#       # uh_usbdap: bulk EP OUT=0x02 IN=0x82, packet size=512

# 10. Run the Python test client from §Appendix D to fire a USB_STATUS over BLE
#     and confirm the round-trip works end-to-end.
```

That's the baseline — every other feature in this doc is incremental.

If step 5 fails with "platform-espressif32 not found", install the pioarduino
fork:
```bash
pio platform install \
  https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip
```

If step 7 doesn't print, check the troubleshooting matrix in Appendix E.

---

## 1. Architecture

```
  ┌─────────────────────┐         ┌─────────────────────────┐         ┌────────────────────┐         ┌─────────────────┐
  │  Phone / fleet app  │  BLE    │  ESP32-S3               │   USB   │  Pico 2 W          │   SWD   │  Target MCU     │
  │  (Android / iOS /   │ ──────▶ │  • NimBLE peripheral    │ ──────▶ │  • CMSIS-DAP v2    │ ──────▶ │  nRF52 / nRF53  │
  │   Web Bluetooth)    │         │  • USB host (IDF)       │  bulk   │    over USB CDC    │         │  STM32 / etc.   │
  │                     │ ◀────── │  • TLV/RPC dispatcher   │ ◀────── │  • PIO SWD driver  │ ◀────── │                 │
  └─────────────────────┘  GATT   └─────────────────────────┘         └─────────────────────┘         └─────────────────┘
       TLV frames                  ESP32 = the bridge                  Pico 2 W = the probe              the device being
       (commands + responses)      No SWD, no flash on ESP32           Standard CMSIS-DAP v2             debugged / flashed
                                                                       device — already done
```

### Design rule — **fat RPC**, not a byte tunnel

The app **never** sends raw DAP packets over BLE. It sends one high-level
command (`READ_MEM`, `ATTACH`, `ERASE_RECOVER`, …) and the ESP32 runs the
multi-step SWD sequence locally over USB. A raw DAP-over-BLE tunnel would cost
one BLE round-trip per SWD transaction → minutes for a single flash read. Fat
RPC keeps a 4 KB flash read to one BLE round-trip and one USB burst.

The ESP32 is **stateless on flash content** — it streams data through. The
two-tier buffer strategy (§13) keeps RAM usage bounded regardless of read size.

---

## 2. Why ESP32-S3 specifically

| Feature | Why required |
|---|---|
| Native USB-OTG (full-speed) | Host role for CMSIS-DAP v2 bulk endpoints. Plain ESP32 does not have USB-OTG. ESP32-S2 has it but no BLE. |
| Bluetooth 5.0 LE | 2M PHY + DLE = a few KB/s throughput for flash dumps. ESP32-C3 / C6 also work but lack USB-OTG. |
| 8 MB+ flash, 512 KB+ SRAM | Holds 64 KB RX read buffer (§13) + NimBLE stack + USB host + firmware partitions. |
| Dual-core Xtensa | One core for USB-host IDF task, one for NimBLE — no shared-bus contention. |

**ESP32-S3 is the only mainstream ESP variant that satisfies all three.** The
reference implementation targets the `esp32-s3-devkitc-1` (8 MB QSPI flash,
on-board USB-OTG port, on-board USB-UART bridge for the serial console).

---

## 3. Hardware bill of materials

| Item | Qty | Notes |
|---|---|---|
| ESP32-S3-DevKitC-1 (or N16R8 variant) | 1 | Native USB-OTG port (USB-C) and a separate USB-UART (also USB-C) for logs |
| Raspberry Pi Pico 2 W | 1 | Running `edev_dapv2_zephyr` firmware (CMSIS-DAP v2 USB device) |
| USB-C ↔ USB-C cable | 1 | ESP32 USB-OTG ↔ Pico 2 W native USB |
| Optional: USB hub or USB-C splitter | 1 | If powering ESP32 from a wall adapter AND keeping the USB-UART connected to the host PC for logs |
| 0.1"-pitch jumper wires | 4 | SWD harness Pico 2 W ↔ target MCU (Pico side; not ESP32 side) |
| Target MCU (e.g. nRF52840 dongle, nRF5340 DK) | 1 | The actual device under test |

### Power & wiring

- The ESP32 USB-OTG port **sources VBUS** to the Pico 2 W when it acts as host.
  No external power injector needed for the Pico — it draws &lt;500 mA, well
  within USB-OTG limits.
- Keep the ESP32's USB-UART port connected to your dev machine for `monitor`
  logs (CDC over the other USB-C). Both USB-C ports on the DevKitC-1 are
  independent.
- SWD wires from Pico 2 W to target are unchanged from the standard edevkit
  bring-up: `SWCLK = GP2`, `SWDIO = GP3`, `nRESET` on a free GPIO, common GND.

```
   ┌────────────┐                       ┌──────────────┐                     ┌────────────┐
   │  Dev PC    │  USB-C ── UART logs   │  ESP32-S3    │  USB-C ── DAP v2    │  Pico 2 W  │  SWD 4-wire  │ Target MCU │
   │ (serial)   │ ───────────────────── │  DevKitC-1   │ ────────────────── ▶│ (probe)    │ ──────────── │            │
   │            │                       │  • OTG=HOST  │                     │            │              │            │
   └────────────┘                       └──────────────┘                     └────────────┘              └────────────┘
   monitor_speed                          BLE peripheral                       (no change                 reads/writes
   921600                                 phone app connects here              from existing fw)          target's flash
```

### Bridge-side serial console pinout

The on-board USB-UART (a CP2102 / CH343 / WCH chip depending on revision) is
wired to **GPIO43 (TX0) / GPIO44 (RX0)** on the ESP32-S3 — keep `Serial`
(UART0) for logs at **921600 baud**. The USB-OTG port is exclusively the host
interface to the Pico.

The Arduino USB stack must NOT claim the USB-OTG port as a CDC device.
Build flags (set in `platformio.ini`):

```ini
build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=0
```

This forces the OTG port into host mode at boot.

---

## 4. Software stack — the layered view

```
┌─────────────────────────────────────────────────────────────────────────┐
│  TLV/RPC command set  (uh_dap_rpc)                                       │  semantic API
│  USB_STATUS · DAP_INFO · CONNECT · ATTACH · READ_MEM · WRITE_MEM ·       │  the app sees
│  ERASE_RECOVER · TARGET_INFO · RESET_HALT · NET_ON · RAW_DAP · …         │
├─────────────────────────────────────────────────────────────────────────┤
│  TLV framing  (uh_tlv)                                                   │  transport-agnostic
│  [type u16][seq u16][length u16][value]                                  │  reassembler + encoder
├─────────────────────────────────────────────────────────────────────────┤
│  Transport bindings — same TLV runs over both, in parallel               │
│                                                                          │
│    Serial UART0 @921600        BLE GATT (NimBLE)                         │
│    │  Serial.read/write        │  RX char 86f6d001 (write)               │
│    │                           │  TX char 86f6d002 (notify, MTU-split)   │
│    └─ uh_tlv reassembler #1    └─ uh_tlv reassembler #2                  │
├─────────────────────────────────────────────────────────────────────────┤
│  DAP / SWD / ADIv5  (uh_dap)                                             │  ARM debug protocol
│  DPIDR/AP_IDR walk, CSW, BANK_SEL, AHB-AP MEM read/write, line reset,    │
│  CTRL-AP for nRF, posted-AP-read pipeline                                │
├─────────────────────────────────────────────────────────────────────────┤
│  CMSIS-DAP v2 transport  (uh_usbdap)                                     │  USB bulk packet framing
│  Bulk OUT/IN, packet size from DAP_Info, vendor commands (0x80–0x9F)     │
├─────────────────────────────────────────────────────────────────────────┤
│  USB host  (ESP-IDF usb/usb_host.h, vendor class)                        │  Hardware
│  Device enumeration, descriptor walk (VID:PID/MS-OS 2.0), claim          │
│  interface, bulk transfer submission                                     │
└─────────────────────────────────────────────────────────────────────────┘
```

The reference at `dap_ble_bridge/` already implements everything above. The
edevkit-specific work is:
- Validate that the Pico 2 W running `edev_dapv2_zephyr` enumerates correctly
  on the ESP32 (it's a standard CMSIS-DAP v2 device, but always re-verify).
- Add the four extended commands the user requested that aren't in the
  reference yet:
  - `RPC_FLASH_READ_FULL` — read entire target flash with streaming
  - `RPC_FLASH_ERASE_FULL` — erase whole chip
  - `RPC_FLASH_ERASE_RANGE` — erase a specific range
  - `RPC_FLASH_WRITE_FULL` — write a full image (hex / bin) uploaded via BLE
- Wire the buffering strategy (§13): 40 KB RAM read ring, ESP32 external flash
  (the QSPI flash on the DevKitC-1) for write staging.

---

## 5. Build environment

### Prerequisites — one-time setup

```bash
# Python 3.10+ for PlatformIO Core
python3 --version          # 3.10.0 or newer

# Install PlatformIO Core
python3 -m pip install --user platformio
export PATH="$HOME/.platformio/penv/bin:$PATH"   # add to .zshrc / .bashrc
pio --version              # expect ≥ 6.1.x

# Install the pioarduino platform (NOT espressif32 — see below)
pio platform install \
  https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip

# Verify
pio platform show pioarduino | grep -E "Arduino|IDF"
```

You also need **`esptool.py`** (bundled with PlatformIO; standalone install
optional) for raw-mode flashing and the **`bleak`** Python library for the
test harness in Appendix D:

```bash
pip install bleak
```

### PlatformIO + pioarduino + Arduino framework

The IDF USB Host Library (`usb/usb_host.h`) is reachable from Arduino on the
ESP32-S3 only via the **pioarduino** fork (Arduino on top of IDF 5.x, the
"espressif32" stock platform pins to IDF 4 which doesn't have the host API).

```ini
; platformio.ini — the working setup from dap_ble_bridge

[env:esp32-s3-devkitc-1]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino

monitor_speed = 921600

build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=0

lib_ldf_mode = deep+

lib_deps =
    h2zero/NimBLE-Arduino@^2.2.3
```

If the app image overflows the default 1 MB OTA partition layout (likely once
flash-write staging code lands), add:

```ini
board_build.partitions = huge_app.csv
```

…which gives a single 3 MB app slot (no OTA on the bridge itself — the
edevkit OTA story is for the **target**, not the bridge).

### Libraries

- **`NimBLE-Arduino` ≥ 2.2.3** — BLE peripheral stack. Avoid the bundled
  BluedroidLE — NimBLE is 70 KB smaller and the only stack on which the
  data-length-extension + 2M PHY tuning sequence in `uh_ble.cpp` works.
- **ESP-IDF USB Host** — `usb/usb_host.h` (no PIO dependency needed for the
  pioarduino build).
- **`esp_mac.h`** — for the BT MAC used in the BLE device name.
- (Optional) **`LittleFS`** — for the on-flash write-staging partition (§13.2).

---

## 6. USB host layer — CMSIS-DAP v2 enumeration

The Pico 2 W running `edev_dapv2_zephyr` presents as:

| Descriptor | Value | Source |
|---|---|---|
| VID | `0x2E8A` | `CONFIG_SAMPLE_USBD_VID=0x2E8A` (Raspberry Pi Trading) |
| PID | `0x000C` | `CONFIG_SAMPLE_USBD_PID=0x000C` |
| bcdDevice | `0x0220` | Set at runtime by `usbd_device_set_bcd_device()` — gates probe-rs discovery; also lets the ESP32 host filter on it |
| Manufacturer | `"Edevkit"` | |
| Product | `"edev_dapv2 (Zephyr)"` | |
| Serial | `"<RP2350 64-bit unique ID hex>"` | From `CONFIG_HWINFO=y` |
| Interfaces | CDC ACM (log) + CMSIS-DAP v2 bulk | Two interfaces in the composite descriptor |
| MS OS 2.0 BOS | `BOS_VREQ_MSOSV2` with CMSIS-DAP v2 standard GUID | Makes Windows auto-bind WINUSB |

### Discovery — what the ESP32 looks for

Two layered filters:

1. **By bcdDevice ≥ 0x0220** — the same gate probe-rs uses; lets us reject
   accidentally-connected non-DAP devices.
2. **By the MS-OS 2.0 CMSIS-DAP v2 Compatible ID descriptor** — guaranteed
   stable across vendors. The reference `uh_usbdap.cpp` reads the BOS
   descriptor and matches the CMSIS-DAP v2 standard GUID
   (`{CDB3B5AD-293B-4663-AA36-1AAE46463776}`).

If the descriptors check out, claim the bulk interface, read the bulk OUT/IN
endpoint addresses, and you're done — no further setup needed before sending
the first DAP packet.

### USB Host code skeleton

The minimum to enumerate a CMSIS-DAP v2 device, claim its bulk OUT/IN
endpoints, and expose a synchronous `xfer()` for higher layers. This is the
exact pattern in `lib/uh_usbdap/uh_usbdap.cpp` (~250 LOC total). The
abbreviated version, with the parts you must implement:

```cpp
// lib/uh_usbdap/uh_usbdap.cpp  (excerpt — see reference for full source)
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static usb_host_client_handle_t s_client = 0;
static usb_device_handle_t      s_dev    = 0;
static uint8_t  s_out_ep = 0, s_in_ep = 0, s_itf = 0;
static bool     s_connected = false;
static SemaphoreHandle_t s_sem;             // signals on transfer complete
static usb_transfer_t   *s_xfer_out, *s_xfer_in;

// Walks the active configuration descriptor; finds a vendor (class 0xFF)
// interface with two bulk endpoints, claims it, captures the EP addresses.
static bool open_and_claim(uint8_t addr) {
    if (usb_host_device_open(s_client, addr, &s_dev) != ESP_OK) return false;
    const usb_config_desc_t *cd = nullptr;
    usb_host_get_active_config_descriptor(s_dev, &cd);

    const uint8_t *p = cd->val; int total = cd->wTotalLength, off = 0;
    bool in_vendor = false;
    uint8_t cand_itf = 0, out_ep = 0, in_ep = 0;
    while (off + 2 <= total) {
        uint8_t blen = p[off], btype = p[off + 1];
        if (blen == 0) break;
        if (btype == 0x04) {                         // INTERFACE descriptor
            auto *id = (const usb_intf_desc_t *)(p + off);
            in_vendor = (id->bInterfaceClass == 0xFF && id->bNumEndpoints >= 2);
            if (in_vendor) { cand_itf = id->bInterfaceNumber; out_ep = in_ep = 0; }
        } else if (btype == 0x05 && in_vendor) {     // ENDPOINT
            auto *ep = (const usb_ep_desc_t *)(p + off);
            if ((ep->bmAttributes & 0x03) == 0x02) { // bulk
                if (ep->bEndpointAddress & 0x80) in_ep  = ep->bEndpointAddress;
                else                              out_ep = ep->bEndpointAddress;
            }
            if (out_ep && in_ep &&
                usb_host_interface_claim(s_client, s_dev, cand_itf, 0) == ESP_OK) {
                s_itf = cand_itf; s_out_ep = out_ep; s_in_ep = in_ep;
                s_connected = true; return true;
            }
        }
        off += blen;
    }
    usb_host_device_close(s_client, s_dev); s_dev = 0;
    return false;
}

static void xfer_done(usb_transfer_t *t) {
    xSemaphoreGive((SemaphoreHandle_t)t->context);
}

static void client_cb(const usb_host_client_event_msg_t *m, void *) {
    if      (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) open_and_claim(m->new_dev.address);
    else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) { /* clear flags, close */ }
}

// Two background tasks: one drives the host library events, one drives client
// events. Spawn from uh_usbdap_init() with low priority and a 4 KB stack.
static void lib_task(void *)    { for (;;) { uint32_t f; usb_host_lib_handle_events(portMAX_DELAY, &f); } }
static void client_task(void *) { for (;;) usb_host_client_handle_events(s_client, portMAX_DELAY); }

void uh_usbdap_init(void) {
    usb_host_config_t hc = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    usb_host_install(&hc);
    usb_host_client_config_t cc = { .is_synchronous = false, .max_num_event_msg = 5,
        .async = { .client_event_callback = client_cb, .callback_arg = nullptr } };
    usb_host_client_register(&cc, &s_client);
    s_sem = xSemaphoreCreateBinary();
    xTaskCreate(lib_task,    "usb_lib",    4096, nullptr, 5, nullptr);
    xTaskCreate(client_task, "usb_client", 4096, nullptr, 5, nullptr);
}

// Synchronous bulk OUT then bulk IN. Returns bytes received in *in_len.
// This is exactly what uh_dap.c calls for each CMSIS-DAP packet.
int uh_usbdap_xfer(const uint8_t *out, size_t out_len,
                   uint8_t *in, size_t in_cap, size_t *in_len) {
    if (!s_connected) return -1;
    usb_host_transfer_alloc(out_len, 0, &s_xfer_out);
    memcpy(s_xfer_out->data_buffer, out, out_len);
    s_xfer_out->num_bytes = out_len;
    s_xfer_out->bEndpointAddress = s_out_ep;
    s_xfer_out->device_handle = s_dev;
    s_xfer_out->callback = xfer_done; s_xfer_out->context = s_sem;
    usb_host_transfer_submit(s_xfer_out);
    if (xSemaphoreTake(s_sem, pdMS_TO_TICKS(1000)) != pdTRUE) return -2;

    usb_host_transfer_alloc(in_cap, 0, &s_xfer_in);
    s_xfer_in->num_bytes = in_cap;
    s_xfer_in->bEndpointAddress = s_in_ep;
    s_xfer_in->device_handle = s_dev;
    s_xfer_in->callback = xfer_done; s_xfer_in->context = s_sem;
    usb_host_transfer_submit(s_xfer_in);
    if (xSemaphoreTake(s_sem, pdMS_TO_TICKS(1000)) != pdTRUE) return -3;
    *in_len = s_xfer_in->actual_num_bytes;
    memcpy(in, s_xfer_in->data_buffer, *in_len);
    usb_host_transfer_free(s_xfer_out); usb_host_transfer_free(s_xfer_in);
    return 0;
}
```

> The reference uses **persistent** transfer buffers allocated once at init,
> not per-call `usb_host_transfer_alloc` — saves ~30 µs per packet, matters
> when you're hitting tens of thousands of packets for a 1 MB flash dump.
> Adopt that optimization after the basic path works.

### Bulk endpoint use

CMSIS-DAP v2 is **packet-oriented**: every host→device packet is one bulk OUT
transfer, every response is one bulk IN transfer. Packet size is queried via
`DAP_Info(0xF1)` (packet size, u16). For RP2350-based probes this is **512 B**
(USB full-speed bulk max). The ESP32 should always allocate a 512 B (or
larger) buffer and post one bulk IN URB at a time.

### Re-enumeration after target reset

A target nRESET pulse can sometimes back-power the SWD lines enough to make
the probe re-enumerate (unlikely on the Pico 2 W, but observed on cheap
clones). The reference handles this with `RPC_REBOOT` (`0x0005`) — issues
`ESP.restart()`, the host loses BLE, reconnects, and the probe re-enumerates
cleanly. Same code path works for "the probe seems wedged, just reboot the
bridge."

---

## 7. CMSIS-DAP v2 protocol primer

The host sends a packet, the device replies one packet. Packet layout:

```
  byte 0       byte 1..N
 ┌──────┐ ┌─────────────────┐
 │ CMD  │ │  command-specific arguments / results  │
 └──────┘ └─────────────────┘
```

The packet layer is **request/response**. There is no streaming; back-pressure
is implicit (host doesn't issue command N+1 until response N is received).

### Commands used by this bridge

| CMD | Name | What it does |
|---|---|---|
| `0x00` | `DAP_Info` | Probe identity (FW ver, name, serial, capabilities, packet size, packet count) — used for `RPC_DAP_INFO` |
| `0x02` | `DAP_Connect` | Open the SWD/JTAG link at a given clock — `RPC_CONNECT` |
| `0x03` | `DAP_Disconnect` | Close the link |
| `0x04` | `DAP_TransferConfigure` | Set idle cycles + retries |
| `0x05` | `DAP_Transfer` | Issue N DP/AP transfers (the workhorse — `RPC_READ_MEM`/`RPC_WRITE_MEM` is one big DAP_Transfer) |
| `0x06` | `DAP_TransferBlock` | Block transfer (32-bit words) — used for bulk flash reads |
| `0x10` | `DAP_SWJ_Sequence` | Drive raw SWCLK / SWDIO bits — needed for the JTAG-to-SWD line reset on `RPC_ATTACH` |
| `0x11` | `DAP_SWJ_Clock` | Set SWCLK frequency |
| `0x12` | `DAP_SWJ_Pins` | Read/write nRESET, nTRST etc. — `RPC_PIN_RESET` |
| `0x13` | `DAP_SWD_Configure` | Set SWD turnaround |
| `0x80..0x9F` | Vendor commands | **Probe-specific. Our `edev_cmsis` Pico firmware exposes several of these — see Appendix G.** |

### Posted-AP-read pipeline (must-implement on host side)

ARM ADIv5 §B4.3: an AP read returns the *previous* AP read's value, not the
current one's. So a sequence of three AP reads requires four reads (the last
one is a DP.RDBUFF flush). The reference `uh_dap.cpp` and the underlying
Pico 2 W firmware (Zephyr `subsys/dap/cmsis_dap.c`) both handle this — the ESP32
must NOT re-implement the pipeline. Send DAP_Transfer with the same
register-list the host wants, and read back the same number of words.

---

## 8. DAP / SWD / ADIv5 layer

The reference `lib/uh_dap` is ~600 LOC of high-quality ADIv5 — leave it
untouched. Things it does:

- **Line reset**: 50+ SWCLK cycles with SWDIO high, then dormant→active
  JTAG-to-SWD wakeup sequence, then SWDIO low for ≥ 2 cycles. ADIv5 §B4.2.2:
  the **first** SWD transaction after line reset MUST be a DPIDR read.
- **`ATTACH`**: line reset → `DPIDR` read → `CTRL/STAT = 0x50000000`
  (CDBGPWRUPREQ + CSYSPWRUPREQ) → wait `bit 28 | bit 30` set → SELECT → AP_IDR.
- **`SET_AP`**: writes the AP CSW with `0x23000002` (32-bit, auto-inc, secure
  access) — required for nRF5340 net core which is AP=1 on the same DP.
- **CTRL-AP for Nordic**: for `RPC_ERASE_RECOVER` and `RPC_RECOVER`. The
  Nordic CTRL-AP lives at AP register address `0x04` (RESET), `0x10` (ERASEALL),
  `0x14` (ERASEALLSTATUS). Sequence: write `0x01` to ERASEALL → poll
  ERASEALLSTATUS until `0x00` → release RESET → re-attach. Variant-specific —
  the existing code handles 52840 / 5340-app / 5340-net.
- **WAIT/FAULT handling**: on ACK=2 (WAIT), retry up to 8× with idle cycles.
  On ACK=4 (FAULT), read+clear CTRL/STAT, return DAP_ERR with the sticky bits
  appended.

The new edevkit commands sit *above* this layer — they call uh_dap's `mem_read`
/ `mem_write` directly and never touch a DAP packet themselves.

---

## 9. BLE peripheral — NimBLE

The reference `lib/uh_ble` is the template. Key knobs:

### GAP

| Setting | Value | Notes |
|---|---|---|
| Device name | `DP_<MAC>` | 12 hex chars, no separators, derived from `esp_read_mac(mac, ESP_MAC_BT)` |
| Advertising flags | `BLE_HS_ADV_F_DISC_GEN \| BLE_HS_ADV_F_BREDR_UNSUP` | Standard LE-only general discoverable |
| Primary advertisement | Name only | Fits in 31 B |
| Scan response | DAP service UUID (128-bit) | Lets nRF Connect filter |
| PHY | 1M + 2M masks | Both supported, 2M preferred |

### GATT — Device Information Service (`0x180A`)

All read-only:

| Char UUID | Value (constant) |
|---|---|
| `0x2A29` Manufacturer | `"edevkit"` (was `"Ultrahuman Healthcare Pvt Ltd"` in reference) |
| `0x2A24` Model | `"EDV-BRIDGE"` (was `"DAP-BLE-BRIDGE"`) |
| `0x2A25` Serial | `"EDV-BRIDGE-<MAC>"` |
| `0x2A27` Hardware Rev | `"R0.01.00.00"` |
| `0x2A26` Firmware Rev | `"01.00.00.01"` |

### GATT — custom DAP service (NUS-style)

Service UUID: `86f6d000-f706-58a0-95b2-1fb9261e4dc7`

| Role | UUID | Properties |
|---|---|---|
| **RX** (app → bridge) | `86f6d001-f706-58a0-95b2-1fb9261e4dc7` | Write + Write-No-Response |
| **TX** (bridge → app) | `86f6d002-f706-58a0-95b2-1fb9261e4dc7` | Notify (with CCCD) |

> **Keep these UUIDs identical to the reference.** Any existing Ultrahuman
> tooling (`tools/dap_cli.py`, `tools/ultra_dap.html`) and the BLE_APP_INTEGRATION
> doc all assume these — and the edevkit app-side libraries will too. Don't
> rename them to "edv-…"; the service identity is the protocol contract.

### MTU / PHY / DLE staggered tuning

After `onConnect`, **do NOT call `updateConnParams` / `updatePhy` / `setDataLen`
inline** — iOS drops the link if these GAP procedures stack (err 133). Run them
on the loop task, **staggered ~450 ms apart**:

| Step | Wait | Action |
|---|---|---|
| 1 | 450 ms after connect | `updateConnParams(handle, 24, 40, 0, 400)` — 30–50 ms interval, 0 latency, 4 s timeout |
| 2 | 900 ms | `updatePhy(handle, 2M, 2M, 0)` |
| 3 | 1350 ms | `setDataLen(handle, 251)` — LL payload (DLE) |

MTU max is **517**; the negotiated value is `min(client, server)`. Allocate
notification chunks of `MTU − 3` bytes.

### Backpressure on TX

`notify()` returns false when the controller has no free buffers — loop with a
2 ms delay, retry up to ~2000× (~4 s). Fail-fast only if the link goes down
or `s_tx_subscribed` clears. **Never drop a TLV frame mid-send** — the host
expects contiguous notifications per frame.

### Subscribe-before-send

The ESP32 sees no central state until the app writes the TX CCCD. If a
command lands before subscription, the response notify is silently dropped.
The reference exposes `s_tx_subscribed` and the `send()` path checks it; an
app sending commands before subscribing will see timeouts.

### NimBLE initialization — the minimum you must call

```cpp
// lib/uh_ble/uh_ble.cpp  (the essential bring-up, ~30 lines)
#include <NimBLEDevice.h>
#define DAP_SVC_UUID  "86f6d000-f706-58a0-95b2-1fb9261e4dc7"
#define DAP_RX_UUID   "86f6d001-f706-58a0-95b2-1fb9261e4dc7"
#define DAP_TX_UUID   "86f6d002-f706-58a0-95b2-1fb9261e4dc7"

static NimBLEServer         *s_server   = nullptr;
static NimBLECharacteristic *s_tx_chr   = nullptr;
static volatile bool         s_connected = false, s_tx_sub = false;
static volatile uint16_t     s_conn_handle = 0, s_mtu = 23;

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
        auto v = c->getValue();
        // hand bytes to the TLV reassembler (see §10)
        uh_tlv_feed(&g_tlv_ble, v.data(), v.length());
    }
};
class TxCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &, uint16_t v) override {
        s_tx_sub = (v & 0x0001) != 0;
    }
};
class SrvCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &i) override {
        s_connected = true; s_conn_handle = i.getConnHandle();
        // DO NOT start link tuning here — schedule it ~450 ms later (see above)
    }
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
        s_connected = false; s_tx_sub = false; s_mtu = 23;
        NimBLEDevice::startAdvertising();
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo &) override { s_mtu = mtu; }
};

void uh_ble_begin(uh_ble_rx_cb on_bytes) {
    NimBLEDevice::init("DP_xxxxxxxxxxxx");          // overwritten by uh_ble_set_name
    NimBLEDevice::setMTU(517);
    NimBLEDevice::setDefaultPhy(
        BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK,
        BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK);

    s_server = NimBLEDevice::createServer();
    s_server->setCallbacks(new SrvCallbacks(), false);

    // Device Information Service (0x180A) — read-only chars
    auto *dis = s_server->createService("180A");
    dis->createCharacteristic("2A29", NIMBLE_PROPERTY::READ)->setValue("edevkit");
    dis->createCharacteristic("2A24", NIMBLE_PROPERTY::READ)->setValue("EDV-BRIDGE");
    // … same for 2A25 (serial), 2A27 (HW rev), 2A26 (FW rev)

    // Custom DAP service — RX (write) + TX (notify)
    auto *svc = s_server->createService(DAP_SVC_UUID);
    auto *rx  = svc->createCharacteristic(DAP_RX_UUID,
                  NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new RxCallbacks());
    s_tx_chr  = svc->createCharacteristic(DAP_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
    s_tx_chr->setCallbacks(new TxCallbacks());

    // Advertising: name in primary advert, service UUID in scan response
    auto *adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData adv_data;
    adv_data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    adv_data.setName("DP_xxxxxxxxxxxx");
    adv->setAdvertisementData(adv_data);

    NimBLEAdvertisementData scan_data;
    scan_data.addServiceUUID(DAP_SVC_UUID);
    adv->setScanResponseData(scan_data);
    adv->enableScanResponse(true);
    adv->start();
}
```

Full version (with the staggered post-connect tuning state machine) is in
the reference at `lib/uh_ble/uh_ble.cpp` lines 134–206.

---

## 10. TLV protocol

Verbatim from the reference — keep it byte-for-byte identical so the same
client libraries work on both the reference Ring tooling and the new edevkit
tooling.

### Frame

```
┌────────────┬────────────┬────────────┬─────────────────┐
│ type  u16  │ seq   u16  │ length u16 │ value [length]  │
└────────────┴────────────┴────────────┴─────────────────┘
   little-endian                     0..4096 bytes
```

- Total frame size = `6 + length`.
- Max frame = `6 + 4096 = 4102` bytes (for a full 4 KB `READ_MEM`).
- Response `type = request | 0x8000`.
- `value[0]` = status byte (see below); result bytes follow.
- A frame with `length > 4096` is **skipped** (stream re-syncs).

### Status codes

| status | name | meaning |
|---|---|---|
| 0 | `OK` | success |
| 1 | `NO_PROBE` | USB probe not connected to the bridge |
| 2 | `STATE` | not connected/attached, or target op failed |
| 3 | `ARGS` | malformed value / bad alignment / too big |
| 4 | `DAP_ERR` | DAP/SWD transaction error |
| 5 | `UNKNOWN` | unknown command type |

### Error diagnostics — `STATE` and `DAP_ERR`

Append `[u8 dap_ack][u32 ctrl_stat]` (little-endian) after any
command-specific data. `dap_ack` is the raw SWD ACK:

- `1` OK · `2` WAIT · `4` FAULT · `7` NO_ACK

`ctrl_stat` is the DP CTRL/STAT captured **before** sticky bits were cleared.
Lets the host say "NO_ACK → wire dead / target sleeping", "FAULT → STICKYERR
bad address", etc.

### `seq` — single-flight discipline

The host increments `seq` per request; the bridge echoes it in the response.
The host drops any response whose `seq` doesn't match the in-flight request
(it's a stale reply to a previously-timed-out request).

**Host runs single-flight** — one outstanding request at a time. Every
command in this protocol is idempotent (read / write / attach / erase /
recover all safe to retry).

### TLV encoder + reassembler (drop-in C)

```c
// lib/uh_tlv/uh_tlv.h
#define UH_TLV_MAX_VALUE 4096
#define UH_TLV_HDR       6

typedef void (*uh_tlv_frame_cb)(uint16_t type, uint16_t seq,
                                const uint8_t *val, uint16_t len, void *ctx);

struct uh_tlv {
    uh_tlv_frame_cb cb; void *ctx;
    uint8_t  state, hdr_have, hdr[UH_TLV_HDR];
    uint16_t type, seq, vlen, v_have;
    uint32_t skip;
    uint8_t  val[UH_TLV_MAX_VALUE];
};

void   uh_tlv_init(struct uh_tlv *t, uh_tlv_frame_cb cb, void *ctx);
void   uh_tlv_feed(struct uh_tlv *t, const uint8_t *data, size_t n);
size_t uh_tlv_encode(uint8_t *out, size_t cap, uint16_t type,
                     uint16_t seq, const uint8_t *val, uint16_t len);
```

```c
// lib/uh_tlv/uh_tlv.c  (~50 LOC, streaming-safe)
static inline uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

void uh_tlv_init(struct uh_tlv *t, uh_tlv_frame_cb cb, void *ctx) {
    memset(t, 0, sizeof(*t)); t->cb = cb; t->ctx = ctx;
}
void uh_tlv_feed(struct uh_tlv *t, const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        switch (t->state) {
        case 0:   /* header */
            t->hdr[t->hdr_have++] = b;
            if (t->hdr_have == UH_TLV_HDR) {
                t->type = le16(t->hdr);
                t->seq  = le16(t->hdr + 2);
                t->vlen = le16(t->hdr + 4);
                t->hdr_have = t->v_have = 0;
                if (t->vlen == 0) {
                    if (t->cb) t->cb(t->type, t->seq, t->val, 0, t->ctx);
                } else if (t->vlen > UH_TLV_MAX_VALUE) {
                    t->skip = t->vlen; t->state = 2;            /* skip oversized */
                } else {
                    t->state = 1;
                }
            }
            break;
        case 1:   /* value */
            t->val[t->v_have++] = b;
            if (t->v_have == t->vlen) {
                if (t->cb) t->cb(t->type, t->seq, t->val, t->vlen, t->ctx);
                t->state = 0;
            }
            break;
        case 2:   /* skip oversized */
            if (--t->skip == 0) t->state = 0;
            break;
        }
    }
}
size_t uh_tlv_encode(uint8_t *out, size_t cap, uint16_t type,
                     uint16_t seq, const uint8_t *val, uint16_t len) {
    if (cap < (size_t)UH_TLV_HDR + len) return 0;
    out[0] = type;  out[1] = type >> 8;
    out[2] = seq;   out[3] = seq  >> 8;
    out[4] = len;   out[5] = len  >> 8;
    if (len && val) memcpy(out + UH_TLV_HDR, val, len);
    return UH_TLV_HDR + len;
}
```

---

## 11. RPC command table — what's already in the reference

| type | name | request value | response value (after status) | status |
|---|---|---|---|---|
| `0x0001` | `PING` | — | fw ASCIIZ | ✅ done |
| `0x0002` | `USB_STATUS` | — | `u8 connected, u16 vid, u16 pid` | ✅ done |
| `0x0003` | `STATUS` | — | 6 × u8 (usb / attached / ap / halted / probe_ok / target_present) | ✅ done |
| `0x0004` | `PROBE_REINIT` | — | — | ✅ done |
| `0x0005` | `REBOOT` | — | — (ESP self-restarts after ack) | ✅ done |
| `0x0006` | `RECOVER` | — | `u32 dpidr, u32 ap_idr` | ✅ done |
| `0x0010` | `DAP_INFO` | — | `u16 caps, u16 pkt_size, u8 cnt, lp_fw, lp_prod, lp_ser` | ✅ done |
| `0x0011` | `CONNECT` | `u32 clock_hz` (0 = 1 MHz) | `u8 actual_port` | ✅ done |
| `0x0012` | `DISCONNECT` | — | — | ✅ done |
| `0x0013` | `DPID` | — | `u32 DPIDR` | ✅ done |
| `0x0014` | `ATTACH` | — | `u32 DPIDR, u32 AP_IDR` | ✅ done |
| `0x0015` | `TARGET_INFO` | — | `u32 DPIDR, u32 CPUID` | ✅ done |
| `0x0016` | `SET_AP` | `u8 apsel` | — | ✅ done |
| `0x0020` | `READ_MEM` | `u32 addr, u32 len` (4-aligned, ≤4096) | `data[len]` | ✅ done |
| `0x0021` | `WRITE_MEM` | `u32 addr, data[]` | — | ✅ done |
| `0x0030..0x0036` | `HALT / RESUME / STEP / RESET / IS_HALTED / RESET_HALT / PIN_RESET` | — | u8 (where applicable) | ✅ done |
| `0x0037` | `ERASE_RECOVER` | `u8 ctrl_ap` | — | ✅ done (Nordic-specific) |
| `0x0040..0x0042` | `REG_READ / REG_WRITE / CALL` | per-cmd | per-cmd | ✅ done |
| `0x0043` | `NET_ON` | — | — | ✅ done (nRF5340-specific) |
| `0x00F0` | `RAW_DAP` | raw DAP packet | raw DAP response | ✅ done |

`lp_X` = length-prefixed string `u8 len, char[len]`.

### Anatomy of a single RPC handler (the pattern)

Copy this shape for every new command. From the reference, this is what
`RPC_READ_MEM` looks like inside `uh_dap_rpc_dispatch()`:

```c
// lib/uh_dap_rpc/uh_dap_rpc.c  (excerpt)
static uint8_t s_resp[UH_TLV_MAX_VALUE + 8];  /* status + payload */

static void emit_err(uh_rpc_reply_fn reply, uint16_t type, uint8_t status,
                     uint8_t dap_ack, uint32_t ctrl_stat)
{
    s_resp[0] = status;
    if (status == RPC_ERR_STATE || status == RPC_ERR_DAP) {
        s_resp[1] = dap_ack;
        memcpy(s_resp + 2, &ctrl_stat, 4);          /* little-endian, native ESP32 */
        reply(type | RPC_RESP_FLAG, s_resp, 6, NULL);
    } else {
        reply(type | RPC_RESP_FLAG, s_resp, 1, NULL);
    }
}

static void do_read_mem(const uint8_t *v, uint16_t len, uh_rpc_reply_fn reply)
{
    /* 1. ARG VALIDATION (always first) */
    if (len != 8) { emit_err(reply, RPC_READ_MEM, RPC_ERR_ARGS, 0, 0); return; }
    uint32_t addr = v[0] | (v[1]<<8) | (v[2]<<16) | (v[3]<<24);
    uint32_t rlen = v[4] | (v[5]<<8) | (v[6]<<16) | (v[7]<<24);
    if (rlen > RPC_READ_MAX || (addr & 3) || (rlen & 3)) {
        emit_err(reply, RPC_READ_MEM, RPC_ERR_ARGS, 0, 0); return;
    }

    /* 2. STATE GATING (do we have what this op needs?) */
    if (!uh_usbdap_connected())  { emit_err(reply, RPC_READ_MEM, RPC_ERR_NO_PROBE, 0, 0); return; }
    if (!uh_dap_attached())      { emit_err(reply, RPC_READ_MEM, RPC_ERR_STATE,    7, 0); return; }

    /* 3. THE ACTUAL WORK */
    uint8_t ack; uint32_t ctrl_stat;
    int rc = uh_dap_mem_read(addr, s_resp + 1, rlen, &ack, &ctrl_stat);
    if (rc) { emit_err(reply, RPC_READ_MEM, RPC_ERR_DAP, ack, ctrl_stat); return; }

    /* 4. EMIT RESPONSE: status byte (0=OK) + payload */
    s_resp[0] = RPC_OK;
    reply(RPC_READ_MEM | RPC_RESP_FLAG, s_resp, 1 + rlen, NULL);
}

void uh_dap_rpc_dispatch(uint16_t type, const uint8_t *val, uint16_t len,
                         uh_rpc_reply_fn reply, void *ctx)
{
    switch (type) {
    case RPC_READ_MEM:    do_read_mem(val, len, reply); break;
    case RPC_DAP_INFO:    do_dap_info(reply); break;
    case RPC_CONNECT:     do_connect(val, len, reply); break;
    /* … add new ones here … */
    default:
        s_resp[0] = RPC_ERR_UNKNOWN;
        reply(type | RPC_RESP_FLAG, s_resp, 1, NULL);
    }
}
```

The same skeleton — **validate args → gate on state → do the work → emit
response** — applies to every new command in §12. Always emit exactly one
response (the transport assumes single-flight). Never `return` without
having called `reply()` at least once.

---

## 12. New commands — edevkit Phase 2-5 additions

The user's command list, mapped onto the protocol layers. The **BLE TLV
RPC** is between phone-app and ESP32; the **Pico-side command** is the
USB CMSIS-DAP packet(s) the ESP32 sends to the probe.

| Phase | App-side request | ESP32 → Pico (today, against v0.1-step5) | Future shortcut |
|---|---|---|---|
| 2 | Read **edev_dap info** (`RPC_DAP_INFO` 0x0010) | Standard `DAP_Info` (0x00) — works today ✅ | n/a |
| 2 | Read **target MCU info** (`RPC_TARGET_INFO` 0x0015) | Standard `DAP_Transfer` reading DPIDR + CPUID — works today ✅ | Vendor cmd `0x89` `NRF53_TARGET_INFO` (pending) adds FICR.PART per family in one call |
| 3 | **Partial flash read** (`RPC_READ_MEM` 0x0020) | Standard `DAP_Transfer` / `DAP_TransferBlock` with the ESP32-side AHB-AP-13-op batching (§15.6) ⚠️ | Vendor cmd `0x88` `NRF53_READ_MEM` (pending step 8) — also fixes the App-readback-0xFF post-reset bug (6a) by clearing VC_CORERESET |
| 3 | **Full flash read** (`RPC_FLASH_READ_FULL` 0x0050) | Loop the partial read on the ESP32 side ⚠️ | Same vendor cmd `0x88` once shipped, with progress packets (step 9) |
| 4 | **Partial flash erase** (`RPC_FLASH_ERASE_RANGE` 0x0052) | Compose from RAM loader's ERASE entry via `DAP_Transfer` ⚠️ — only for non-Nordic targets; for Nordic, partial erase usually isn't needed because RECOVER (0x84) is whole-chip | n/a (full-chip flow is the Nordic path) |
| 4 | **Full flash erase** (`RPC_FLASH_ERASE_FULL` 0x0053) | Vendor cmd `0x85` `NRF53_ERASE` — works today ✅ | n/a |
| 4 | **Target MCU recovery** (`RPC_ERASE_RECOVER` 0x0037) | Vendor cmd `0x84` `NRF53_RECOVER` — works today ✅ (full unlock: ERASEALL both cores + App UICR + Net UICR via on-target stub, one call) | n/a — already the shortcut |
| 5 | **Partial flash write** (`RPC_FLASH_WRITE_RANGE` 0x0054) | App: vendor `0x87 NRF53_FLASH_WRITE_APP` (one call per 13-word batch) ✅; Net (nRF5340 only): vendor `0x86 NRF53_FLASH_WRITE_NET` ✅ | (already shipped) |
| 5 | **Full flash write** (`RPC_FLASH_WRITE_FULL` 0x0055) | Multi-chunk: upload to ESP32 staging flash, then loop `0x86`/`0x87` over the image ~30× faster than DAP_Transfer composition ✅ | (already shipped) |

**Legend**: ✅ = no extra ESP32 work, just send the right packet.
⚠️ = today the ESP32 owns the orchestration; future Pico vendor cmds
will subsume it as a 1-call shortcut.

**Practical takeaway for the bridge author (v0.1.6-step7 reality):**
Almost everything is a single vendor command now. Phase 2 (probe +
target info), Phase 3 (flash read via `0x88`), Phase 4 (erase via
`0x85`, recover via `0x84`), Phase 5 (App flash write via `0x87`, Net
via `0x86`) — all need only ~zero ESP32-side orchestration code.
Loop the vendor command, push BLE notifications for progress, done.
The ESP32 doesn't need to know about NVMC sequencing, CTRL-AP magic,
or the AHB-AP-13-op wall — those all live probe-side now.

### 12.1 `RPC_FLASH_READ_FULL` — 0x0050

```
Request:  u32 chunk_size      (0 = use 4096 default; client hint for progress granularity)
Response: u8 status                              (then one OK, followed by …)
          u32 total_bytes      (the flash size the bridge auto-detected)
          u32 base_addr        (the starting address, usually 0x00000000)

… then the bridge emits a series of UNSOLICITED notifications of type
0x4050 (FLASH_READ_FULL_CHUNK), each carrying:

   u32 offset          (running offset into the dump, monotonic)
   u8  data[chunk_size]    (last chunk may be short)

… terminated by one type 0x4051 (FLASH_READ_FULL_END):

   u8 status           (0 OK, or error if mid-stream failure)
   u32 final_offset    (== total_bytes on success)
```

**Why streaming notifications and not loops of `READ_MEM`?** Two reasons:
1. The app side gets a single API call instead of an open loop. The bridge
   handles flash-size detection, addressing, and end-of-flash exactly once.
2. The 40 KB RAM buffer (§13.1) doubles as a streaming window — the bridge can
   prefetch the next chunk while the BLE notify is draining.

App pseudocode:

```python
dev.subscribe(TX, on_notification)
seq = ...
dev.write(RX, frame(RPC_FLASH_READ_FULL, seq, u32(0)))

# initial ack
status, total, base = await_response(RPC_FLASH_READ_FULL, seq, timeout=5)

# then accumulate chunks
out = bytearray(total)
while True:
    typ, _, val = next_unsolicited_or_response()
    if typ == 0x4050:
        off = u32(val[:4]); chunk = val[4:]
        out[off:off+len(chunk)] = chunk
        report_progress(off + len(chunk), total)
    elif typ == 0x4051:
        if val[0] == 0: break
        else: raise dap_error(val)
```

### 12.2 `RPC_FLASH_ERASE_RANGE` — 0x0052

```
Request:  u32 addr, u32 len
          (both page-aligned; bridge errors with ARGS if not)
Response: u8 status
```

Implementation per-vendor:
- **Nordic nRF52/53**: load the on-target loader (`loader/nrf_flash_loader.S`)
  into target RAM (one `WRITE_MEM` of ~256 B), then `CALL(ERASE_entry, page_addr,
  nvmc_base, style)` per page. The reference already has the loader artifact
  baked in.
- **Other vendors**: bridge returns `ARGS` for now; the app falls back to
  `WRITE_MEM(addr, 0xFF...)` (which works for some MCUs that emulate erased
  flash as 0xFF reads but is incorrect for write-once flash).

App pseudocode:

```python
status = call(RPC_FLASH_ERASE_RANGE, u32(0x10000) + u32(0x4000))  # erase 16 KB at 0x10000
```

Boundary checks the bridge performs:
- `addr % page_size == 0` (page size from target identity — 4 KB on nRF52, 4 KB on nRF53)
- `len % page_size == 0`
- `addr + len ≤ flash_top` (flash_top from target identity)
- All ARGS error if violated.

### 12.3 `RPC_FLASH_ERASE_FULL` — 0x0053

```
Request:  —
Response: u8 status
```

- **Nordic with locked chip**: alias for `RPC_ERASE_RECOVER` (CTRL-AP ERASEALL).
- **Nordic with unlocked chip / non-Nordic**: loops `RPC_FLASH_ERASE_RANGE`
  internally over the full flash range, with watchdog feed each page (the
  bridge handles this transparently — see §15 below).

### 12.4 `RPC_FLASH_WRITE_RANGE` / `RPC_FLASH_WRITE_FULL` — 0x0054 / 0x0055

Different shapes because the **data has to get from the app to the bridge first**.

**`RPC_FLASH_WRITE_RANGE` — single-shot ≤ 4 KB:**

```
Request:  u32 addr, u32 len, u8 data[len]
          (len ≤ 4096, addr 4-aligned, len 4-aligned)
Response: u8 status
```

Same as `RPC_WRITE_MEM` but goes through the flash loader (ERASE + PROGRAM)
instead of poking memory.

**`RPC_FLASH_WRITE_FULL` — multi-chunk streaming upload:**

```
Phase A — start the upload:
   Request:  u32 total_bytes, u32 base_addr (or 0 for "auto")
   Response: u8 status, u32 staging_offset (where the bridge will buffer first chunk)

Phase B — push N chunks (one TLV write per chunk, ≤ 4096 B value each):
   Request:  u32 offset, u8 data[len]
             type = 0x0056 (FLASH_WRITE_FULL_CHUNK)
   Response: u8 status, u32 received_offset (running tally)

Phase C — commit / flash:
   Request:  —
             type = 0x0057 (FLASH_WRITE_FULL_COMMIT)
   Response: u8 status, u32 verified_bytes
   …then UNSOLICITED notifications of type 0x4057 (FLASH_WRITE_FULL_PROGRESS)
   carrying u32 bytes_programmed during the actual page-by-page flash, until
   the final response notification with the result.
```

**Why three phases?** Decouples *transport* (slow BLE upload) from *target
write* (fast on-target loader). The full 1 MB image streams into ESP32
external flash at BLE speed (~5 s for 1 MB at MTU 517 + 2M PHY), then the
bridge runs the loader at USB speed (~10–15 s for 1 MB to the target).

Buffer management: see §13.2.

### 12.5 Target-identity refinement (optional Phase 2 extension)

Today `RPC_TARGET_INFO` returns DPIDR + CPUID — enough to identify the ARM
core but not the silicon. To get vendor/part/variant, extend with:

```
Request:  —              type = RPC_TARGET_IDENT (0x0017)
Response: u8 status,
          u32 dpidr,
          u32 cpuid,
          u32 nvic_iser0_addr,        (vendor SCB-relative)
          u32 vendor_id_word,         (FICR.INFO.PART for Nordic, DEVICE_ID for ST, …)
          u32 flash_size_bytes,
          u32 ram_size_bytes,
          u32 page_size_bytes,
          u8  lp_vendor, char[],      ("Nordic", "STM32", …)
          u8  lp_part,   char[]       ("nRF52840", "STM32F411", …)
```

The bridge auto-detects the vendor by inspecting CPUID's vendor field (0x41
ARM), then falls back to a small per-vendor probe (e.g. for Nordic: read
`FICR.INFO.PART` at `0x10000100`). This drives the boundary checks in the
erase/write commands. Implement after the basic Phase 2 ships.

---

## 13. Buffering strategy

### 13.1 Flash read — 40 KB RAM streaming buffer

Per the user's spec: "for a 1 MB flash read, keep RAM of 40 KB for reading and
continuously transfer in BLE."

```
       ┌──────────────────┐                        ┌──────────────────┐
       │   Pico 2 W       │   USB bulk             │   ESP32 (loop)   │
       │   (probe)        │ ────────────────────▶  │                  │
       │                  │  4 KB DAP_TransferBlk  │  RAM ring buffer │  BLE notify
       │                  │                        │  ┌─────┬─────┬─┐ │ ──────────▶  app
       │                  │                        │  │ ... │ ... │ │ │
       │                  │                        │  └─────┴─────┴─┘ │  514 B per
       │                  │                        │     40 KB        │  notification
       └──────────────────┘                        └──────────────────┘
```

- Ring buffer size: 40 KB (10 × 4 KB chunks). Tunable via `#define
  EDV_READ_RING_KB 40` in `lib/uh_flash/uh_flash.h`.
- Producer (USB task): submits a `READ_MEM(next_addr, 4096)` over CMSIS-DAP,
  appends the result to the ring tail.
- Consumer (NimBLE task): pulls bytes from ring head, fragments into
  `(MTU - 3)`-byte notifications via `uh_ble_send()`.
- Back-pressure: when ring fills, USB task blocks on a binary semaphore until
  BLE drains below 50%. This is implicit (the loop task does both) — no
  explicit semaphore needed.

The ring buffer lives in ESP32 internal SRAM (`DRAM_ATTR uint8_t
ring[40 * 1024]`). Costs ~40 KB of the ~256 KB free SRAM — easy fit.

### 13.2 Flash write — external flash staging

Per the user's spec: "for flash write use external flash for storing and then
writing into target device."

The ESP32-S3 DevKitC-1's on-board QSPI flash (8 MB) is partitioned:

```
┌─────────────────────────────────────────────────────────────┐
│  bootloader │ partition table │ nvs │ app   (3 MB)          │
├─────────────────────────────────────────────────────────────┤
│            staging (4 MB)         │     free                │
└─────────────────────────────────────────────────────────────┘
                ▲
                └── one big LittleFS partition; holds the inbound
                     hex/bin image until COMMIT
```

Partition table (`partitions.csv`):

```
# Name,    Type, SubType, Offset,  Size,    Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
app0,     app,  ota_0,   0x10000, 0x300000,
staging,  data, fat,     ,        0x400000,
```

Why FAT (not raw)? Easier to clear/list/inspect from the bridge's own serial
console for debugging. LittleFS would also work — pick whichever the project's
existing tooling supports better.

The write flow:
- Phase A (`FLASH_WRITE_FULL` start): bridge erases the staging partition (≤1 s
  on a 4 MB region; fast enough to not need user-facing progress).
- Phase B (chunk push): each chunk is `fwrite()`'d to `/staging/image.bin` at
  the given offset. The bridge responds within a few ms — no BLE flow-control
  needed because the host is single-flight.
- Phase C (commit): bridge reads the file back in 4 KB pages, calls the
  on-target loader for each page (ERASE + PROGRAM), sends a progress
  notification every page.

**Why not write target-flash directly during Phase B?** Two reasons:
1. The target needs to be `RESET_HALT`'d before any flash op — but the user
   might decide to abort mid-upload (BLE disconnect, app crash). External
   flash staging means the target is touched **only** when Phase C runs.
2. Resumability — if BLE drops in the middle of Phase B, the staging file is
   intact; the app re-uploads only the missing chunks (the bridge tracks
   `received_offset`).

---

## 14. Bring-up sequence for the app

The canonical bring-up the app performs after BLE connect — same as the
reference and identical to what the existing edevkit tooling expects:

```
1.  scan + connect, subscribe to TX, request MTU 517
2.  RPC_USB_STATUS         ← is the probe even plugged in?
3.  RPC_DAP_INFO           ← probe identity (display in app)
4.  RPC_CONNECT  (clock=0) ← open SWD link
5.  RPC_ATTACH             ← line reset → DPIDR → AP_IDR
6.  RPC_TARGET_INFO        ← target identity (display in app)
7.  RPC_RESET_HALT         ← park core at reset vector (critical — see §15)
8.  RPC_FLASH_READ_FULL    ← or any other Phase 2-5 op
9.  RPC_RESUME (or RPC_RESET if image was flashed)
10. RPC_DISCONNECT
```

The app should expose these as discrete steps in the UI — operators want to
see "probe found ✓ / target found ✓ / halted ✓" before they fire off a flash
op.

---

## 15. Robustness — things that will bite

### 15.1 Target enters ship-mode mid-read

A *running* MCU often powers down its debug domain ~1 s after reset / wake
(Nordic does this aggressively to save battery). SWD then returns `NO_ACK
(ack=7)` indefinitely. Mitigations:

- **`RESET_HALT` right after `ATTACH`** — holds the core at the reset vector,
  preventing it from entering ship-mode. Do this *before* any long read.
- **WDT feed loop** — the bridge runs a background task that writes
  `WDT.RR[n] = 0x6E524635` (Nordic magic) every ~200 ms while attached. Stops
  the target's own watchdog from firing mid-flash. Auto-detects WDT instance.
- **Keep the device on a charger** — for sealed wearables, the firmware ships
  itself off below a battery threshold; ops fail until USB power is applied.

### 15.2 BLE link drops mid-multipart-response

If the app disconnects while a `FLASH_READ_FULL` is streaming, the bridge:
1. Aborts the read on the next loop tick (checks `s_connected` between
   chunks).
2. Issues `RPC_RESUME` to unfreeze the target.
3. Goes back to advertising.

The app, on reconnect, restarts from step 2 of the bring-up sequence. No
state persisted across reconnects.

### 15.3 USB probe disconnects mid-operation

The IDF USB Host Library raises a `USB_HOST_CLIENT_EVENT_DEV_GONE` event.
The bridge clears `s_probe_connected`, fails the in-flight RPC with
`NO_PROBE`, and waits for re-enumeration. App sees `NO_PROBE` on the next
command and re-runs from step 2.

### 15.4 nRF5340 net-core gotchas

The net core needs `RPC_NET_ON` after every app reset (releases the
Force-OFF). Once on, `RPC_SET_AP 1` switches the DAP to the net core; flash
ops then target the net core's address space (`0x01000000` flash, `0x21000000`
RAM). The bridge handles AP arbitration internally — the app just specifies
which core via `SET_AP`.

### 15.5 Bridge wedge → app-driven recovery

If everything goes sideways, the nuclear option from the app side:
1. `RPC_REBOOT` — ESP32 self-restarts, USB re-enumerates, BLE re-advertises.
2. App reconnects.
3. Bring-up sequence from step 2.

Total recovery time: ~4 s.

### 15.6 AHB-AP transaction wall (~13 ops) — the long-read killer

**This is the single most important target-side gotcha** for long reads on
running firmware. Empirically validated against Nordic Ring firmware:

- After ~13 consecutive AHB-AP transactions the target's DP locks; subsequent
  reads return `WAIT` indefinitely.
- It's not a wire issue and not solved by retrying. It's the target firmware
  doing power-management on the debug domain mid-burst.
- **Fix**: between every batch of N≤12 AHB-AP ops, issue a CTRL-AP soft-reset
  (write `0x00000001` to CTRL-AP RESET, wait ≥2 ms, write `0x00000000`).
  This re-arms the DP for the next batch.

The ESP32's `uh_dap` flash-read loop must enforce this batching. The reference
`uh_dap` does it; if writing fresh, the pattern is:

```c
// pseudo-code in the READ_MEM bulk read loop
const int BATCH = 12;     // safe; some chips tolerate 13, none tolerate 14
for (i = 0; i < total; i += BATCH * 4) {
    int n = MIN(BATCH * 4, total - i);
    int rc = ahb_ap_read_block(addr + i, dst + i, n);
    if (rc) return rc;
    if (i + n < total) ctrl_ap_soft_reset();    // ← critical
}
```

Symptom of forgetting this: dump succeeds for the first 48 B (12 words) then
hangs with status=2 STATE / dap_ack=2 WAIT.

Background: `reference_ap_transaction_counter_wall.md` in the project memory.

### 15.7 NVMC operations cannot run while RESET is held

Nordic NVMC (the flash controller) is **clocked from the AHB bus**, which is
gated off when the chip is in reset. So a CTRL-AP `ERASEALL` issued **with
RESET asserted** stalls forever — `ERASEALLSTATUS` stays BUSY indefinitely.
OpenOCD's old "hold reset during eraseall" pattern is folklore that doesn't
apply to nRF52/53.

**Correct sequence for `ERASE_RECOVER`:**

```
1. CTRL-AP write RESET = 1            (assert reset)
2. wait ≥ 2 ms
3. CTRL-AP write RESET = 0            (RELEASE reset — NVMC needs the clock!)
4. CTRL-AP write ERASEALL = 1
5. poll CTRL-AP ERASEALLSTATUS until 0   (typically 200–500 ms)
6. CTRL-AP write APPROTECTSTATUS = 1  (re-arm)
7. re-attach (line reset → DPIDR → AP_IDR)
```

The reference `uh_dap` handles this; verify before merging any new erase code.

Background: `reference_nrf52_ctrl_ap_eraseall_no_reset_hold.md`.

### 15.8 nRF5340 multidrop SWD — both cores share TINSTANCE

The two cores (App = AP0, Net = AP1) live on the **same SWD wire**. ADIv5
multidrop is the spec-correct way to address them, but on nRF5340 specifically:

- Both cores advertise the **same `TINSTANCE`** in `TARGETID` → SWD `TARGETSEL`
  cannot disambiguate them. You **must** use `SELECT.APSEL = 0 or 1` instead.
- Switching cores via `SET_AP` is fast (one DP write), but cross-core bus
  contention can wedge the link if both cores are running.

**Mitigation:** the bridge keeps the net core in Force-OFF (default after any
app reset) until the app issues `RPC_NET_ON`. While the net core is off, the
app core is the only SWD client → no contention.

If the app needs to read both cores in one session: program app first while
net is off, then `NET_ON` → `SET_AP 1` → program net. **Don't** try to
program both concurrently.

### 15.9 nRF54 is different — RRAM and IronSide

- **nRF54L** uses **RRAM** (resistive RAM) instead of flash. The NVMC concept
  doesn't apply; programming is byte-granular and erase is implicit. Loader
  routines from nRF52/53 will NOT work — they'll fault on the missing NVMC
  registers.
- **nRF54H** uses Nordic's **IronSide secure controller + ADAC** debug
  authentication. No open-source path exists; OSS tools (probe-rs, pyocd, our
  `edev_cmsis`) cannot program nRF54H. Don't promise nRF54H support.

For now the bridge should detect the family from `FICR.INFO.PART` (addresses
differ per family — see Appendix G.4) and return `ARGS` for unsupported
families with a clear error.

Background: `reference_nrf54_programming_pattern.md`, `reference_xiao_nrf54l15_architecture.md`.

### 15.10 `probe-rs --allow-erase-all` is a flash-destroyer on nRF5340

If anyone tries to debug the bridge by running `probe-rs read --chip nRF5340
--allow-erase-all` against the same Pico, **it will wipe both cores** — even
just to read locked flash, `--allow-erase-all` issues a CTRL-AP ERASEALL
first. Use **`pyocd` direct AHB-AP reads** (or our `uhocd` fork) for nRF5340
flash inspection. Never `probe-rs --allow-erase-all` on a chip you want to
preserve.

Background: `reference_nrf5340_probe_rs_auto_erase_destroys_flash.md`.

### 15.11 SWCLK keep-alive is currently OFF in the Zephyr Pico firmware

The Zephyr port of `edev_cmsis` does NOT run a SWCLK keep-alive. Asynchronous
keep-alive pulses (the obvious implementation: a 5 kHz toggle running on a
side PIO state machine) break SWD frame alignment — the target's frame
aligner counts the keep-alive cycles, and subsequent DPIDR reads land at the
wrong bit offset.

**Consequence for the ESP32 flash-read loop**: on a target whose firmware
ship-modes its debug domain (sleepy Nordic), the link will go NO_ACK after
1–2 s of idle. The bridge must **keep the bus busy** — between batches, issue
DP IDLE reads or CTRL-AP polls, not just `k_sleep()`.

The correct in-protocol fix lives in Zephyr's `swdp_bitbang.c` WAIT-ACK retry
path; it hasn't been merged yet. Until then, the ESP32 working around it is
the practical path.

Background: `project_edev_dapv2_zephyr_audit_2026_06_29.md`,
`reference_nrf5340_edev_vs_jlink_firmware_gap_2026_06_29.md`.

### 15.12 ACK byte framing — parse bits 0:2 only

CMSIS-DAP returns the SWD ACK in bits 0:2 of `resp[2]` (after the response
header bytes). Bits 3:7 are reserved and can carry junk from some probe
firmwares. **Always mask with `& 0x07`** before comparing — don't compare
the whole byte. Valid values:

| ACK & 0x07 | Meaning |
|---|---|
| 1 | OK |
| 2 | WAIT |
| 4 | FAULT |
| 7 | NO_ACK (line dead, target absent, or debug domain off) |

A handler that compares `resp[2] == 1` instead of `(resp[2] & 7) == 1` will
randomly drop frames depending on which probe firmware build you have.

Background: `project_uh_dapv2_ack_framing_fix.md`.

### 15.13 First xact after line reset MUST be DPIDR

ADIv5 §B4.2.2: after a JTAG-to-SWD line reset (50+ SWCLK with SWDIO high,
then dormant wake), the very first transaction on the bus MUST be a DPIDR
read. Any other transaction returns FAULT and leaves the DP in a wedged
state.

This is baked into the reference `uh_dap`. If you refactor the SWD layer,
the order is non-negotiable.

Background: `reference_adiv5_first_xact_dpidr.md`.

### 15.14 Battery-powered targets defeat USB power cycle

For wearable / sealed targets (Nordic Rings, sealed sensors), removing USB
power doesn't reset the chip — it has its own battery and stays running.
"Reset by unplugging" is meaningless. Options:
- Pulse nRESET via `RPC_PIN_RESET` (requires the wire to be connected from
  the Pico's GPIO16 to the target's nRESET pin)
- Issue `RPC_RESET` (AIRCR SYSRESETREQ — works only if the core is
  attached + halted)
- Last resort: `RPC_ERASE_RECOVER` (CTRL-AP ERASEALL — wipes everything)

The app must understand which path applies to the target type before
offering a "reset" button.

Background: `reference_ring_battery_defeats_power_cycle.md`.

### 15.15 WAIT-ACK retry budget can exceed the USB transfer timeout

If the target stalls with `WAIT` ACKs (target busy / about to ship-mode), the
Pico firmware retries up to 8 times in-protocol. Each retry takes ~25 µs at
25 MHz SWD, so the retry budget is ~200 µs — well within USB timeout.

But: if the **DAP_Transfer batch is large** (e.g. 12 AHB-AP reads in one
packet) and the target stalls partway, the cumulative time can exceed the
ESP32-side USB transfer timeout (1 s in the reference). Symptom: ESP32 sees
USB timeout → returns `RPC_ERR_DAP` to the app even though the SWD link is
healthy.

**Mitigation:**
- Keep DAP_Transfer batches small (≤ 12 ops per §15.6) — this also bounds
  worst-case latency.
- Set the USB IN endpoint timeout to ≥ 2 s in the `uh_usbdap_xfer()` retry
  loop. The reference uses 1 s; if you see spurious DAP_ERRs on slow targets,
  bump it.
- Do NOT retry the USB transaction blindly — the Pico's WAIT path has
  already retried; another USB-level retry just doubles the wait without
  fixing anything.

Background: `reference_nrf5340_edev_vs_jlink_firmware_gap_2026_06_29.md`.

### 15.16 Erased-chip sleep trap — mass-erase makes Nordic unrecoverable via SWD

A mass-erased Nordic chip has **no firmware** — its boot ROM doesn't run any
user code, but it also doesn't keep the debug domain awake. On nRF5340 in
particular, an erased-and-released chip enters progressively deeper System
OFF; SWD returns `NO_ACK` and stays dead.

This is the most surprising operational gotcha in the protocol: `ERASE_FULL`
**works**, but the chip immediately afterward looks dead to SWD.

**The required flow** (the app must enforce this):
```
1. RPC_ERASE_RECOVER (0x0037)         ← erase succeeds
2. (chip is now "asleep" — SWD will NO_ACK)
3. RPC_RECOVER (0x0006)               ← line reset + re-attach
   …if RECOVER still NO_ACKs (likely):
4. Power-cycle the target externally  ← apply nRESET pulse, or unplug+replug if accessible
5. RPC_ATTACH → write the new firmware image as the first operation
   (the loader CALL wakes the core enough to keep it accessible)
```

**Anti-pattern:** "erase, then dump to verify it's erased". The dump
will fail with NO_ACK; users will think the erase failed. **Always flash
something within ~1 s of erase**, or warn the operator that a power-cycle
is required.

Background: `reference_nrf5340_erased_chip_sleep.md`.

### 15.17 There is NO APPROTECT bypass — only `ERASEALL`

A common request: "can the bridge unlock my chip without erasing flash?"
**No.** Nordic APPROTECT is enforced in silicon; the only way to clear it
is CTRL-AP `ERASEALL`, which **wipes everything** (flash + UICR + RAM).

- J-Link / nrfjprog have no APPROTECT bypass either.
- probe-rs / pyocd / OpenOCD have no APPROTECT bypass either.
- Our `edev_cmsis` has no APPROTECT bypass either.

The app must surface this clearly when the operator triggers `ERASE_RECOVER`
on a locked target: **"this will permanently destroy all flash contents,
including your firmware, calibration, and bonds — confirm?"**.

Background: `reference_segger_no_approtect_bypass_2026_06_25.md`.

### 15.18 Diagnostic patterns to recognize

Common failure modes the operator will hit; ship these as user-facing hints
in the app rather than raw error codes:

| Pattern | What's happening | What the app should say |
|---|---|---|
| Attach succeeds → first 1–4 `READ_MEM` succeed → then suddenly `NO_ACK` | Target firmware ship-moded its debug domain | "Target went to sleep — keep it on a charger and retry. Or issue `RESET_HALT` immediately after attach." |
| `ATTACH` returns `NO_ACK` immediately | Target unpowered, or wires not connected, or target locked (APPROTECT) | "Cannot reach target. Check wiring + power. If chip is locked, run Erase-and-Recover (destroys flash)." |
| `READ_MEM` returns `WAIT` for ≥ 1 s | AHB-AP transaction wall (§15.6) | "Internal: re-batching reads. (No user action.)" |
| `ERASE_RECOVER` succeeds, then `ATTACH` fails | Erased-chip sleep trap (§15.16) | "Erase done. Flash a firmware image now — the chip will sleep otherwise." |
| `DAP_INFO` returns garbage strings | Probe not actually CMSIS-DAP v2 (wrong VID/PID, or wedged) | "Probe identity unreadable. Try unplug + replug, or reboot bridge." |
| Bridge advertises but won't connect | iOS BLE bonding cache | "Forget the device in your phone's Bluetooth settings and retry." |
| All commands time out after a flash op | Bridge or Pico got into a bad state | "Reboot the bridge (issue RPC_REBOOT or power-cycle the ESP32)." |

These should drive the app's error UI — never show raw `RPC_ERR_DAP /
ack=7` to an operator.

Background: `reference_target_debug_power_pin.md`.

---

## 16. Build + flash + bring-up checklist

### Build

```bash
cd /Users/yogesh/projects/uh_firmware/uh_firmware/platformio/dap_ble_bridge  # or new edevkit copy
pio run -e esp32-s3-devkitc-1
```

First build pulls in NimBLE (~3 min on a fresh cache). Subsequent builds: <30 s.

### Flash

```bash
pio run -e esp32-s3-devkitc-1 -t upload
pio device monitor              # logs at 921600
```

### Smoke test — without the Pico

Power up the ESP32 alone. Expected serial banner:

```
# dap_ble_bridge — TLV up on serial + BLE 'DP_xxxxxxxxxxxx' (tools/dap_cli.py)
```

Scan from nRF Connect on a phone — confirm `DP_<MAC>` is advertising. Connect,
subscribe to `86f6d002`, write `0x02 0x00 0x01 0x00 0x00 0x00` (USB_STATUS,
seq=1, len=0) to `86f6d001` — expect a notify response of `0x82 0x00 0x01 0x00
0x05 0x00 0x01 0x00 0x00 0x00 0x00 0x00 0x00` (USB_STATUS|0x8000, seq=1, len=5,
status=1 NO_PROBE, connected=0, vid=0, pid=0).

### Smoke test — with the Pico 2 W plugged in

Plug the Pico 2 W into the ESP32's USB-OTG port. Within ~1 s expect:

```
# uh_usbdap: probe enumerated VID=0x2E8A PID=0x000C bcdDevice=0x0220
# uh_usbdap: bulk EP OUT=0x02 IN=0x82, packet size=512
```

Re-send `USB_STATUS` — now expect `status=0 OK, connected=1, vid=0x2E8A,
pid=0x000C`.

### Phase 2-5 validation

For each phase the user expects:

| Phase | Validation |
|---|---|
| 2 | `RPC_DAP_INFO` returns probe fw/product/serial; `RPC_TARGET_INFO` returns DPIDR + CPUID matching the target datasheet. |
| 3 | `RPC_FLASH_READ_FULL` on a known target → md5 of received dump equals md5 of the same dump done via `nrfjprog --readcode` (or pyocd / probe-rs / picotool). |
| 4 | `RPC_FLASH_ERASE_RANGE(0x10000, 0x1000)` → subsequent `RPC_READ_MEM(0x10000, 16)` returns 16 × `0xFF`. `RPC_ERASE_RECOVER` on a locked nRF → reconnect succeeds. |
| 5 | `RPC_FLASH_WRITE_FULL` of a known hex → `RPC_FLASH_READ_FULL` returns byte-identical bytes, target boots correctly on `RPC_RESET`. |

---

## 17. File / library map of the reference

The reference at
`/Users/yogesh/projects/uh_firmware/uh_firmware/platformio/dap_ble_bridge/` is
organized as PlatformIO libraries — copy the structure verbatim:

| Dir | Role |
|---|---|
| `src/main.cpp` | Setup + loop. Dispatches TLV frames from serial + BLE in parallel. ~100 LOC. |
| `lib/uh_usbdap/` | USB Host vendor-bulk client (talks CMSIS-DAP v2 over USB). Reads BOS for MS-OS 2.0 GUID match. |
| `lib/uh_dap/` | DAP / SWD / ADIv5. Line reset, DPIDR, AP_IDR, AHB-AP mem read/write, CTRL-AP for Nordic. |
| `lib/uh_dap_rpc/` | TLV ↔ uh_dap command dispatcher. **Extend HERE for the new edevkit commands** (§12). |
| `lib/uh_tlv/` | Streaming TLV reassembler + encoder. Don't modify. |
| `lib/uh_ble/` | NimBLE GAP + GATT + connection tuning. |
| `loader/` | Cortex-M flash loader (Thumb-2 assembly), built once, embedded as a byte array. |
| `docs/BLE_APP_INTEGRATION.md` | App-side integration guide (existing). |
| `docs/TLV_PROTOCOL.md` | Protocol spec (existing). |
| `docs/CMSIS_DAP_V2.md` | CMSIS-DAP v2 wire reference (existing). |
| `tools/dap_cli.py` | Host-side REPL — works over serial today, port to BLE for app prototyping. |

The edevkit-specific code lives entirely in:
- `lib/uh_dap_rpc/uh_dap_rpc.cpp` — new RPC opcodes for §12.
- `lib/uh_flash/uh_flash.{h,cpp}` (new) — the 40 KB read ring + external-flash
  staging logic for §13.

---

## 18. References

### Protocol specs

- **CMSIS-DAP v2 spec** — https://arm-software.github.io/CMSIS-DAP/latest/
- **ARM ADIv5** — IHI0031, freely downloadable from ARM
- **NimBLE-Arduino** — https://github.com/h2zero/NimBLE-Arduino
- **ESP-IDF USB Host Library** —
  https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32s3/api-reference/peripherals/usb_host.html

### edevkit-side references

- Pico 2 W firmware producing the CMSIS-DAP v2 USB endpoint —
  `/Users/yogesh/projects/temp/edevkit/edevkit/firmware/projects/edev_dapv2_zephyr/`
- App-side BLE library reference (NUS framing pattern) —
  `/Users/yogesh/projects/uh_firmware/uh_firmware/oot_modules/drivers/lib/uh_lib/`
- Existing dap_ble_bridge implementation (start here, copy and extend) —
  `/Users/yogesh/projects/uh_firmware/uh_firmware/platformio/dap_ble_bridge/`

### Vendor docs you'll need open during work

- Nordic nRF52840 PS (NVMC + CTRL-AP) — Section "Non-Volatile Memory
  Controller", Section "Access Port Protection"
- Nordic nRF5340 PS — same chapters; also "Inter-domain interaction" for the
  app-net AP arbitration
- RP2350 datasheet — Section 4 (USB), only relevant for the Pico 2 W side
- ESP32-S3 TRM — Section "USB OTG" for the host-mode signaling

---

## 19. What's intentionally NOT in this doc

- **App-side code** — that's the consumer of this bridge. Each app (Android /
  iOS / Web / a desktop tool) implements the TLV layer per
  `docs/BLE_APP_INTEGRATION.md`. No examples here on purpose; the protocol
  spec is the single source of truth.
- **Pico 2 W firmware changes** — none required. The Pico runs the existing
  `edev_dapv2_zephyr` and acts as a standard CMSIS-DAP v2 device.
- **Wi-Fi on the ESP32** — out of scope. NimBLE only. If Wi-Fi is ever needed
  (e.g. remote bridge over TCP), it's an additional `uh_tlv` transport on the
  same dispatch, not a rewrite.
- **Production OTA for the bridge itself** — out of scope. The bridge is a
  dev-bench tool; flash it from PlatformIO directly when firmware changes.

---

## Appendix A — Minimum viable Phase 1 deliverable

The smallest thing that proves the architecture end-to-end:

1. Build the reference firmware unmodified.
2. Flash to ESP32-S3.
3. Plug in a Pico 2 W running `edev_dapv2_zephyr`.
4. From nRF Connect on a phone: connect, subscribe to TX, send
   `RPC_USB_STATUS`. Expect `status=0, connected=1, vid=0x2E8A, pid=0x000C`.
5. Send `RPC_DAP_INFO`. Expect a response containing probe identity strings.
6. Send `RPC_CONNECT(8MHz)` → `RPC_ATTACH`. Expect DPIDR + AP_IDR.
7. Send `RPC_READ_MEM(0x00000000, 64)`. Expect the first 64 B of the target's
   flash.

If all of that works, every other Phase 2-5 command is incremental dispatcher
code on top of the same foundation.

---

## Appendix B — Starter project scaffold (file by file)

If you're starting from scratch instead of copying the reference, create
these files in this layout:

```
edev_bridge/
├── platformio.ini
├── partitions.csv
├── src/
│   └── main.cpp
├── lib/
│   ├── uh_tlv/        (uh_tlv.h + uh_tlv.c from §10)
│   ├── uh_ble/        (uh_ble.h + uh_ble.cpp — start with §9 skeleton)
│   ├── uh_usbdap/     (uh_usbdap.h + uh_usbdap.cpp — start with §6 skeleton)
│   ├── uh_dap/        (uh_dap.h + uh_dap.cpp — ADIv5; copy from reference)
│   ├── uh_dap_rpc/    (uh_dap_rpc.h + uh_dap_rpc.c — dispatcher per §11)
│   └── uh_flash/      (NEW — your 40 KB ring + staging logic for §13)
└── tools/
    └── test_ble.py    (Python bleak client from Appendix D)
```

### `platformio.ini`

```ini
[env:esp32-s3-devkitc-1]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino

monitor_speed = 921600
monitor_filters = esp32_exception_decoder

build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=0
    -DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
    -DCONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517
    -DCONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM=0
    -Os                               # the firmware is size-bound, not speed-bound

board_build.partitions = partitions.csv

lib_ldf_mode = deep+
lib_deps =
    h2zero/NimBLE-Arduino@^2.2.3
```

### `partitions.csv` — 8 MB flash, 3 MB app + 4 MB staging

```csv
# Name,    Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
app0,     app,  ota_0,   0x10000,  0x300000,
staging,  data, fat,     ,         0x400000,
```

Verify with: `pio run -v` (look for "Auto-detected: 8 MB flash" in the log).

### `src/main.cpp` — minimal skeleton

```cpp
#include <Arduino.h>
#include "esp_mac.h"
#include "uh_usbdap.h"
#include "uh_dap.h"
#include "uh_tlv.h"
#include "uh_dap_rpc.h"
#include "uh_ble.h"

static struct uh_tlv g_tlv_serial, g_tlv_ble;
static uint8_t       g_txbuf[UH_TLV_HDR + RPC_READ_MAX + 8];
static uint16_t      g_cur_seq;

static void serial_reply(uint16_t type, const uint8_t *val, uint16_t len, void *) {
    size_t n = uh_tlv_encode(g_txbuf, sizeof(g_txbuf), type, g_cur_seq, val, len);
    if (n) Serial.write(g_txbuf, n);
}
static void ble_reply(uint16_t type, const uint8_t *val, uint16_t len, void *) {
    size_t n = uh_tlv_encode(g_txbuf, sizeof(g_txbuf), type, g_cur_seq, val, len);
    if (n) uh_ble_send(g_txbuf, n);
}
static void dispatch(uint16_t type, uint16_t seq, const uint8_t *v, uint16_t l, uh_rpc_reply_fn rpl) {
    g_cur_seq = seq;
    if (type == RPC_REBOOT) { uint8_t s = RPC_OK; rpl(RPC_REBOOT | RPC_RESP_FLAG, &s, 1, NULL);
                              Serial.flush(); delay(150); ESP.restart(); }
    uh_dap_rpc_dispatch(type, v, l, rpl, NULL);
}
static void on_serial(uint16_t t, uint16_t s, const uint8_t *v, uint16_t l, void *) { dispatch(t,s,v,l, serial_reply); }
static void on_ble   (uint16_t t, uint16_t s, const uint8_t *v, uint16_t l, void *) { dispatch(t,s,v,l, ble_reply); }
static void ble_rx(const uint8_t *d, size_t n) { uh_tlv_feed(&g_tlv_ble, d, n); }

void setup() {
    Serial.begin(921600); delay(300);
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
    char name[24], serial[40];
    snprintf(name,   sizeof(name),   "DP_%02X%02X%02X%02X%02X%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    snprintf(serial, sizeof(serial), "EDV-BRIDGE-%02X%02X%02X%02X%02X%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    uh_usbdap_init();
    uh_dap_init(uh_usbdap_xfer);
    uh_tlv_init(&g_tlv_serial, on_serial, NULL);
    uh_tlv_init(&g_tlv_ble,    on_ble,    NULL);

    uh_ble_set_name(name);
    uh_ble_set_dis("edevkit", "EDV-BRIDGE", "R0.01.00.00", "01.00.00.01", serial);
    uh_ble_begin(ble_rx);

    Serial.printf("# edev_bridge — TLV up on serial + BLE '%s'\n", name);
}

void loop() {
    uint8_t buf[256]; int got = 0;
    while (Serial.available() && got < (int)sizeof(buf)) buf[got++] = Serial.read();
    if (got) uh_tlv_feed(&g_tlv_serial, buf, got);
    uh_ble_poll();
    if (!got) delay(1);
}
```

That's a complete firmware. The `lib/uh_*` libraries are the work — start
with `uh_tlv` and `uh_ble` (they're transport-only, easy to validate), then
`uh_usbdap`, then `uh_dap`, then `uh_dap_rpc`. Each builds on the previous.

---

## Appendix C — ESP32-S3 USB pinout + sdkconfig essentials

### USB pin assignments — DO NOT CHANGE

The ESP32-S3's USB-OTG controller is **hardwired** to GPIO19 and GPIO20:

| GPIO | USB signal | Notes |
|---|---|---|
| 19 | D− | Pad type 0/1 (fixed). Cannot remap. |
| 20 | D+ | Pad type 0/1 (fixed). Cannot remap. |

If you accidentally route a signal onto GPIO19/20 in your design, host mode
breaks silently — the OTG PHY still tries to drive D+/D− and the bus floats.

VBUS for host-mode is sourced from the dev board's onboard 5V switch. On the
`esp32-s3-devkitc-1`, the OTG USB-C port both **accepts** VBUS (when ESP32 is
self-powered) and **sources** it (when ESP32 is host) — the on-board USB
power switch handles the direction.

### Other GPIOs the bridge claims

| GPIO | Use | Why |
|---|---|---|
| 19, 20 | USB-OTG D−/D+ | Host link to Pico 2 W (above) |
| 43, 44 | UART0 TX/RX | Serial console at 921600, via on-board USB-UART bridge |
| 0, 3, 45, 46 | Strapping pins | DO NOT use — boot strapping. ESP32-S3 boots from flash only when 0=high, 45/46 in default state. |
| 48 | Onboard WS2812 RGB LED | Optional status indicator |
| All others | Free | Use for future expansion |

### sdkconfig values that matter

The pioarduino build derives most of these from Kconfig defaults, but **the
following four are non-negotiable** for this project:

```ini
# Add to build_flags in platformio.ini (Arduino doesn't expose sdkconfig directly)
build_flags =
    -DCONFIG_USB_OTG_SUPPORTED=1                  # enables the host stack at runtime
    -DCONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517      # request max MTU from clients
    -DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=1          # only the one app talks
    -DCONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM=0        # no L2CAP CoC — saves heap

    # Heap tuning — the 40 KB read ring + NimBLE stack + USB host
    -DCONFIG_ESP_MAIN_TASK_STACK_SIZE=8192        # 4 KB default is too small
    -DCONFIG_FREERTOS_HZ=1000                     # 1 ms timer granularity for backpressure
```

### Power budget — what to design for

| Component | Typical current @ 5V | Peak |
|---|---|---|
| ESP32-S3 (Wi-Fi off, BLE TX at 0 dBm) | 60 mA | 120 mA |
| ESP32-S3 (BLE TX at +9 dBm peak) | — | 320 mA peak (transient, ms) |
| Pico 2 W (CMSIS-DAP active, no target) | 30 mA | 80 mA |
| Pico 2 W + powered SWD target sourcing Vtgt | varies | up to 500 mA total |

If the **ESP32 sources VBUS to the Pico** (the usual case), the upstream USB
host (your dev PC, or a 5V power supply on the UART USB-C) must supply
≥500 mA. A standard PC USB port is fine; a low-power wall-wart USB-A adapter
may not be. Symptom of insufficient VBUS: Pico 2 W enumerates but resets
intermittently mid-DAP-transaction.

---

## Appendix D — Python test client (`bleak`)

Run this from a Mac/Linux machine with `bleak` installed (Windows works too
but requires admin to scan). It speaks the full TLV protocol — useful for
validating any new command before you build an app around it.

```python
#!/usr/bin/env python3
# tools/test_ble.py — exercise the bridge over BLE without an app.
import asyncio, struct, sys
from bleak import BleakScanner, BleakClient

DAP_SVC = "86f6d000-f706-58a0-95b2-1fb9261e4dc7"
DAP_RX  = "86f6d001-f706-58a0-95b2-1fb9261e4dc7"
DAP_TX  = "86f6d002-f706-58a0-95b2-1fb9261e4dc7"

# TLV ops
def frame(typ, seq, value=b""):
    return struct.pack("<HHH", typ, seq, len(value)) + value

def parse(buf):
    if len(buf) < 6: return None
    typ, seq, ln = struct.unpack_from("<HHH", buf, 0)
    if len(buf) < 6 + ln: return None
    return typ, seq, buf[6:6+ln], buf[6+ln:]

async def main():
    print("scanning for DP_…")
    devices = await BleakScanner.discover(timeout=4.0)
    target = next((d for d in devices if d.name and d.name.startswith("DP_")), None)
    if not target:
        print("no DP_ device found"); sys.exit(1)
    print(f"found {target.name} ({target.address})")

    rx_buf = bytearray()
    response = asyncio.Event()
    last = {}

    def on_notify(_, data):
        rx_buf.extend(data)
        while True:
            r = parse(rx_buf)
            if r is None: break
            typ, seq, val, tail = r
            rx_buf[:] = tail
            last['typ'], last['seq'], last['val'] = typ, seq, val
            response.set()

    async with BleakClient(target, timeout=15) as cli:
        # Important: subscribe BEFORE writing any command, else responses are dropped
        await cli.start_notify(DAP_TX, on_notify)
        try:
            await cli.exchange_mtu(517)
        except Exception:
            pass

        seq = 0
        async def call(typ, value=b"", timeout=5.0):
            nonlocal seq
            seq = (seq + 1) & 0xFFFF
            response.clear()
            await cli.write_gatt_char(DAP_RX, frame(typ, seq, value), response=False)
            await asyncio.wait_for(response.wait(), timeout)
            return last['val']  # value bytes of the response

        # Phase 1 smoke test
        v = await call(0x0002)                     # USB_STATUS
        st, conn, vid, pid = v[0], v[1], int.from_bytes(v[2:4],'little'), int.from_bytes(v[4:6],'little')
        print(f"USB_STATUS  status={st} connected={conn} VID={vid:#06x} PID={pid:#06x}")

        v = await call(0x0010)                     # DAP_INFO
        print(f"DAP_INFO    bytes={len(v)}  status={v[0]}")

        if conn:
            await call(0x0011, struct.pack("<I", 8_000_000))    # CONNECT @8 MHz
            v = await call(0x0014)                              # ATTACH
            dpidr = int.from_bytes(v[1:5],'little')
            ap_idr = int.from_bytes(v[5:9],'little')
            print(f"ATTACH      DPIDR={dpidr:#010x} AP_IDR={ap_idr:#010x}")

            v = await call(0x0020, struct.pack("<II", 0, 64))   # READ_MEM 0..64
            print(f"READ_MEM    status={v[0]}  first 16 B = {v[1:17].hex()}")

asyncio.run(main())
```

Run:
```bash
python3 tools/test_ble.py
```

Expected output on a working bridge with Pico + nRF52840 target:
```
scanning for DP_…
found DP_AABBCCDDEEFF (AA:BB:CC:DD:EE:FF)
USB_STATUS  status=0 connected=1 VID=0x2e8a PID=0x000c
DAP_INFO    bytes=64  status=0
ATTACH      DPIDR=0x2ba01477 AP_IDR=0x24770011
READ_MEM    status=0  first 16 B = 00800020c1000000…
```

If any line shows a status ≠ 0, see Appendix E.

---

## Appendix E — Troubleshooting

### Build / flash

| Symptom | Likely cause | Fix |
|---|---|---|
| `pio run` says "platform-espressif32: not found" | Plain espressif32 platform pinned IDF 4; missing USB Host API | Install pioarduino fork (§5 prerequisites) |
| `error: 'usb_host.h' file not found` | Building against stock IDF 4 | Same — switch to pioarduino |
| Flash succeeds but no serial output | Wrong USB-C port connected; OTG port has no UART | Connect the UART port (the one near the CP2102/CH343 chip) |
| `A fatal error occurred: Could not connect to the device` | Wrong serial port | `pio device list` to find the right `/dev/cu.usbserial-*` |
| Image too big — partition overflow | App grew past 1 MB default | Add `board_build.partitions = partitions.csv` per Appendix B |

### USB host (probe enumeration)

| Symptom | Likely cause | Fix |
|---|---|---|
| Pico plugged in but no `# uh_usbdap: probe enumerated` log | OTG D+/D− not connected (wrong USB-C port on ESP32) | Use the OTG port, not the UART port |
| `# uh_usbdap: probe enumerated` then immediately `DEV_GONE` | VBUS insufficient — host PC's USB doesn't supply enough | Use a powered USB hub, or power ESP32 from a wall adapter |
| Enumerates with wrong VID/PID | Plugged in the wrong device | Confirm Pico 2 W is running `edev_dapv2_zephyr` (expects VID=0x2E8A PID=0x000C) |
| Enumerates but `usb_host_interface_claim` fails | Probe descriptor missing the vendor (0xFF) interface | Check that the Pico firmware actually exposes CMSIS-DAP v2 (probe with `lsusb -v -d 2e8a:` on Linux) |

### BLE (advertising / connection)

| Symptom | Likely cause | Fix |
|---|---|---|
| `DP_…` not visible in nRF Connect | BLE never started; check serial log for NimBLE errors | Verify `lib_deps` includes NimBLE-Arduino ≥ 2.2.3 |
| Connects but disconnects immediately | iOS bonding mismatch — bridge has no pairing, app insists on pairing | Remove the device from the phone's BT settings; reconnect (the bridge advertises with no security) |
| MTU stuck at 23 | App didn't call `requestMtu(517)` after services discovered | App side bug — see app integration guide |
| Notifications never arrive but commands appear to be accepted | App didn't subscribe to TX CCCD before writing RX | Subscribe FIRST, write SECOND |
| Disconnect right after MTU exchange | iOS err 133 — too many GAP procedures stacked | Verify the staggered 450 ms gap between updateConnParams / updatePhy / setDataLen (§9 tuning sequence) |

### RPC / target

| Symptom | Likely cause | Fix |
|---|---|---|
| `USB_STATUS` always returns `connected=0` | USB enumeration didn't complete | Check the USB section above |
| `ATTACH` returns `status=4 (DAP_ERR), dap_ack=7 (NO_ACK)` | Target not present, or target debug domain powered down | Verify SWD wiring; if target is wearable / battery powered, plug in charger |
| `ATTACH` returns `status=4, dap_ack=4 (FAULT)` | Sticky error on the DP — usually a previous wedge | Issue `PROBE_REINIT` (`0x0004`) and retry |
| `READ_MEM` returns OK but data is `0xFF`s | Reading from an erased region, or unmapped address | Verify the address matches the target's flash map |
| **First 1–4 `READ_MEM` succeed, then NO_ACK forever** | **Target firmware ship-moded after attach (§15.18)** | **Issue `RESET_HALT` immediately after `ATTACH`, before any long read; keep target powered** |
| `READ_MEM` succeeds for 48 B then hangs with WAIT | AHB-AP transaction wall (§15.6) — batch overflow | Batch reads in groups of ≤12 ops with CTRL-AP soft-reset between |
| `READ_MEM` returns NO_ACK partway through a long read | Target went to sleep mid-read | Issue `RESET_HALT` after `ATTACH`; ensure WDT feed (§15.1) is enabled |
| `ERASE_RECOVER` succeeds but `READ_MEM(0)` still returns the old image | The chip rebooted with the protection re-armed; need to re-attach | After ERASE_RECOVER: `PROBE_REINIT` → `ATTACH` → re-read |
| `ERASE_RECOVER` succeeds, but every subsequent op returns NO_ACK | **Erased-chip sleep trap (§15.16)** | **Flash a firmware image within ~1 s of erase, or power-cycle target before retry** |
| `READ_MEM` returns garbage on locked nRF5340 specifically | The chip is locked AND someone tried `probe-rs --allow-erase-all` which silently wiped it | §15.10 — use `pyocd` or `uhocd` direct AHB-AP reads on nRF5340, never probe-rs's auto-erase |
| Bridge says `ATTACH` failed but DPIDR comes back nonzero | Locked chip — DP is reachable, AHB-AP isn't | Use `RPC_TARGET_IDENT` to read `DP.TARGETID` (G.9) to identify the locked chip; offer `ERASE_RECOVER` as the next step |
| User asks "can I unlock without erasing?" | APPROTECT is silicon-enforced | §15.17 — only path is `ERASE_RECOVER` which destroys flash. No tool has a bypass. |

### Pico-side recovery (when the probe itself is wedged)

The ESP32 isn't always the problem. If `RPC_REBOOT` doesn't clear a probe
wedge:

| Symptom | Likely cause | Fix |
|---|---|---|
| Pico stops responding entirely; USB doesn't re-enumerate | Pico firmware crashed (Zephyr fault, infinite loop) | Hold BOOTSEL while plugging Pico into a dev PC → mounts as a UF2 mass-storage device → drag-drop a known-good firmware image |
| Pico enumerates but every DAP packet returns garbage | Wedged Pico USB state | `picotool save -p -v current_fw.uf2` from BOOTSEL, then `picotool load -ux known_good.uf2` |
| Need to dump current Pico firmware before flashing new | Diagnostic preserve | Hold BOOTSEL → mount → `picotool save -p -v firmware.uf2` |
| 1200-baud trick to force BOOTSEL | Pico firmware exposes the magic baud rate | `stty -f /dev/cu.usbmodem* 1200` on macOS, equivalent on Linux. The Pico's `bootsel_watch` thread sees the 1200-baud line coding and triggers `reset_usb_boot()` |

For wedged Pico firmware on the bench: connect a USB-A → USB-C cable to a
dev PC and use the 1200-baud trick. For a Pico embedded in a kit where
the BOOTSEL button isn't accessible, the 1200-baud trick is the only path.

Background: `reference_picotool_save_bootsel.md`.

### Performance

| Symptom | Likely cause | Fix |
|---|---|---|
| Flash dump is slow (< 1 KB/s) | MTU stayed at 23 or PHY stayed at 1M | Confirm the staggered tuning sequence ran (look at serial log for "MTU=517", "PHY 2M") |
| ESP32 reboots mid-flash-write | Heap exhausted by 4 MB staging buffer | The staging partition is on flash, not heap — if you accidentally `malloc()`'d a multi-MB buffer, fix that |
| BLE notifications appear delayed (seconds, not ms) | Power-save mode on the phone backgrounded the BLE app | Keep the app foregrounded during bulk ops |

---

## Appendix F — What to build first if you're starting from scratch

If you can't / don't want to copy the reference, build in this order. Each
step is a working firmware on its own that you can flash and verify.

| # | Build | Validates |
|---|---|---|
| 1 | `main.cpp` that just blinks LED at 1 Hz over `Serial.print` | toolchain, flash, serial monitor |
| 2 | + `uh_tlv` library + a unit test in `setup()` that encodes a frame and feeds it back to the decoder | TLV round-trips |
| 3 | + `uh_ble` from §9 — advertise `DP_xxx`, echo any RX byte back as a TX notify | NimBLE works; phone can connect and see notifications |
| 4 | + `uh_usbdap` from §6 — log probe enumeration; expose a stub `RPC_USB_STATUS` that uses real probe state | USB host enumeration works |
| 5 | + `uh_dap` (port from reference; ADIv5 is mature, don't rewrite) — wire `RPC_DAP_INFO`, `CONNECT`, `ATTACH` | full SWD chain works |
| 6 | + `RPC_READ_MEM` / `WRITE_MEM` | full reference-equivalent functionality |
| 7 | + edevkit Phase 2-5 commands (§12) + `uh_flash` buffering (§13) | everything in this doc |

Each step is mergeable to `main`. Don't try to land step 7 in one go.

---

## Appendix G — `edev_cmsis` firmware quirks (what the Pico side actually does)

The Pico 2 W runs a customized CMSIS-DAP v2 firmware (`edev_dapv2_zephyr` in
the edevkit tree; previously `edev_dapv2` on pico-sdk). It is **mostly**
spec-compliant, but it has properties the ESP32 must know about. Each entry
below references the project-memory note that documents the underlying
finding.

### G.1 USB descriptors

| Field | Value | Why it matters |
|---|---|---|
| VID:PID | `0x2E8A` : `0x000C` | Use as a positive identification filter |
| bcdDevice | `0x0220` | Set at runtime by `usbd_device_set_bcd_device()` — **probe-rs gates discovery on this**; if it's < 0x0220 the host won't list the probe |
| Interface class | `0xFF` (vendor) | Standard CMSIS-DAP v2 — claim with `usb_host_interface_claim(itf, 0)` |
| Endpoints | bulk OUT then bulk IN | Both at 512 B MPS (USB-FS) |
| MS OS 2.0 BOS | CMSIS-DAP v2 standard GUID `{CDB3B5AD-…}` | Lets Windows hosts auto-bind WINUSB; also a stable secondary filter |

Background: `reference_probe_rs_bcd_device_gate.md`.

### G.2 Capability byte (`DAP_Info[0xF0]`)

`edev_cmsis` advertises `caps = 0x13` — bits set:
- **bit 0 (0x01)**: SWD supported
- **bit 1 (0x02)**: JTAG supported (the bit-bang driver wires it; Zephyr port keeps it on)
- **bit 4 (0x10)**: Atomic Commands supported

Bits NOT set (intentionally):
- **bit 2 (0x04)** SWO UART capture — not implemented yet
- **bit 3 (0x08)** SWO Manchester — not implemented
- **bit 5 (0x20)** Test Domain Timer — not implemented

The ESP32's `DAP_INFO` response can return the raw caps verbatim from
`DAP_Info[0xF0]`. If the app tries to enable SWO before that capability ships,
the bridge should return `ARGS` with a clear message.

Background: `project_uh_dapv2_caps_byte.md`.

### G.3 Vendor commands (DAP `0x80–0x9F`) the Pico exposes

Definitive vendor command table for the **`edev_dapv2_zephyr`** firmware.
This supersedes any older draft that listed pico-sdk legacy commands —
those (CORTEX_M_HALT/DUMP/REG_READ/RESUME, NRF_RESET, SWCLK_KEEPALIVE)
are **NOT** in the Zephyr port. The Zephyr port re-uses the 0x80–0x9F
space for the Nordic recover/erase/write/read primitives.

| Cmd | Name | Status (v0.1-step5) | What it does | Wire response (bytes) |
|---|---|---|---|---|
| `0x84` | `NRF53_RECOVER`            | ✅ shipped | Full Nordic unlock — composes ERASEALL both cores, App UICR program, Net UICR via on-target stub | `[0x84, status, ap_count, u32 app_approt, u32 app_secure, u32 net_marker, u32 net_approt]` (19) |
| `0x85` | `NRF53_ERASE`              | ✅ shipped | CTRL-AP IDR scan + ERASEALL each Nordic CTRL-AP found | `[0x85, status, ap_count]` (3) |
| `0x86` | `NRF53_FLASH_WRITE_NET`    | ⏳ step 6 (returns `0xFF`) | Probe-side Net flash programming (direct AP#1 + Net NVMC, inline verify) | future |
| `0x87` | `NRF53_FLASH_WRITE_APP`    | ⏳ step 7 (returns `0xFF`) | Probe-side App flash programming via RAM-loaded CMSIS Flash Algorithm | future |
| `0x88` | `NRF53_READ_MEM`           | ⏳ step 8 (returns `0xFF`) | Direct AHB-AP read with `DEMCR.VC_CORERESET` clear (fixes the App-readback-0xFF bug 6a) | future |
| `0x89` | `NRF53_TARGET_INFO`        | ⏳ later (returns `0xFF`) | Chip identification — DPIDR + AP_IDR + FICR.PART per family | future |
| `0x8A` | `NRF53_UICR_PROGRAM_APP`   | ✅ shipped | Host-side App UICR.APPROTECT + SECUREAPPROTECT writes (CSW=0x23000002) | `[0x8A, status, u32 approtect, u32 secureapprotect]` (10) |
| `0x8B` | `NRF53_UICR_PROGRAM_NET`   | ✅ shipped | On-target Net stub: programs Net UICR.APPROTECT from Net CPU side after CTRL-AP#3 RESET | `[0x8B, status, u32 sram_marker, u32 net_approtect]` (10) |
| `0x80–0x83`, `0x8C–0x9F` | unused | (returns `0xFF`) | Reserved space; not handled by the Zephyr port | `[0xFF]` (1) |

**Status byte values** (mirror of `nrf53_status_t` in `nrf53.h`):

| status | meaning |
|---|---|
| `0` | OK |
| `1` | WAIT — SWD WAIT-ACK retry budget exhausted |
| `2` | FAULT — SWD ACK=FAULT (sticky bit set) |
| `3` | NO_ACK — line dead or target debug domain off |
| `4` | PROTO — driver / parity / framing error |
| `5` | TIMEOUT — poll loop timeout |
| `6` | ARGS — caller passed bad arguments |
| `7` | NO_DEV — SWDP device not bound |
| `8` | STUB_FAIL — Net stub didn't write the success marker |

All multi-byte fields are **little-endian**.

**Expected values for the magic readbacks** (on success):
- `app_approtect`, `app_secureapprotect`, `net_approtect` → `0x50FA50FA`
- `net_marker` → `0xDEADC0DE` (success) or `0xBADF00D5` (stub fault — PC + xPSR logged probe-side)

**Migration path:** the bridge can be written today against the ✅-shipped
commands plus standard DAP_Transfer for everything else. As step 6/7/8/9
ship over subsequent releases, the bridge can replace its DAP_Transfer
loops with the higher-level vendor commands — each migration is a
1-call substitution.

Background: `project_edev_dapv2_zephyr_v01_step5_release_2026_06_30.md`
(the release this table reflects), `docs/NRF5340_ALGORITHMS.md` (algorithm
spec), and `releases/v0.1-step5/RELEASE.md` (release notes).

### G.4 Nordic FICR / chip-ID addresses per family

For the optional `RPC_TARGET_IDENT` (§12.5), the bridge needs to know where
to find the part number on each Nordic family:

| Family | FICR base | INFO.PART offset | Notes |
|---|---|---|---|
| nRF52 (51840, 52833, …) | `0x10000000` | `+0x100` (= `0x10000100`) | Read 4 B; e.g. `0x0000_52840` |
| nRF53 App | `0x00FF0000` | ⚠ unreliable — see note below | Different base from nRF52 |
| nRF53 Net | `0x01FF0000` | ⚠ unreliable — see note below | Net core has its own FICR |
| nRF91 | `0x00FF0000` | ⚠ unconfirmed | Layout assumed similar to nRF53 |

> **⚠ nRF53 FICR caveat (verified 2026-06-30 against bench nRF5340 DK):**
> Reading FICR.INFO.PART at the documented `+0x140` offset returns
> `0xFFFFFFFF` via the App AHB-AP on this silicon — possibly engineering
> sample with unprogrammed FICR, possibly wrong offset for this variant
> (different Nordic docs cite `+0x314`). Until verified against a Ring
> Pro 351 production target, treat nRF53 FICR readbacks as best-effort.
> The bridge can identify the chip reliably via `0x89 NRF53_TARGET_INFO`
> (DPIDR + AP_IDR + CPUID) without needing FICR.
>
> **Recommendation:** for chip subtype identification beyond family
> (nRF52840 vs 52833 vs 52832, etc.), probe FICR.INFO.PART at multiple
> candidate offsets via `0x88 NRF53_READ_MEM` and treat `0xFFFFFFFF` as
> "subtype unknown, fall back to DPIDR + CPUID family detection".
| nRF54L | `0x00FFD000` | `+0x300` | Different base again |
| nRF54H | — | — | IronSide-locked; bridge cannot read without DK debug auth |

The bridge auto-detects family by:
1. Reading `DPIDR.DESIGNER` — 0x144 = Nordic
2. Reading `AP_IDR` — distinguishes Cortex-M0 (nRF52) from M33 (nRF53/54)
3. Reading `CPUID.PARTNO` — finer-grain (M33 vs M33-with-FP, etc.)
4. Then reading the family-appropriate FICR addr above

Background: `reference_chip_identification_swd.md`,
`reference_nrf_family_reset_recover.md`.

### G.5 Performance baseline (what to expect)

Measured against the current Zephyr port on a stable USB-host setup:

| Operation | Time | Throughput |
|---|---|---|
| Full 1 MB nRF52840 flash read | ~16 s | ~64 KB/s |
| Full 0.7 MB nRF5340 (App+Net) flash read | ~42 s | ~17 KB/s (net core IPC overhead) |
| Single 4 KB `READ_MEM` over USB | ~80 ms | ~50 KB/s |
| Single 4 KB `READ_MEM` over BLE (MTU 517, 2M PHY) | ~600 ms | ~7 KB/s |

The BLE link is the bottleneck for bulk reads (~10× slower than USB).
Don't promise faster than ~10 KB/s sustained over BLE.

Background: `project_edev_dapv2_pio_upgrade_2026_06_25.md`,
`project_edev_dapv2_nrf5340_full_flash_2026_06_26.md`.

### G.6 Posted-AP-read pipeline — already done upstream

Zephyr's `subsys/dap/cmsis_dap.c::dap_swdp_transfer()` (lines 467–579)
already implements ADIv5's posted-AP-read pipeline correctly: tracks a
`post_read` flag, returns the previous AP value on the next AP read, flushes
via DP.RDBUFF on DP intervention or end-of-batch.

**The ESP32 must NOT re-implement this pipeline.** Send the host-side
register list verbatim to DAP_Transfer; read back the same number of words.
The reference `uh_dap.cpp` already does this correctly — don't refactor.

Background: `NOTES.md` in the Zephyr port (audit findings 2026-06-29).

### G.7 SWD wire-speed fixes baked into the Pico firmware

For reference (the ESP32 doesn't implement these — but knowing they exist
helps when debugging "why does my flash read return garbage when I bypass
the Pico"):

1. **SWDIO updates after the falling SWCLK edge** — hold-time fix in
   `pio/probe_swd.pio`. Wrong polarity = intermittent CRC errors.
2. **WAIT/FAULT data-phase asymmetry** — on read+WAIT, target drives 33
   dummy cycles; on write+WAIT, host drives 33 zeros. Asymmetric.
3. **Parity-error retry budget = 8 attempts** — if you ever see a `WAIT`
   ack burst that exceeds 8, the link is dead, not just busy.

All three live in `firmware/projects/edev_dapv2_zephyr/drivers/dp/`. The
ESP32 talks DAP_Transfer (one level above), so these are invisible.

Background: `project_edev_dapv2_three_swd_fixes_2026_06_25.md`,
`project_edev_dapv2_pio_upgrade_2026_06_25.md`.

### G.8 Recovery procedure when the SWD link wedges

The order matters. From most-likely-to-fix to nuclear:

```
1. RPC_PROBE_REINIT    (0x0004)  — DAP disconnect → connect → SWJ_Clock → line reset
2. RPC_ATTACH          (0x0014)  — re-walk DPIDR/AP_IDR
3. RPC_PIN_RESET       (0x0036)  — pulse nRESET (needs wire; not always present)
4. RPC_RECOVER         (0x0006)  — line reset + re-attach (escalation of step 1)
5. RPC_ERASE_RECOVER   (0x0037)  — CTRL-AP ERASEALL (destroys all flash)
6. RPC_REBOOT          (0x0005)  — ESP self-restart, full USB+BLE re-init
```

The app should expose these as a "diagnostics" menu rather than firing them
in sequence — step 5 destroys flash, which the operator must confirm.

Background: `reference_nrf5340_erase_recover_write_canonical_2026_06_26.md`,
`project_edev_dapv2_nrf5340_recover_button_2026_06_26.md`.

### G.9 Locked-chip identification — `DP.TARGETID` fallback

A locked chip with APPROTECT enabled returns NO_ACK on AHB-AP transactions
— `READ_MEM(0x10000100)` to fetch the Nordic FICR.INFO.PART fails. The
identity then has to come from the DP layer (which is always reachable):

| Source | Address | Tells you |
|---|---|---|
| `DP.DPIDR` (read 0x00) | DP | Designer (`0x144` = Nordic), revision, version. Always readable. |
| `DP.TARGETID` (read 0x24 after SELECT.DPBANKSEL=2) | DP | TINSTANCE + TPARTNO + TDESIGNER. Identifies the chip family even when locked. |
| `DP.DLPIDR` (read 0x34 after SELECT.DPBANKSEL=3) | DP | Multi-drop instance ID. Distinguishes cores on the same wire. |
| ARM ROM table at `0xE00FE000` | AHB-AP | Once unlocked, distinguishes M33-with-FP from plain M33, etc. |

The bridge's `RPC_TARGET_IDENT` (§12.5) should fall back to `DP.TARGETID`
when AHB-AP is wedged, and surface "locked" as a separate state from
"unknown" in the response. The app can then offer Erase-and-Recover as the
explicit next step.

Background: `reference_chip_identification_swd.md`.

### G.10 SWD-only reset paths (4 ways without nRESET wire)

If the target's nRESET pin is not connected (common on sealed wearables),
software-only reset is still possible. The bridge should pick the highest-
in-the-list option that succeeds:

| # | Method | Mechanism | Limitation |
|---|---|---|---|
| 1 | **AIRCR SYSRESETREQ** | Write `0x05FA0004` to AIRCR (0xE000ED0C) | Requires attach + halt; doesn't reset peripherals |
| 2 | **Nordic CTRL-AP RESET** | Write 1 then 0 to CTRL-AP RESET register | Nordic-only; works on locked chips |
| 3 | **CTRL-AP ERASEALL + recover** | §15.16 | Destroys flash; last resort |
| 4 | **Dormant→active wakeup** (line reset variant) | 8 × `0xFF` + 4-bit selection + DPIDR read | Only resets the DP, not the core |

The reference `uh_dap` exposes (1) as `RPC_RESET`, (2) as part of
`RPC_RECOVER`, (3) as `RPC_ERASE_RECOVER`, (4) implicitly in `RPC_ATTACH`'s
line reset. The bridge composes these into recovery sequences (G.8); the
app sees only high-level RPCs.

Background: `reference_swd_only_reset_paths.md`.

### G.11 CMSIS Flash Algorithm pattern (the *only* sleep-resistant flash write)

Direct AHB-AP writes to NVMC registers (the naive way to write flash) are
slow AND blocked by the SPU on TF-M targets. The portable solution is the
CMSIS "Flash Algorithm" pattern, also used by J-Link, nrfjprog, pyocd, and
probe-rs:

```
1. Host downloads a small (~256 B) Thumb-2 routine into target RAM
2. Sets target's R0..R3 with arguments (addr, src, len, etc.)
3. Sets PC to the entry point, LR to a BKPT instruction
4. Resumes core → core runs the routine ON-TARGET → BKPTs back to halt
5. Host reads R0 for return code
```

The routine runs **on the target's core**, with the SPU/TZ context the core
already has. It does NOT need SWD to reach NVMC — NVMC is just another
memory-mapped peripheral as far as the running core is concerned. This is
**the only way** to flash a TF-M nRF5340 (where SPU blocks AHB-AP writes to
secure flash).

The flash loader source is `loader/nrf_flash_loader.S` in the reference;
build with `loader/build.sh`. The bridge embeds the loader as a byte array
in `lib/uh_dap_rpc/`; `RPC_FLASH_WRITE_*` commands load it on demand.

Background: `reference_cmsis_flash_algorithm.md`.

---

## Appendix H — Reference Python tools (mirror these behaviors)

When implementing the ESP32 `uh_dap_rpc` layer, **the canonical reference for
correct semantics is `uhocd`** — our pyocd fork that has all the Nordic
workarounds. The ESP32 firmware needs to mirror its protocol-level behavior
(retry, batching, recovery sequences) — not its Python code, just its
behavior. Where the doc above is ambiguous, `uhocd` is the tiebreaker.

### H.1 `uhocd` — pyocd fork (the spec-by-example for SWD/DAP semantics)

- **Repo**: `uhocd` (fork of pyocd v0.44.1 at `uh-dapv2-vendor` branch). Lives
  at `~/.uhocd-venv/` as an editable Python install in the project memory.
- **What it adds vs upstream pyocd**:
  - AHB-AP transaction batching (§15.6) — `Batch ≤ 12 reads, then CTRL-AP reset`
  - WDT auto-feed loop (Nordic magic `0x6E524635`)
  - SWCLK keep-alive emulation in software (when the probe doesn't do it)
  - nRF5340 multidrop SWD handling (avoids TARGETSEL contention)
  - Nordic CTRL-AP recovery flows for nRF52/53/91 with the correct family-specific offsets
  - The vendor commands listed in Appendix G.3 (Cortex-M dump, etc.)
  - `dump-mem` / `dump-flash` subcommands
- **Why mirror it**: the ESP32 needs the same workarounds. The protocol the
  app speaks to the ESP32 (TLV) is one level above DAP; the ESP32's `uh_dap`
  layer is the equivalent of `uhocd`'s SWD/DAP layer in Python. Same logic,
  different language.
- **Validate-by-comparison**: when developing the ESP32, run `uhocd` against
  the same Pico over the same USB cable, compare results byte-for-byte. If
  `uhocd dump-flash` matches the ESP32 bridge's `RPC_FLASH_READ_FULL`, the
  ESP32 logic is right.

Project memory: `project_uhocd_fork.md`, `project_uhocd_validated_2026_06_19.md`.

### H.2 `webgui` — FastAPI :8766 (reference web client for the probe)

- A FastAPI server that wraps `uhocd` + `probe-rs` + `pyocd` and exposes the
  same operations the bridge offers (`USB_STATUS`, `DAP_INFO`, `READ_MEM`,
  `WRITE_MEM`, `ERASE_RECOVER`, etc.) over HTTP at `http://127.0.0.1:8766`.
- **Why it matters for ESP32 dev**: when building a phone app against the
  ESP32, you can also point the same app at `webgui` over HTTP for
  side-by-side regression. If `webgui` returns 4 KB at addr X and the ESP32
  bridge returns different bytes, the ESP32 has a bug.
- Validated end-to-end (all panels work) per `project_edev_dapv2_webgui_2026_06_25.md`.

### H.3 `dap_cli.py` — host-side TLV REPL

Lives at `/Users/yogesh/projects/uh_firmware/uh_firmware/platformio/dap_ble_bridge/tools/dap_cli.py`.
A Python REPL that speaks the **same TLV protocol** the ESP32 bridge does,
over serial (today) and BLE (the Python `bleak` client in Appendix D is its
BLE sibling). Useful for:
- Manual exploration: `dap> read 0 64`
- Regression: scripted test suites in CI
- Comparing ESP32 bridge responses against `uhocd`'s direct output

### H.4 `webdbg` v2 — cross-tool benchmark

Project memory: `project_webdbg_v2_2026_06_19.md`. Runs the same operation
(flash read, DPIDR walk, attach, …) against multiple tool backends in
parallel — `probe-rs`, `pyocd`, `uhocd`, `nrfjprog`, and the new ESP32
bridge — and reports timing + bit-identical diffs. The fastest way to
catch a regression in the ESP32 firmware once it's running.

### H.5 The Pico-side firmware itself

`edev_dapv2_zephyr` — at `/Users/yogesh/projects/temp/edevkit/edevkit/firmware/projects/edev_dapv2_zephyr/`.
When debugging an ESP32 ↔ Pico interaction, ALWAYS also have the Pico's
serial console (`/dev/cu.usbmodem*` from its CDC log endpoint) tailing in
parallel — every DAP packet it receives is logged at LOG_LEVEL_INF. The
Pico is the ground truth for "did the command arrive at the SWD layer."

If the Zephyr Pico firmware needs a protocol-level patch (e.g. to add a
missing vendor command), the pattern is:
1. Add the handler in `firmware/projects/edev_dapv2_zephyr/src/dap/`.
2. Document it in Appendix G.3 above.
3. Update both `uhocd` and the ESP32 bridge to use it.

That keeps Pico, Python, and ESP32 in lockstep.

---

## Appendix I — Cross-reference: project-memory notes for any deep-dive

Every "Background:" pointer in §15 and Appendix G is a memory note in
`~/.claude/projects/-Users-yogesh-projects-temp-edevkit/memory/`. The
most-load-bearing ones for ESP32 work:

| Memory note | What's in it |
|---|---|
| `reference_ap_transaction_counter_wall.md` | The 13-op AHB-AP wall + CTRL-AP soft-reset workaround |
| `reference_nrf52_ctrl_ap_eraseall_no_reset_hold.md` | NVMC + RESET interaction; correct ERASEALL sequence |
| `reference_nrf5340_erase_recover_write_canonical_2026_06_26.md` | The seven gotchas for nRF5340 specifically |
| `reference_ring_battery_defeats_power_cycle.md` | Wearable target reset paths |
| `reference_target_debug_power_pin.md` | "First 4 OK then NACK" = firmware sleeping (not wire issue) |
| `reference_adiv5_first_xact_dpidr.md` | ADIv5 line reset semantics |
| `reference_chip_identification_swd.md` | FICR addresses per family |
| `reference_nrf_family_reset_recover.md` | Per-family CTRL-AP offsets |
| `project_uhocd_fork.md` | uhocd structure + custom command list |
| `project_edev_dapv2_zephyr_audit_2026_06_29.md` | Zephyr port findings + SWCLK keep-alive issue |
| `project_uh_dapv2_ack_framing_fix.md` | The `& 0x07` ACK parsing fix |
| `reference_practical_debugging_book.md` | Vidović / SEGGER reference: feature-parity checklist |
| `reference_nrf5340_erased_chip_sleep.md` | Mass-erased Nordic sleep trap — chip needs flash within ~1 s |
| `reference_segger_no_approtect_bypass_2026_06_25.md` | No tool has APPROTECT bypass — only ERASEALL |
| `reference_swd_only_reset_paths.md` | 4 ways to reset a target without nRESET wire |
| `reference_picotool_save_bootsel.md` | Pico recovery via BOOTSEL + 1200-baud trick |
| `reference_cmsis_flash_algorithm.md` | RAM-loaded Thumb-2 loader pattern (only sleep-resistant flash write path) |

When something in this doc is unclear or contradicts what you observe in
practice, the memory notes are the source of truth — they document the
actual debugging sessions that produced each finding.

---

## Appendix J — Final coverage audit (what's in this doc vs what's in memory)

A pass against `~/.claude/projects/-Users-yogesh-projects-temp-edevkit/memory/MEMORY.md`,
which is the authoritative index of ~60 project memory notes. Status of each
critical category:

| Category | Findings count | Coverage in this doc |
|---|---|---|
| CMSIS-DAP protocol quirks | 7 | ✅ Full (G.1–G.3, §15.12, §15.13) |
| SWD wire-level (Pico-internal) fixes | 5 | ✅ Referenced (G.7) — not the ESP32's concern; just don't bypass |
| AHB-AP / DP semantics | 4 | ✅ Full (§15.6, §15.13, G.6) |
| Nordic CTRL-AP / APPROTECT | 6 | ✅ Full (§15.7, §15.8, §15.10, §15.16, §15.17, G.4) |
| Target sleep / debug-domain power | 4 | ✅ Full (§15.1, §15.11, §15.14, §15.16) |
| Multidrop SWD / dual-core nRF53 | 2 | ✅ Full (§15.8) |
| nRF54 RRAM / IronSide | 2 | ✅ Full (§15.9) |
| Recovery sequences | 5 | ✅ Full (G.8, G.10, §15.18 + troubleshooting) |
| Locked-chip identification | 2 | ✅ Full (G.9) |
| Diagnostic patterns (operator-facing) | 3 | ✅ Full (§15.18 + Appendix E) |
| Pico-side bring-up + Zephyr quirks | 4 | ✅ Full (G.1–G.7) |
| Reference Python tooling | 4 | ✅ Full (Appendix H) |
| ESP32-side hardware (USB pins, sdkconfig) | — | ✅ Full (Appendix C) |
| Build / test / smoke-test recipes | — | ✅ Full (§0, §16, Appendices D, E) |
| Edevkit-specific Phase 2-5 commands | 4 new commands | ✅ Full (§12) |
| Buffering strategy (40 KB RAM / ext flash) | — | ✅ Full (§13) |
| Practical Debugging book parity | 1 | ✅ Referenced |
| BOOTSEL / picotool recovery | 1 | ✅ Added (troubleshooting matrix) |
| ESP32-side BLE tuning (MTU/PHY/DLE) | — | ✅ Full (§9) |

**Intentionally NOT in this doc** (out of scope for ESP32 firmware engineer):
- Pico hardware schematic specifics (lives in the edevkit hardware docs)
- KiCad / PCB design (different audience)
- Pico-side Zephyr port internal work (separate Phase 1 effort, see project memory)
- USB Book / training material
- Brand / project layout / filesystem layouts (project-org concerns, not engineering)

**Coverage assessment: complete for the stated scope.** The ESP32 firmware
engineer building this from zero, with this doc + access to the reference
implementation, has everything they need. If a new finding emerges during
implementation, add it both to project memory AND back to this doc (the
two are intended to stay in sync).

---

## Appendix K — nRF52840 cookbook (all 6 ops, concrete recipes)

Self-contained reference for building the six edevkit operations
(probe-info, target-info, flash-read, flash-erase, flash-write,
recover) against an nRF52840 target through the v0.1.1-step5 probe
firmware. Hardware-validated. Where v0.1.1-step5's vendor commands
help, the recipe says so; where the ESP32 has to compose from
standard `DAP_Transfer`, the exact byte sequence is given.

### K.1 nRF52840 address & constant reference

| Region | Address | Notes |
|---|---|---|
| Flash | `0x00000000` – `0x000FFFFF` | 1 MB, 256 pages × 4 KB |
| SRAM | `0x20000000` – `0x2003FFFF` | 256 KB |
| FICR base | `0x10000000` | Read-only chip identification |
| FICR.CODEPAGESIZE | `0x10000010` | 4096 on nRF52840 |
| FICR.CODESIZE | `0x10000014` | Page count (256 on nRF52840) |
| FICR.INFO.PART | `0x10000100` | e.g. `0x00052840` for nRF52840 |
| FICR.INFO.VARIANT | `0x10000104` | ASCII variant, e.g. `0x41414230` ("AAB0") |
| FICR.INFO.PACKAGE | `0x10000108` | `0x00002000` = QIAA, etc. |
| FICR.INFO.RAM | `0x1000010C` | KB of RAM (256) |
| FICR.INFO.FLASH | `0x10000110` | KB of flash (1024) |
| FICR.DEVICEID[0/1] | `0x10000060`/`0x10000064` | 64-bit unique device ID |
| UICR base | `0x10001000` | Read/erase as part of a page |
| UICR.APPROTECT | `0x10001208` | Single word, magic `0x5A` in low byte = HwDisabled |
| UICR.NRFFW[15] | `0x10001014..0x10001050` | Reserved for Nordic FW |
| NVMC base | `0x4001E000` | Non-Volatile Memory Controller |
| NVMC.READY | `0x4001E400` | bit 0 = 1 when idle |
| NVMC.CONFIG | `0x4001E504` | 0=Ren, 1=Wen, 2=Een |
| NVMC.ERASEPAGE | `0x4001E508` | Write page address to erase one page |
| NVMC.ERASEALL | `0x4001E50C` | Write 1 to erase chip + UICR (slower than CTRL-AP) |
| NVMC.ERASEUICR | `0x4001E514` | Write 1 to erase UICR only |
| Cortex-M4 CPUID | `0xE000ED00` | Should read `0x410FC241` on nRF52840 |
| Cortex-M4 DHCSR | `0xE000EDF0` | Debug Halting Control/Status |
| Cortex-M4 DEMCR | `0xE000EDFC` | bit 0 = VC_CORERESET (halt on reset) |
| Cortex-M4 AIRCR | `0xE000ED0C` | bit 2 = SYSRESETREQ |
| ARM ROM table | `0xE00FE000` | Standard ROM table; walk for component IDs |

CTRL-AP (Nordic-specific debug port, AP=1 on nRF52840):

| CTRL-AP reg | Offset | Use |
|---|---|---|
| RESET | `0x00` | Write 1 to assert; 0 to release |
| ERASEALL | `0x04` | Write 1 to start mass erase |
| ERASEALLSTATUS | `0x08` | Read 0 when idle, 1 when busy |
| APPROTECTSTATUS | `0x0C` | Read APPROTECT state (bit 0 = locked) |
| IDR | `0xFC` | Identifies CTRL-AP (`0x02880000` or `0x12880000`) |

CSW value for nRF52840 App AHB-AP: `0x23000002` (HPROT=secure/priv,
32-bit, no auto-increment).

### K.2 Op 1 — Read edev_dap info (probe identity)

Standard `DAP_Info` (cmd 0x00). The ESP32 bridge reads multiple IDs to
build its probe-info display.

```
Send:    [0x00, 0xF0]                       # CAPABILITIES
Receive: [0x00, len, caps...]
Send:    [0x00, 0xFF]                       # PACKET_SIZE (u16 LE)
Receive: [0x00, 0x02, lo, hi]
Send:    [0x00, 0x04]                       # FW_VERSION (string)
Receive: [0x00, len, "2.1.0" ...]
Send:    [0x00, 0x01]                       # VENDOR (string)
Send:    [0x00, 0x02]                       # PRODUCT
Send:    [0x00, 0x03]                       # SERIAL
```

Expected from v0.1.1-step5: caps=`0x11`, packet_size=512, fw_version
`"2.1.0"`, vendor `"Edevkit"`, product `"edev_dapv2 CMSIS-DAP"`, serial
= RP2350 chip unique 64-bit ID as hex.

### K.3 Op 2 — Read target MCU info (full ARM identification)

DPIDR + AP_IDR + CPUID + FICR — full chip identification recipe:

```
1. dap_connect_and_reset(probe)           # DAP_Connect + SWJ_Sequence + idle
2. read DPIDR (DP reg 0x00)               # MUST be first xact after line reset
3. write CTRL/STAT (DP reg 0x04) = 0x50000000   # power up debug + system
4. read CTRL/STAT, verify (val & 0xA0000000) == 0xA0000000
5. read AHB-AP[0].IDR (AP=0, bank=0xF, reg 0xFC)
6. read mem 0xE000ED00 (CPUID) via AHB-AP[0]   # ARM core identification
7. read mem 0x10000100 (FICR.INFO.PART)        # 0x00052840 = nRF52840
8. read mem 0x10000104 (FICR.INFO.VARIANT)     # silicon revision
9. read mem 0x10000060 (FICR.DEVICEID[0])      # unique ID
10. read mem 0x10000064 (FICR.DEVICEID[1])
```

Expected on nRF52840:
- DPIDR = `0x2ba01477`  (ARM=0x23B, partno=0xba, version=1=DPv1)
- AHB-AP[0].IDR = `0x24770011`  (ARM AHB-AP rev 1)
- CPUID = `0x410FC241`  (Cortex-M4, r0p1, ARM)
- FICR.INFO.PART = `0x00052840`
- FICR.DEVICEID = unique 64-bit per chip

Bridge response can pack this into the existing `RPC_TARGET_INFO`
TLV format (§11), or use a richer per-vendor format. For the
**all-ARM-supported-details** ask: CPUID gives implementer + arch +
part + revision; ROM table walk at `0xE00FE000` enumerates components
(ITM, DWT, FPB, etc.) if needed.

### K.4 Op 3 — Partial + full flash read

Standard AHB-AP memory read. Use auto-increment for bulk reads (set
CSW bit 4); set TAR once per 1 KB boundary (Nordic's TAR wraps at 1
KB with auto-inc):

```
Per-1-KB-block:
  write AP.CSW = 0x23000012      # HPROT=secure/priv, 32-bit, +AddrInc=on
  write AP.TAR = block_base
  for offset in 0..1023, step 4:
    read AP.DRW   (posted)        # data appears on the NEXT AP read or DP.RDBUFF
    accumulate result
  read DP.RDBUFF                  # flush final word
```

Or per-word (safer, ~5% slower):
```
For each word:
  write AP.CSW = 0x23000002      # no auto-inc
  write AP.TAR = addr
  read AP.DRW                    # posted
  read DP.RDBUFF                 # actual data
```

**Address range for nRF52840 flash**: `0x00000000` to `0x000FFFFF`
(1 MB). Reading **flash only** = read `0x00000000` to `0x000FFFFF`.
Reading UICR additionally = read `0x10001000` to `0x10001FFF`. FICR
is read-only and accessible at any time (no halt required).

**Watch the AHB-AP-13-op wall on running firmware** (§15.6). For
nRF52840 with sleepy firmware (any battery-saving Nordic app), batch
≤ 12 AP transactions then issue a CTRL-AP soft-reset:

```
Between batches of ≤ 12 AHB-AP reads:
  write CTRL-AP[1].RESET = 1
  sleep ≥ 2 ms
  write CTRL-AP[1].RESET = 0
  # then re-arm DP: write DP.ABORT = 0x1E (sticky clear)
  #                write DP.CTRL/STAT = 0x50000000 (re-power)
```

### K.5 Op 4 — Partial + full flash erase

**Full chip erase** — use the vendor command:

```
Send:    [0x85]                          # NRF53_ERASE
Receive: [0x85, 0x00, 0x01]              # status=OK, ap_count=1
```

Erases all flash + UICR via CTRL-AP ERASEALL. ~1 s on nRF52840. After
this the chip is unlocked (APPROTECT = 0xFFFFFFFF) and ready for flash
write.

**Partial erase** (per-page, 4 KB granularity) — host-driven via NVMC:

```
1. halt the core (write DHCSR @ 0xE000EDF0 = 0xA05F0003)
2. wait NVMC.READY (read 0x4001E400 bit 0 == 1)
3. write NVMC.CONFIG (0x4001E504) = 2     # Een = erase enabled
4. wait NVMC.READY
5. write NVMC.ERASEPAGE (0x4001E508) = page_addr (must be 4-KB aligned)
6. wait NVMC.READY   (page erase takes ~85 ms on nRF52840)
7. write NVMC.CONFIG = 0                  # Ren = back to read-only
```

Boundary validation: `page_addr & 0xFFF == 0` AND `page_addr < 0x100000`.

### K.6 Op 5 — Partial + full flash write

**Two approaches, pick one:**

**A. Direct NVMC writes** (slow but simple) — write each word through
the NVMC. Each write takes ~40 µs on nRF52 + the SWD round-trip
(~200 µs at 1 MHz). About **40 KB/s typical** for full-flash write.

```
1. halt the core
2. erase the target page(s) via §K.5 partial erase first
3. wait NVMC.READY, write NVMC.CONFIG = 1 (Wen)
4. for each word:
     wait NVMC.READY
     write the word directly to flash address  (NVMC handles the program op)
5. wait NVMC.READY, write NVMC.CONFIG = 0 (Ren)
6. verify by reading back
```

Validation per write: `addr & 3 == 0`, `addr < 0x100000`, the page
containing `addr` must already be erased (NVMC will fault on writes
to non-erased flash).

**B. CMSIS Flash Algorithm RAM loader** (fast, recommended) — load a
small Thumb-2 program into RAM, set its register args, run it on the
target's own core. The target CPU does the NVMC dance at native speed:
**~150 KB/s typical** on nRF52.

```
1. halt core (DHCSR write)
2. write the loader binary to RAM (e.g. 0x20000000), ≤ 256 B for a
   minimal erase/program/verify routine
3. write source data to RAM (e.g. 0x20000400), one page at a time
4. set CPU registers via DAP_Transfer to DCRSR/DCRDR:
     R0 = function selector (1=ERASE, 2=PROGRAM)
     R1 = arg1 (page addr / source RAM addr)
     R2 = arg2 (length / page addr)
     PC = loader entry
     LR = BKPT trap address
     SP = top of staging RAM
5. write DHCSR = 0xA05F0001  (release halt, run)
6. poll DHCSR bit 17 (S_HALT) until target hits BKPT
7. read R0 via DCRSR/DCRDR → return code
8. repeat per page
```

A reference Thumb-2 loader for nRF52 is at
`firmware/projects/edev_dapv2/loader/nrf_flash_loader.S` in the
pico-sdk reference branch (algorithm-only; port if needed). pyocd
and probe-rs ship the same pattern as a `.FLM` Cortex Flash Magic
file — either is a valid starting point.

**Hex / bin file upload to the bridge:** stage in ESP32 external
flash (4 MB FAT partition per §13.2 of this doc), then drive the
flash write op page-by-page from the staged file.

### K.7 Op 6 — nRF52840 recovery (chip unlock)

**Use vendor cmd 0x85.** On nRF52, "recovery" = ERASEALL = unlock.
There's no separate UICR-unlock step like nRF5340 needs. One call:

```
Send:    [0x85]                          # NRF53_ERASE
Receive: [0x85, 0x00, 0x01]              # status=OK, ap_count=1
```

Post-recovery state of the chip:
- All flash = `0xFFFFFFFF`
- All UICR = `0xFFFFFFFF` (including UICR.APPROTECT — the chip is now unlocked)
- CTRL-AP.APPROTECTSTATUS = 1 (locked from chip's POV until next reset)
- Need to pulse a reset (or just attach again) for the unlocked state to take effect

After recovery, the bridge typically flashes new firmware (§K.6) which
includes either UICR.APPROTECT=0x5A (HwDisabled) at offset 0x208 in the
UICR page, or just leaves UICR alone (chip stays unlocked because
nothing wrote 0x00 to APPROTECT).

**Do NOT call `0x84 NRF53_RECOVER` on nRF52** — that command composes
nRF5340-specific UICR programming and will fail mid-flow with the chip
in a partial state.

### K.8 Halt / reset / resume primitives

Standard Cortex-M debug, no vendor commands needed:

| Op | Sequence |
|---|---|
| Halt core | write `DHCSR` (`0xE000EDF0`) = `0xA05F0003` (DBGKEY \| C_DEBUGEN \| C_HALT) |
| Resume | write `DHCSR` = `0xA05F0001` (DBGKEY \| C_DEBUGEN, clear C_HALT) |
| Single step | write `DHCSR` = `0xA05F000D` (DBGKEY \| C_DEBUGEN \| C_HALT \| C_STEP) |
| System reset | write `AIRCR` (`0xE000ED0C`) = `0x05FA0004` (VECTKEY \| SYSRESETREQ) |
| Reset + halt | DEMCR.VC_CORERESET=1 → SYSRESETREQ → core halts at reset vector |
| Read register | write `DCRSR` (`0xE000EDF4`) = `reg_id`, then read `DCRDR` (`0xE000EDF8`) |
| Write register | write `DCRDR` = value, then write `DCRSR` = `reg_id \| 0x10000` |

Register IDs for DCRSR: R0..R12 = 0..12, SP = 13, LR = 14, PC = 15,
xPSR = 16, MSP = 17, PSP = 18, CONTROL+FAULTMASK+BASEPRI+PRIMASK = 20.

### K.9 What a complete ESP32 BLE session looks like on nRF52840

End-to-end flow for the phone-app `RPC_FLASH_WRITE_FULL` operation
(per §12), mapped onto nRF52840:

```
Phone → ESP32   (BLE TLV)               ESP32 → Pico   (USB CMSIS-DAP)
─────────────                            ──────────────────────────────
RPC_USB_STATUS                           [0x00, 0xF0] DAP_Info(caps)
                                         [0x02, 0x01] DAP_Connect(SWD)
                                         [0x12, 72,  …]  SWJ_Sequence (line reset)
                                         [0x12, 64,  …]  SWJ_Sequence (line reset + idle)
                                         [0x05, …] DAP_Transfer (DPIDR read)

RPC_TARGET_INFO                          DAP_Transfer chain reading CPUID + FICR.INFO.PART
                                         → identify nRF52840, flash_size=1MB, page=4KB

RPC_FLASH_WRITE_FULL  (start, total)     stage to ESP32 ext flash, ack

RPC_FLASH_WRITE_FULL_CHUNK × N           each chunk → ext flash, ack

RPC_FLASH_WRITE_FULL_COMMIT              [0x85] NRF53_ERASE  (full wipe + unlock)
                                         DAP_Transfer halt core
                                         DAP_Transfer write loader to 0x20000000
                                         DAP_Transfer set R0..PC/LR, resume, poll halt
                                          (loop per 4 KB page)
                                         DAP_Transfer read back to verify
                                         DAP_Transfer reset target

RPC_FLASH_WRITE_FULL_PROGRESS (every page)
…final response with verify md5
```

Every USB packet here is either a standard CMSIS-DAP command (DAP_*)
or the single vendor command `0x85 NRF53_ERASE`. No other vendor
command is needed for nRF52840 against v0.1.1-step5.

### K.bonus.0 — `0x89 NRF53_TARGET_INFO` (v0.1.7)

Single-call ARM target identification. Replaces three separate
`DAP_Transfer` / `READ_MEM` round-trips on initial connect.

```
Request:  [0x89]                       (1 byte, no payload)
Response: [0x89, status,
           u32_le dpidr,                — DP.DPIDR
           u32_le ap0_idr,              — AHB-AP[0].IDR
           u32_le cpuid]                — Cortex-M CPUID at 0xE000ED00
          = 14 bytes
```

**Family inference from response:**
- `(DPIDR >> 12) & 0xF` → `1 = nRF52 family (DPv1)`, `2 = nRF5340 (DPv2)`
- `(CPUID >> 4) & 0xFFF` → `0xC24 = Cortex-M4`, `0xD21 = Cortex-M33`
- `(DPIDR >> 1) & 0x7FF` → `0x23B = ARM designer`, `0x144 = Nordic designer`

**FICR fields are intentionally NOT in this response** — see the FICR
caveat above. Bridge should compose chip-specific FICR reads via
`0x88 NRF53_READ_MEM` after inferring family from this call.

**When to call:**
- Right after connecting: one packet establishes the target's identity
  and reachability.
- Before any vendor cmd that depends on chip family (e.g., picking the
  right `nvmc_base` for `0x87 FLASH_WRITE_APP`).

### K.bonus.1 — `0x86 NRF53_FLASH_WRITE_NET` (v0.1.5) + `0x87 NRF53_FLASH_WRITE_APP` (v0.1.6)

Probe-side **batched flash programming** — wraps the Nordic NVMC
sequence (Wen → per-word write+READY → Ren) in a single packet so the
bridge avoids per-word USB round-trips.

**`0x86 NRF53_FLASH_WRITE_NET`** — Net flash write (nRF5340 only):
```
[0x86, u32_le addr, u16_le word_count, data[word_count*4]]
   addr        — Net flash destination (4-byte aligned, typ 0x01000000..0x0103FFFF)
   word_count  — ≤ 14 (7-byte hdr + 14*4 = 63 ≤ 64-B USB-FS packet)
→ [0x86, status, u16_le words_written]
```
ap=1, csw=0x03800042 (Net AHB-AP), NVMC at 0x41080000 are all
hardcoded inside the probe.

**`0x87 NRF53_FLASH_WRITE_APP`** — App flash write (nRF52 + nRF5340 App):
```
[0x87, u32_le nvmc_base, u32_le addr, u16_le word_count, data[word_count*4]]
   nvmc_base   — 0x4001E000 nRF52 family; 0x50039000 nRF5340 App
   addr        — App flash destination (4-byte aligned, typ 0x00000000..0x000FFFFF)
   word_count  — ≤ 13 (11-byte hdr + 13*4 = 63)
→ [0x87, status, u16_le words_written]
```
ap=0, csw=0x23000002 (App AHB-AP) hardcoded; NVMC base is the only
family-specific knob.

**Behaviour for both:**
- Caller pre-erases via `0x85 NRF53_ERASE` (NVMC can't reprogram
  non-erased flash; would WAIT-loop forever otherwise).
- Words equal to `0xFFFFFFFF` are **skipped** — NVMC can't reprogram
  erased flash anyway, and skipping saves USB time on sparse images
  (e.g., a hex image where unused pages stay 0xFF).
- `words_written` reports the count programmed before any mid-batch
  failure — the bridge can resume from `addr + words_written*4` on
  retry.
- `NVMC.CONFIG = Ren` is restored even on mid-loop failure; flash is
  never left write-enabled.

> **⚠ Net flash boot region (0x01000000..~0x01004110) — multi-batch
> writes corrupt; bench-verified 2026-06-30 on nRF5340 DK with two
> Ring Pro 351 production hex images:**
>
> A single 14-word `0x86 FLASH_WRITE_NET` call to `0x01000000`
> writes + reads back bit-perfect. **But sustained multi-batch
> writes covering the Net core's boot/vector region (the first
> ~16 KB, addresses up to roughly `0x01004110`) result in readback
> that doesn't match what was written.** All individual batches
> return `status=OK, words_written=14/14` — the corruption only
> shows up on readback verification.
>
> **Likely root cause:** the Net core is alive while we're writing
> its boot region. Each completed batch publishes new MSP / reset
> vector / handler values. The Net core fault-loops on partial /
> invalid early data, generating AHB-AP bus contention that
> interferes with subsequent flash writes. The corruption is
> deterministic — programming the same image twice yields the
> same wrong content at the same addresses.
>
> **Empirical evidence (bench DK, 2026-06-30):** flashing both
> `uh_ringpro351_v5241951_merged.hex` and `uh_ringpro351_v5242051_merged.hex`,
> 13 of 14 segments verified bit-perfect (~700 KB of 718 KB) but
> the first Net segment (`0x01000000..0x0100410c`, 16,652 bytes)
> mismatched on both files with identical expected/got hashes.
>
> **Workarounds (pick one):**
> 1. **Bridge-side:** write the Net boot region (≤16 KB starting at
>    `0x01000000`) LAST, after all other Net flash is written. The
>    Net core can't fault-loop on data it hasn't seen yet.
> 2. **Bridge-side:** write the Net boot region in small chunks
>    (1–2 batches per call) with brief pauses — gives the Net core
>    less time to react between writes.
> 3. **Probe firmware:** future `0x86 FLASH_WRITE_NET` could halt
>    Net CPU before writes touching `0x01000000..0x01004110`. This
>    requires Net core DHCSR access (which works on nRF5340) — not
>    shipped in v0.1.8, would be a v0.1.9 feature.
>
> Until then, **bridge verification step must check the Net boot
> region last and have a retry path** that re-writes the boot
> region as the final step of a full-image flash. The rest of Net
> flash (`0x01004110` and above) writes reliably with single-pass
> programming.

**Bridge workflow for a full image (1 MB App, 256 KB Net):**

```python
# 1. Erase (one call wipes everything)
TX([0x85])

# 2. Stream pages from BLE staging to flash, 13 (App) or 14 (Net) words per call
for batch_start in range(0, image_len, batch_words * 4):
    batch = image[batch_start : batch_start + batch_words * 4]
    if family == "nrf5340" and target == "net":
        cmd = bytes([0x86]) + u32(0x01000000 + batch_start) + u16(batch_words) + batch
    else:  # App
        cmd = bytes([0x87]) + u32(NVMC_BASE) + u32(batch_start) + u16(batch_words) + batch
    resp = TX(cmd)
    # response: [echo, status, u16 words_written]
    assert resp[1] == 0
    assert struct.unpack('<H', resp[2:4])[0] == batch_words

# 3. (nRF5340 only) program UICR.APPROTECT permanently if desired — done by 0x84 RECOVER
# 4. (Both) verify by reading back via 0x88 READ_MEM in 15-word chunks
```

**Speedup math** (USB-FS, ~1 ms RTT):

| Image | Path | USB calls | Wall clock |
|---|---|---|---|
| 1 MB App, compose via 0x8C+0x88 poll | 3 per word | ~750,000 | ~12 minutes |
| 1 MB App, vendor `0x87` (13 words/call) | 1 per 13 words | ~20,000 | ~25 seconds |
| 256 KB Net, compose via 0x8C+0x88 poll | 3 per word | ~200,000 | ~3 minutes |
| 256 KB Net, vendor `0x86` (14 words/call) | 1 per 14 words | ~4,700 | ~6 seconds |

~35× fewer USB calls; ~30× wall-clock speedup on USB-FS.

---

### K.bonus.2 — `0x88 NRF53_READ_MEM` + `0x8C NRF53_WRITE_MEM` (v0.1.2 + v0.1.3)

Available since v0.1.2-step8 (READ) and v0.1.3-step8 (WRITE). Chip-
agnostic — work against any AHB-AP. Replace multi-`DAP_Transfer`
sequences with a single packet.

**`0x88 READ_MEM`** — 13-byte request, up to ~60 B response (USB-FS
packet size limit; bridge loops for bulk):
```
[0x88, flags, ap_index, u32_le csw, u32_le addr, u16_le word_count]
   flags bit 0 = clear DEMCR.VC_CORERESET before read (nRF5340 fix 6a)
→ [0x88, status, data[word_count*4]]   (LE words)
```

**`0x8C WRITE_MEM`** — ≥13-byte request, 2-byte response:
```
[0x8C, flags, ap_index, u32_le csw, u32_le addr, u16_le word_count,
 data[word_count*4]]
   flags = 0 (reserved)
   word_count ≤ 12  (13 + 12*4 = 61 ≤ 64-byte USB-FS packet)
→ [0x8C, status]
```

**Example: write nRF52 UICR.APPROTECT = 0x5A in one call:**
```python
# Step 1. NVMC.CONFIG = Wen
WRITE_MEM(ap=0, csw=0x23000002, addr=0x4001E504, n=1, data=<u32 1>)
# Step 2. poll NVMC.READY via READ_MEM
while READ_MEM(0x4001E400, n=1)[0] & 1 != 1: pass
# Step 3. UICR.APPROTECT ← 0x5A
WRITE_MEM(ap=0, csw=0x23000002, addr=0x10001208, n=1, data=<u32 0x5A>)
# Step 4. poll READY
while READ_MEM(0x4001E400, n=1)[0] & 1 != 1: pass
# Step 5. NVMC.CONFIG = Ren
WRITE_MEM(ap=0, csw=0x23000002, addr=0x4001E504, n=1, data=<u32 0>)
```

5 vendor-cmd round-trips vs ~20+ `DAP_Transfer` round-trips composing
the same sequence by hand. The wire-time saving on USB-FS is ~3×.

**Example: halt a target (Cortex-M DHCSR write):**
```python
# DHCSR @ 0xE000EDF0 ← 0xA05F0003 (DBGKEY | C_DEBUGEN | C_HALT)
WRITE_MEM(ap=0, csw=0x23000002, addr=0xE000EDF0, n=1, data=<u32 0xA05F0003>)
```

**Example: set CPU register via DCRSR/DCRDR before run:**
```python
# Write R0 = arg_value
WRITE_MEM(ap=0, csw=0x23000002, addr=0xE000EDF8, n=1, data=<u32 arg_value>)  # DCRDR
WRITE_MEM(ap=0, csw=0x23000002, addr=0xE000EDF4, n=1, data=<u32 0x10000|0>)   # DCRSR write R0
```

These primitives are sufficient to compose:
- **App UICR programming** on nRF52840 (5 calls instead of 20+)
- **RAM loader staging** for the CMSIS Flash Algorithm flash-write
  pattern (loader bytes via WRITE_MEM, regs via DCRSR/DCRDR, resume
  via DHCSR)
- **Target peripheral configuration** (any AHB-AP-reachable register)

### K.10 What pieces of nRF52840 support are still bridge-composed (v0.1.8)

Updated 2026-06-30. Most of K.10's original items have been
subsumed by vendor cmds shipped in v0.1.2 through v0.1.7. What
remains:

- **nRF52840 UICR write** — still composed bridge-side from
  `0x8C WRITE_MEM` + `0x88 READ_MEM` polling the NVMC.READY
  register (~5 calls). A future `0x8D NRF52_UICR_PROGRAM` would
  trim the RTT count but is not blocking; bridge does it today.
- **Multi-batch flash reads >15 words** — `0x88 NRF53_READ_MEM`
  reads up to 15 words/call (USB-FS 64 B packet limit). For
  bulk flash dump the bridge loops over batches itself. The
  AHB-AP-13-op transaction-counter wall ([[reference_ap_transaction_counter_wall]])
  is handled inside `0x88` — the bridge does NOT need to insert
  CTRL-AP soft-resets between batches.
- **Multi-batch flash writes >13 words** — same loop pattern for
  `0x87 NRF53_FLASH_WRITE_APP`. The probe re-initializes NVMC
  state on each call; no cross-call invariants to maintain.

None of these block ESP32 development; they're efficiency wins for
future releases.

---

## Appendix L — Per-cmd timing budgets + bridge progress strategy (v0.1.8)

This appendix closes out **Step 9 of the original 10-step plan** —
the "progress packets / WAIT-ACK / USB timeout" item. After running
v0.1.7 against bench nRF5340 DK and nRF52840 hardware, the
conclusion is:

> **The probe does not need to emit intermediate progress packets.**
> All long-running ops complete inside the host's USB bulk transfer
> deadline at the values below. The BLE-side progress feedback is
> purely a bridge concern (own timer + state machine).

This appendix gives the bridge author the timing data needed to
(a) set sensible USB bulk transfer timeouts on the ESP32 side, and
(b) drive a "still working… T+Ns" BLE notification stream from the
bridge's own clock.

### L.1 Probe-side timing characteristics (firmware-internal, v0.1.7)

What the probe firmware already does to keep itself sane during
long ops — none of this requires bridge attention, but knowing it
helps when reading log output:

| Mechanism | Constant | Where | Purpose |
|---|---|---|---|
| WAIT-ACK retry budget | `DP_WAIT_RETRIES = 8` | `nrf53_dp.c` | In-protocol retry on AHB-AP WAIT; ~1 µs per retry at 25 MHz SWD → ~8 µs total overhead. Effectively zero against USB deadline. |
| CTRL-AP ERASEALL deadline | `10000 ms / AP` | `nrf53_erase_all` | Two APs (App + Net) → worst case 20 s total. Actual on DK silicon: ~3 – 6 s. |
| NVMC READY poll interval | `500 µs` | `nrf53_nvmc.c` | Word-level NVMC.READY poll; per-word op completes in microseconds. |
| NVMC per-word timeout | `100 ms` | `nrf53_flash_write.c` | Defense-in-depth; never exceeded in practice. |
| Net stub per-stage timeout | `100 ms` | `nrf53_stubs.c` | Each marker transition during Net UICR programming. |
| DP power-up poll | `100 ms` | `nrf53_dp_power_up` | CSYSPWRUPACK + CDBGPWRUPACK wait. |
| `k_msleep` between polls | Various | All long-poll loops | Yields to other Zephyr threads → USB stack stays responsive, no watchdog starvation, no priority inversion against `cmsis_dap_thread`. |

The key invariant: **every long-poll loop in the nrf53 ops code
yields to the Zephyr scheduler via `k_msleep` or `k_usleep`**, so
USB SOFs / NAKs continue to be processed during the wait. The host
sees a normal USB session that just happens to have a slow IN
response — well within libusb's default 5-second bulk timeout
when the bridge bumps it as recommended below.

### L.2 Observed wall-clock timing — bench measurements

Bench measurements taken 2026-06-30 against nRF5340 DK (chip ID
`FA6418C89BAB3D36`) running the bench test harness. nRF52840
column is the matching v0.1.1-step5 run. All times are wall-clock
from cmd-OUT to response-IN over USB-FS.

| Vendor cmd | Typical | Worst observed | Recommended bridge USB timeout |
|---|---:|---:|---:|
| `0x84 RECOVER` (nRF5340 only) | ~7 s | ~10 s | **30 s** |
| `0x85 ERASE` nRF5340 (2 APs) | ~5 s | ~8 s | **15 s** |
| `0x85 ERASE` nRF52840 (1 AP) | ~3 s | ~5 s | **10 s** |
| `0x86 FLASH_WRITE_NET` (14 W) | ~15 ms | ~50 ms | **2 s** |
| `0x87 FLASH_WRITE_APP` (13 W) | ~15 ms | ~50 ms | **2 s** |
| `0x88 READ_MEM` (15 W) | ~10 ms | ~50 ms | **2 s** |
| `0x89 TARGET_INFO` | ~5 ms | ~20 ms | **1 s** |
| `0x8A UICR_PROGRAM_APP` | ~50 ms | ~200 ms | **5 s** |
| `0x8B UICR_PROGRAM_NET` (stub) | ~300 ms | ~600 ms | **5 s** |
| `0x8C WRITE_MEM` (12 W) | ~10 ms | ~50 ms | **2 s** |
| Standard `DAP_Transfer` (pkts) | < 1 ms | ~5 ms | **1 s** |

Bench timings are with the probe and target on the same host;
USB-FS round-trip is dominated by chip-side work, not USB
transport. A 30 s budget for `RECOVER` is **4×** the worst
observed value — leaves headroom for slow silicon or production
units with longer ERASEALL cycles (Ring Pro 351 hasn't been bench
tested yet; see Appendix M for the acceptance procedure).

### L.3 Bridge-side timeout configuration

Use distinct timeouts per cmd class on the USB host side. The
ESP32 USB host stack has its own per-transfer timeout that lives
above libusb's default — set it to the values in column 4 above.

```c
// in bridge_rpc.c
static const struct {
    uint8_t vendor_cmd;
    uint32_t usb_timeout_ms;
} g_cmd_timeouts[] = {
    {0x84, 30000},  // NRF53_RECOVER
    {0x85, 15000},  // NRF53_ERASE
    {0x86, 2000},   // NRF53_FLASH_WRITE_NET
    {0x87, 2000},   // NRF53_FLASH_WRITE_APP
    {0x88, 2000},   // NRF53_READ_MEM
    {0x89, 1000},   // NRF53_TARGET_INFO
    {0x8A, 5000},   // NRF53_UICR_PROGRAM_APP
    {0x8B, 5000},   // NRF53_UICR_PROGRAM_NET
    {0x8C, 2000},   // NRF53_WRITE_MEM
    // Standard CMSIS-DAP cmds (0x00..0x7F): 1000 ms
};
```

### L.4 Bridge-side progress notifications over BLE

Two patterns to choose from. **Pattern A is recommended** —
simpler, sufficient for the v0.1.x release. Pattern B is an
optional v0.2+ extension if the BLE client wants finer feedback.

#### Pattern A — Timer-driven BLE notifications (recommended)

The bridge runs a `xTimerCreate` periodic timer (250 ms is fine)
that fires while a long-running RPC is in flight. The timer
handler emits a `TLV_PROGRESS` notification on the BLE TX
characteristic:

```c
// In the bridge's RPC dispatcher, when starting a long op
void rpc_start_long_op(uint16_t rpc_id, uint16_t seq) {
    g_long_op.start_tick = xTaskGetTickCount();
    g_long_op.rpc_id    = rpc_id;
    g_long_op.seq       = seq;
    g_long_op.budget_ms = lookup_budget_ms(rpc_id);
    xTimerStart(g_long_op.progress_timer, 0);
    // dispatch the USB transfer (blocking, in its own task)
}

// Timer fires every 250 ms
static void progress_timer_cb(TimerHandle_t t) {
    uint32_t elapsed_ms = pdTICKS_TO_MS(
        xTaskGetTickCount() - g_long_op.start_tick);
    uint8_t percent = (elapsed_ms * 100U) / g_long_op.budget_ms;
    if (percent > 99) percent = 99;
    ble_tlv_send_progress(g_long_op.seq, percent, elapsed_ms);
}

// Final completion path stops the timer + sends real response
void rpc_complete_long_op(uint8_t status, ...) {
    xTimerStop(g_long_op.progress_timer, 0);
    ble_tlv_send_response(g_long_op.seq, status, ...);
}
```

The 99 % cap prevents the bar from sitting at 100 % while the
real response races to arrive.

Wire format of the progress notification (proposed addition to
the TLV protocol):

```
type   = 0x0A  TLV_PROGRESS
length = 6
data   = [seq u16_le, percent u8, elapsed_ms u24_le]
status = (none — informational only)
```

The BLE client treats this as a hint, not a state transition.
The authoritative "operation complete" signal remains the
TLV response packet matched by `seq`.

#### Pattern B — Probe-side intermediate packets (NOT RECOMMENDED)

In principle the probe could chunk a long op into N sub-ops and
emit one response per chunk. This would require:

1. A new "command in progress" packet type on the CMSIS-DAP bulk
   IN endpoint (which would break compatibility with stock
   CMSIS-DAP host tools — they'd see unexpected packets).
2. Splitting the firmware's ops to be re-entrant / resumable
   (today they're blocking single calls).
3. A new bridge-side state machine to receive partial responses.

The win would be real-time chip-side progress (e.g., "1.2 MB of
1.5 MB written"). The cost is invasive firmware changes and
incompatibility with `probe-rs`, `pyocd`, and other standard
CMSIS-DAP clients. **Not worth it** for the v0.1.x release;
Pattern A delivers good-enough UX without breaking anything.

### L.5 What the bridge should NOT do

| Anti-pattern | Why it's wrong |
|---|---|
| Set a single 30 s timeout for all USB transfers | One in-flight `0x85 ERASE` blocks the bridge from detecting an actually-wedged probe for 30 s on every cmd. |
| Cancel an in-flight USB transfer to "speed up" | The probe has no way to abort; you'll lose response framing and need to re-enumerate. |
| Send a second cmd before the first response | CMSIS-DAP is strict request/response; the probe doesn't queue, the second cmd will be dropped or merged with the response. Use the `seq` field's single-flight discipline (§10). |
| Show a determinate progress bar (0–100 %) tied to real chip-side work | The probe doesn't report partial progress. Use elapsed-time-vs-budget as a proxy (Pattern A). |

### L.6 Summary — what Step 9 ends up being

The original Step 9 in the implementation plan was loosely
"progress packets / WAIT-ACK / USB timeout."

What actually ended up being needed:
1. **WAIT-ACK** — handled probe-side; in-protocol DP retry budget
   of 8 covers all observed cases. No bridge work needed.
2. **USB timeout** — handled bridge-side; the per-cmd timeout
   table in L.3 above. Doc-only deliverable.
3. **Progress packets** — Pattern A (bridge-side timer-driven
   notifications) is the recommended approach. No firmware
   changes required; the bridge can implement at its own pace.

**No probe firmware changes are required for Step 9.** The v0.1.8
release ships the same firmware binary as v0.1.7 — both are the
final probe firmware for this phase of the plan.


---

## Appendix M — Ring Pro 351 acceptance procedure (Step 10)

This appendix closes out the last item in the original 10-step
plan: end-to-end acceptance against a Ultrahuman Ring Pro 351
(nRF5340) production target. **Pending bench execution; document
shipped so the procedure runs in one command when a Ring is on
bench.**

### M.1 What this proves

When this acceptance passes against a production Ring, the
v0.1.8 probe firmware (== v0.1.7 binary) is certified for
production-Nordic-target debug + recover workflows. Everything tested on the bench
nRF5340 DK (DK silicon, 11/11 PASS) is reproduced against a real
sealed wearable. This is the bar for "release the probe firmware
to production use."

### M.2 Hardware required

- **Pico running edev_dapv2_zephyr v0.1.8** (commit `c26fbfa` or later;
  v0.1.7 firmware binary works identically)
- **Ring Pro 351** with:
  - Charging puck connected (Ring's internal Li-ion alone is not
    enough — ERASEALL pulls more current than the cell delivers
    under SWD load; see
    [[reference_ring_battery_defeats_power_cycle]])
  - SWD pogo pins making solid contact (Ring's debug pads are
    tiny; a loose contact mid-ERASEALL leaves the chip in a
    half-erased state that takes a second RECOVER to clean up)
- **Re-flashable firmware image for the Ring** — the procedure
  fully wipes both cores; you'll need to re-flash production
  firmware afterwards via the normal release tooling

### M.3 Running the procedure

```bash
cd firmware/projects/edev_dapv2_zephyr/tools
python3 ring_pro_351_acceptance.py
```

The script will:
1. Prompt with `WIPE THE RING` confirmation (explicit, can't
   accidentally type past it)
2. Run 9 phases in order, each with a PASS / FAIL line
3. Print a green banner on full pass, or red banner with the
   failing phase on any failure

Non-destructive identification-only mode (safe to run before
committing to a wipe):

```bash
python3 ring_pro_351_acceptance.py --identify
```

This runs only the ping + TARGET_INFO phases — verifies SWD
contact and reads back DPIDR / AP_IDR / CPUID.

### M.4 Phase-by-phase expected output

| # | Phase | What it tests | Budget |
|---|---|---|---|
| 1 | `Identify target` | TARGET_INFO returns nRF5340 DPIDR + M33 CPUID | 3 s |
| 2 | `Mem roundtrip` | WRITE_MEM + READ_MEM bit-perfect against App SRAM | 6 s |
| 3 | `ERASEALL both cores` | CTRL-AP ERASEALL on App + Net | 30 s |
| 4 | `Verify erase` | flash[0] = 0xFFFFFFFF post-erase | 3 s |
| 5 | `FLASH_WRITE_APP` | 13-word pattern → bit-perfect readback | 6 s |
| 6 | `FLASH_WRITE_NET` | 14-word pattern → bit-perfect readback (Net flash) | 6 s |
| 7a | `UICR_PROGRAM_APP` | App APPROTECT/SECUREAPPROTECT = 0x50FA50FA | 5 s |
| 7b | `UICR_PROGRAM_NET` | Net UICR.APPROTECT = 0x50FA50FA via stub | 10 s |
| 8 | `RECOVER` | Idempotent full-chain RECOVER (does ERASE + both UICRs) | 60 s |

Total budget: ~130 s. Bench DK timing was ~30 s; production
silicon may be slower so the budgets are 3× the bench worst-case.

### M.5 Notes on the Net SRAM marker check (phase 7b)

The Net stub writes `0xDEADC0DE` to SRAM `0x21000000` as a
"stub completed" marker. On the bench nRF5340 DK silicon this
write was NOT visible via Net AHB-AP (CPU bus vs debug bus
asymmetry — see
[[project_edev_dapv2_zephyr_v017_feature_complete_2026_06_30]] §2).

**The Ring Pro 351 is expected to be production silicon where
both buses are consistent.** If the marker reads `0xDEADC0DE`,
that's the production-silicon behavior and the script logs a
green NOTE confirming this. If the marker reads `0x00000000`
(matching DK silicon), the script logs a yellow NOTE — this is
NOT a failure; the UICR.APPROTECT readback is the actual
success criterion either way.

### M.6 If acceptance fails

| Failure phase | Most likely cause | Next step |
|---|---|---|
| 1 (Identify) | SWD contact / wiring | Re-seat pogo pins, check Vtgt, retry `--identify` |
| 2 (Mem roundtrip) | DP wedged from earlier work | Power-cycle Ring + Pico, retry |
| 3 (ERASEALL) | Charging puck not connected, internal Li-ion drooping | Confirm puck is delivering, retry |
| 4 (Verify erase) | Erase succeeded but readback returns stale value — likely AHB-AP cache | Try ERASE again; possible firmware bug if reproducible |
| 5 / 6 (Flash write) | NVMC state from prior incomplete op | Run RECOVER first, then re-run acceptance |
| 7a (UICR App) | NVMC config bug | Capture firmware log via Pico CDC, file issue |
| 7b (UICR Net) | Net stub didn't fault, just didn't complete | Capture firmware log; verify Net VMC RAMBLOCK powerset reached the chip |
| 8 (RECOVER) | Any of the above re-emerging on the second run | Should be impossible if phases 1-7 passed — file issue |

### M.7 What "passed" obligates

When this acceptance passes against a production Ring, update:

1. `firmware/projects/edev_dapv2_zephyr/releases/v0.1.8/RELEASE.md`
   — change the "Hardware acceptance" section from "11/11 PASS on
   bench nRF5340 DK" to "11/11 PASS on bench nRF5340 DK + Ring
   Pro 351 production silicon."
2. `docs/ESP32_BRIDGE.md` — change v0.1.8 status callout from
   "feature-complete (bench-validated)" to "released
   (production-validated)."
3. Memory note `project_edev_dapv2_zephyr_v017_feature_complete_2026_06_30.md`
   — strike the "Step 10 pending" line.
4. Tag the release as `v0.1.8-final` if not already tagged.

