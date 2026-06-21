# Chapter 5 — CMSIS-DAP v2 Protocol

## What you'll learn

1. **What CMSIS-DAP v2 is** and how it differs from v1 (HID-based), and why every modern host tool prefers v2.
2. **How a host's "read DPIDR" call maps to USB bulk packets** that edev_dapv2 sees on its endpoints.
3. **The command/response framing** every CMSIS-DAP command shares, with the capability byte and command-ID space (including the 0x80–0xBF vendor range).

## Concept

### 5.1 What CMSIS-DAP is

CMSIS-DAP is **ARM's open USB protocol** for debug probes. Pre-CMSIS-DAP, each probe vendor had its own protocol (J-Link, ST-LINK, Black Magic, etc.) and your host tool needed a per-vendor driver. CMSIS-DAP standardised a small set of commands so any conforming probe works with any conforming host — out of the box.

**Two versions exist:**

| Version | Transport          | Speed         | Notes |
|---------|--------------------|---------------|-------|
| **v1**  | USB HID            | ~10–100 kB/s  | Driverless on Windows; slow because HID limits packet rate |
| **v2**  | USB Bulk endpoints | ~1–10 MB/s    | Fast; requires WinUSB driver on Windows (auto-installs via MS OS 2.0 descriptors) |

edev_dapv2 implements v2 only. Modern hosts (probe-rs, pyocd, OpenOCD 0.11+) prefer v2 and fall back to v1 only for legacy probes.

### 5.2 What's on the USB wire

edev_dapv2 exposes a single USB interface with three endpoints:

```
Endpoint 0x01 OUT (bulk)  ← host sends DAP commands here
Endpoint 0x81 IN  (bulk)  → probe sends responses here
Endpoint 0x82 IN  (bulk)  → probe streams SWO (instrumentation trace) here
```

A "DAP transaction" from the host's view is:

1. Build a command byte array (e.g. `[0x05, 0x00, 0x02, 0x05, 0xA5]` for a DAP_Transfer with 2 transfers).
2. Write it to endpoint 0x01.
3. Wait for response on endpoint 0x81.
4. Parse the response.

### 5.3 Command framing

Every CMSIS-DAP command and response starts with a **command ID byte**. The response echoes the command ID, then status/data bytes follow:

```
Host → probe:  [CMD_ID] [payload bytes for that command]
Probe → host:  [CMD_ID] [status + data bytes for that command's response]
```

Exception: an unknown command yields `[0xFF]` (DAP_Invalid) instead of the echo.

The complete v2 command ID space:

| Range       | Purpose |
|-------------|---------|
| 0x00 – 0x09 | DAP general (Info, Connect, Disconnect, Transfer, etc.) |
| 0x0A        | DAP_ResetTarget (device-specific reset, optional) |
| 0x10 – 0x12 | SWJ (Pins, Clock, Sequence) |
| 0x13, 0x1D  | SWD (Configure, Sequence) |
| 0x14 – 0x16 | JTAG (Sequence, Configure, IDCODE) |
| 0x17 – 0x1F | SWO (Transport, Mode, Baudrate, Control, Status, Data, …) |
| 0x7E – 0x7F | DAP_QueueCommands / DAP_ExecuteCommands (atomic bundles) |
| **0x80 – 0xBF** | **Vendor extensions** (32 slots) — see Chapter 7 |

edev_dapv2's full enum lives in `src/dap/dap_config.h`. Worth a one-pass read to anchor the namespace in your head.

### 5.4 The capability byte

Some commands are optional (SWO, atomic bundles, etc.). The probe tells the host which it supports via `DAP_Info(0xF0)` — the **capability bits**:

```
bit 0: SWD supported
bit 1: JTAG supported
bit 2: SWO UART supported (Manchester-style framing)
bit 3: SWO Manchester supported
bit 4: Atomic commands supported (DAP_ExecuteCommands)
bit 5: TD Timer (timestamp) supported
bit 6: SWO streamed via separate endpoint
bit 7: UART communication port supported
```

