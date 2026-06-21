# usb_01_hello_enum

The smallest possible USB device that enumerates cleanly on Linux, macOS, and
Windows. Vendor class (`0xFF/0xFF/0xFF`), one interface, **zero data
endpoints** — the device sits there and answers descriptor reads.

This is **sample 1** of the USB sample series and pairs with **Book Chapter 05
(Enumeration walkthrough)** at `tools/web/pages/usb/partI/05_enumeration.html`.

---

## What you'll see when it works

After build + flash + plug in:

```
[00:00:00.001,000] <inf> usbd_init: bNumInterfaces 1 wTotalLength 18
[00:00:00.001,000] <inf> usb_01: usb_01 hello_enum: USB enabled, idle
[00:00:00.323,000] <inf> usbd_core: Actual device speed 1
[00:00:00.405,000] <inf> usbd_iface: No endpoints, skip interface
[00:00:00.405,000] <inf> hello_enum: configured (enabled)
```

`wTotalLength = 18` is the byte-level milestone — exactly what Book Ch 05
predicts: 9-byte configuration descriptor + 9-byte interface descriptor +
nothing else. `configured (enabled)` means the host completed
`SET_CONFIGURATION(1)`. From there the device idles forever.

On the host side:

```sh
# macOS — system_profiler can lag, ioreg is reliable
ioreg -p IOUSB -l -w 0 | grep -B 2 -A 25 "edevkit"

# Linux
lsusb -v -d 1209:ede1
```

VID `0x1209` (pid.codes umbrella), PID `0xede1` (usb_01 in our scheme),
manufacturer "edevkit", product "usb_01", serial auto-generated from the
RP2350 chip ID.

---

## Build, flash, monitor

Assuming you've sourced the edevkit zephyr helpers (added to
`~/.zshrc` and `$EDEV_VENV/bin/activate` — see
`tools/web/pages/firmware_workflow.html` for setup):

```sh
cd firmware/samples/usb/usb_01
edev_build              # west build -b rpi_pico2/rp2350a/m33 .
edev_run                # flash via Pico debug probe + stream RTT
```

Without the helpers, the equivalents are:

```sh
west build -b rpi_pico2/rp2350a/m33 .
probe-rs run --chip RP2350 build/zephyr/zephyr.elf
```

---

## File layout

```
usb_01/
├── CMakeLists.txt          standard Zephyr boilerplate
├── prj.conf                Kconfig: USB-Device-Next + RTT console
├── app.overlay             devicetree fragment enabling RP2350 USB controller
├── README.md               this file
└── src/
    ├── main.c              USB context init, run usbd_enable, idle
    └── hello_enum.c        the no-op vendor class implementation
```

### `prj.conf`

| Kconfig | Why |
|---|---|
| `CONFIG_USB_DEVICE_STACK_NEXT=y` | Use the new "device next" stack, not the legacy one. **Forgetting this leaves all `USBD_*` macros undefined.** |
| `CONFIG_LOG=y` + `CONFIG_USBD_LOG_LEVEL_INF=y` | Lifecycle logs for enumeration |
| `CONFIG_USE_SEGGER_RTT=y` + `CONFIG_RTT_CONSOLE=y` + `CONFIG_UART_CONSOLE=n` | Console over SWD via RTT (no UART pins burned) |
| `CONFIG_HEAP_MEM_POOL_SIZE=2048` | USB stack uses some heap |

### `app.overlay`

Enables the RP2350's USB controller node. The exact node label comes from the
board's `.dts` — grep `compatible` for `raspberrypi,pico-usbd` or similar.
Typical name is `usbd` or `usbd0`.

### `src/main.c`

Sequence in `main()`:

1. `usbd_add_descriptor` × 4 — add language-IDs, manufacturer, product, serial
2. `usbd_add_configuration` — add the one configuration at FS speed
3. `usbd_register_class` — attach the `hello_enum` class to that configuration
4. `usbd_init` — finalize the descriptor tree (validates, computes
   `wTotalLength`, links indices)
5. `usbd_enable` — connect the D+ pull-up so the host can see us
6. `k_sleep(K_FOREVER)` — stack runs in its own work queue; main idles

### `src/hello_enum.c`

The minimal vendor class. Three pieces:

1. **Interface descriptor** — 9 bytes, vendor `0xFF/0xFF/0xFF`, zero endpoints.
2. **Descriptor pointer array** — `static struct usb_desc_header *fs_desc[]
   = { ..., NULL }`. The USB-Device-Next core walks `get_desc`'s return value
   as a NULL-terminated array of pointers, **not as a single descriptor**.
   Returning a bare `&iface` causes a bus fault on second iteration. (See
   "Bug 4" below.)