edev_dapv2 v0.1 advertises `0x13` = SWD + JTAG + Atomic. SWO bits stay clear until Milestone 7 (the ring buffer isn't in yet). **Lying about capabilities is the #1 source of "host hangs" bugs** — if you set bit 2 but return zero bytes on every SWO_Data poll, OpenOCD enters a busy-loop watching an empty buffer. We hit this exact bug; see `[[uh-dapv2-caps-byte-decision]]`.

### 5.5 An example: reading DPIDR end-to-end

Let's trace `edevocd nrf-dpidr` from the user typing the command to the chip ACKing on the wire.

**Step 1 — Host opens the probe.**
```
USB control xfers: SET_CONFIGURATION, GET_DESCRIPTORs, …
```
WinUSB driver loads (Windows) or libusb claims the interface (macOS/Linux). All driverless because edev_dapv2 ships an MS OS 2.0 BOS descriptor pointing at WinUSB.

**Step 2 — Host configures the wire.**
```
Host → 0x01 OUT: [0x02, 0x01]                        ; DAP_Connect(SWD)
Probe → 0x81 IN: [0x02, 0x01]                        ; SWD selected
Host → 0x01 OUT: [0x11, 0x80, 0x96, 0x98, 0x00]      ; DAP_SWJ_Clock(10_000_000 Hz)
Probe → 0x81 IN: [0x11, 0x00]                        ; OK
Host → 0x01 OUT: [0x12, 0x33, 0xFF, 0xFF, 0xFF, 0xFF, ...]  ; DAP_SWJ_Sequence(51 ones)
Probe → 0x81 IN: [0x12, 0x00]
Host → 0x01 OUT: [0x12, 0x10, 0x9E, 0xE7]            ; DAP_SWJ_Sequence(16 bits = 0xE79E)
Probe → 0x81 IN: [0x12, 0x00]
... (more SWJ sequence segments)
Host → 0x01 OUT: [0x04, 0x00, 0x40, 0x00, 0x00, 0x00]; DAP_TransferConfigure
Probe → 0x81 IN: [0x04, 0x00]
Host → 0x01 OUT: [0x13, 0x00]                        ; DAP_SWD_Configure(1 turnaround, no data phase)
Probe → 0x81 IN: [0x13, 0x00]
```

**Step 3 — Host reads DPIDR via DAP_Transfer.**
```
Host → 0x01 OUT: [0x05, 0x00, 0x01, 0x02]            ; DAP_Transfer: 1 transfer, request byte 0x02 = DP read of register 0x00 (DPIDR)
                                                       ;  ↑     ↑     ↑     ↑
                                                       ; cmd  dap_idx count request
```

`request byte 0x02` decodes to: APnDP=0, RnW=1, A2=0, A3=0 → DP read at offset 0. That's DPIDR.

**Step 4 — edev_dapv2 dispatches.**
```
USB ISR → ring buffer → main loop → dap_dispatch(req, len, resp, cap)
                                          │
                          dispatch_one() looks at req[0] = 0x05 → DAP_Transfer
                                          │
                                          ▼
                          dap_handle_transfer() in src/dap/dap_swd.c
                                          │
                                          ▼
                          swd_transfer(0x02, &val)
                                          │
                          probe.pio bit-bangs the 46-bit packet onto SWCLK/SWDIO
                                          │
                                          ▼
                          target ACKs OK, sends 32-bit DPIDR data, parity ok
                                          │
                                          ▼
                          response buffer built up
```

**Step 5 — Probe → host response.**
```
Probe → 0x81 IN: [0x05, 0x01, 0x01, 0x77, 0x24, 0xA0, 0x6B]
                  │     │     │     └─── 32-bit DPIDR (LE) = 0x6BA02477
                  │     │     └─ ACK = 0x01 (OK)
                  │     └─ count of completed transfers = 1
                  └─ command ID echo
```

Host parses `0x6BA02477` as DPIDR, then prints it. Done.

### 5.6 Why edev_dapv2 advertises `bcdDevice = 0x0220`

Specifically `0x0220` because probe-rs has a hard-coded check that rejects CMSIS-DAP v2 probes with `bcdDevice < 0x0220`. It's documented in `[[probe-rs-bcd-device-gate]]`. This is the only place where a magic-number constant has bitten enough integrators to warrant a memory entry of its own.

## In the spec

- **CMSIS-DAP** — there is no central ARM-IHI document; the canonical reference is **`Components/cmsis-dap/Doc/index.html`** in the CMSIS_5 GitHub repo: https://github.com/ARM-software/CMSIS_5/tree/develop/CMSIS/DAP
- The spec is rendered in HTML; the key pages are *Commands* (every command, request/response) and *Implementation Details*.
- USB-side: the **WinUSB / MS OS 2.0 descriptors** are documented in Microsoft's [USB MSOS-2.0 specification](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-os-2-0-descriptors-specification).

## In our code

🛠 **`src/usb/usb_descriptors.c`** — USB descriptors: device, configuration, interface, endpoint, MS OS 2.0 BOS. ~250 lines, mostly one-time setup.

🛠 **`src/usb/usb_dap_class.c`** — bulk endpoint plumbing. Reads OUT-EP packets into a ring buffer; ferries response packets out the IN-EP.

🛠 **`src/dap/dap.c`** — the dispatcher. ~170 lines. Function `dap_dispatch` walks the request buffer (which may contain back-to-back commands) and routes each one to the matching handler.

🛠 **`src/dap/dap_info.c`** — handles `DAP_Info` (0x00) including the capability byte. Read it alongside the `DAP_CAPABILITIES_BYTE0` define in `dap_config.h`.

🛠 **`src/dap/dap_swd.c::dap_handle_transfer()`** — the heart. Walks the `DAP_Transfer` request, calls `swd_transfer()` once per requested DP/AP transaction.

## Try it

🧪 **Watch USB traffic on macOS** with the `wireshark` package + USBPcap or the built-in `XHCI Capture`:

```bash
# macOS: enable USB packet capture for the edev_dapv2 interface
sudo ifconfig XHC20 up
# then in Wireshark, capture on the XHC interface and filter usb.idVendor == 0x2e8a
```

You'll see the exact byte sequences from §5.5 flow over the bulk endpoints.

🧪 **See edev_dapv2's capability byte:**

```bash
.venv/bin/pyocd cmd 0x00 0xF0
# (raw DAP_Info(CAPABILITIES) call)
```

🧪 **Inspect what each tool actually sends:** the **webdbg** (`tools/webdbg/`) browser bench captures the verbose log from each of pyocd / probe-rs / openocd / edevocd against the same probe + target. Diffing the logs is the fastest way to understand each host's "open + connect" choreography.

## Gotchas

⚠️ **CMSIS-DAP v1 (HID) and v2 (bulk) can coexist on the same probe** with two USB interfaces. edev_dapv2 implements v2 only. If a host tries v1 first (some still do), it'll fall back to v2 after one round-trip — adds ~10 ms of probe-attach latency.

⚠️ **The packet size you advertise via `DAP_Info(0xFF)` must match your real USB packet size.** We advertise 512 bytes; our USB descriptors must say `wMaxPacketSize=512`. Mismatch → silent corruption when the host stuffs a 256-byte payload thinking the probe will accept up to 512.

⚠️ **Packet count `DAP_Info(0xFE)` enables host-side pipelining.** We advertise 4, meaning the host can have up to 4 OUT packets queued before waiting for responses. Too low → throughput suffers. Too high → if the probe stalls, host buffers explode.

⚠️ **The dispatcher must handle "request packet contains multiple back-to-back commands"** — yes, CMSIS-DAP v2 allows that. Our `dap_dispatch` walks the buffer until it's consumed. Easy to miss in a first-pass implementation; you'll get "first command works, second is ignored" symptoms.

## Further reading

- **Chapter 6** — *Inside the dispatcher* — walks `dap.c` end to end and explains how `DAP_ExecuteCommands` (atomic bundling) interacts with the dispatcher.
- **Chapter 7** — *Vendor commands* — what we put in 0x80–0xBF and why.
- `[[probe-rs-bcd-device-gate]]` memory — the `bcdDevice ≥ 0x0220` requirement.
- `[[uh-dapv2-caps-byte-decision]]` memory — caps byte 0x57 → 0x13 fix story.
- CMSIS-DAP source-of-truth HTML: https://github.com/ARM-software/CMSIS_5/tree/develop/CMSIS/DAP