3. **Class API vtable** — only four real callbacks:
   `init`, `enable` (`void`), `disable` (`void`), `get_desc` (returns the
   array). Everything else is `NULL` and the core handles that.

Then `USBD_DEFINE_CLASS(hello_enum, &hello_enum_api, NULL, NULL)` registers
the instance under the name `"hello_enum"` — which must match the string
passed to `usbd_register_class` in `main.c`.

---

## Descriptor design choices

Locked across the entire sample series — don't change per-sample:

| Field | Value | Reason |
|---|---|---|
| **VID** | `0x1209` | pid.codes open-hardware umbrella |
| **PID** | `0xede1` | Per-sample: `0xede0 + sample_number` (so `ede1` for usb_01, `ede2` for usb_02, …). Lets us grep VID:PID in captures and instantly know which sample is plugged in. |
| **Manufacturer** | `"edevkit"` | Constant across all samples |
| **Product** | `"usb_01"` | Per-sample, matches the directory name |
| **Serial** | auto-generated | Zephyr's `USBD_DESC_SERIAL_NUMBER_DEFINE(name)` reads the RP2350 chip ID via `hwinfo_get_device_id()` and converts to hex — no extra code needed. usb_03 will customize the format. |
| **bcdDevice** | `0x0100` | **Bump on every iteration** — macOS aggressively caches strings keyed on `(VID, PID, bcdDevice)`. Forgetting to bump means you keep seeing the old strings. |
| **bMaxPower** | 100 mA | conservative bus-powered budget |
| **bmAttributes** | `USB_SCD_REMOTE_WAKEUP` | declares capability so we don't have to change it later |

---

## Bugs we hit while bringing this up (reference for clones)

If you fork this sample as a starting point, these are the seven mistakes
most likely to bite you:

1. **`USBD_DESC_SERIAL_NUMBER_DEFINE` arity** — takes ONE argument
   (just the descriptor name), not `(name, "string")`. Zephyr generates the
   serial automatically. Verify in your tree:
   ```sh
   grep -n "USBD_DESC_SERIAL_NUMBER_DEFINE" \
     $ZEPHYR_BASE/include/zephyr/usb/usbd.h
   ```
2. **Wrong devicetree node label** — guessing the USB controller node name
   fails at compile time with `'__device_dts_ord_DT_N_NODELABEL_xxx_ORD'
   undeclared`. Find the real label:
   ```sh
   grep -i "compatible.*pico-usbd\|usbd:" \
     $ZEPHYR_BASE/boards/raspberrypi/rpi_pico2/*.dts
   ```
3. **`enable`/`disable` callbacks return `void`**, not `int`. Also
   `feature_halt`, `update`, `suspended`, `resumed`, `sof`, `shutdown` —
   all `void`. Only `init`, `request`, `control_to_dev`, `control_to_host`
   return `int`.
4. **`get_desc` must return a NULL-terminated array of `struct
   usb_desc_header *`** — not a single descriptor pointer. Returning a bare
   `&iface` crashes with `BUS FAULT, BFAR=0xffffff00` because the core walks
   the descriptor bytes as if they were pointers. Always wrap in:
   ```c
   static struct usb_desc_header *fs_desc[] = {
       (struct usb_desc_header *) &your_iface,
       NULL,
   };
   ```
5. **Don't `const` the interface descriptor** — Zephyr writes
   `bInterfaceNumber` at init time. Marking it `const` causes a silent
   crash if it lands in flash.
6. **Class name string mismatch** between `USBD_DEFINE_CLASS(name, …)` and
   `usbd_register_class(&ctx, "name", …)` causes registration to silently
   fail with `-ENOENT`. Use a single `#define` to pin the name.
7. **Returning `0` from `control_to_*` for unsupported requests**, instead
   of `-ENOTSUP`, makes the host wait forever for data. Always return
   `-ENOTSUP` for class-specific requests you don't implement.

---

## Optional polish

Zephyr USB-Device-Next defaults the device-class triple to `0xEF/0x02/0x01`
(IAD). Book Ch 05's worked example expects `0x00/0x00/0x00` ("see interface
descriptors"). Functionally harmless — every host accepts both — but to
match the chapter byte-perfectly, add this between `usbd_init` and
`usbd_enable` in `main.c`:

```c
usbd_device_set_code_triple(&usbd_ctx, USBD_SPEED_FS, 0, 0, 0);
```

---

## Related docs

- **Book Ch 05 — Enumeration walkthrough** —
  `tools/web/pages/usb/partI/05_enumeration.html`
- **Zephyr USB-Device-Next reference** —
  `tools/web/pages/firmware_zephyr_usbd.html` (API + gotchas catalogue)
- **Firmware workflow** —
  `tools/web/pages/firmware_workflow.html`
  (env vars, `edev_*` helpers, RTT, coredump, USB verification)
