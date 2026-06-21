# edev_dapv2 web tester

A WebUSB-based bring-up and bench tool for the edev_dapv2 probe. Plugs
straight into the probe over USB from a Chromium-based browser; no
host driver, no `pyocd`, no `probe-rs` — useful for triaging when
hosts can't see the probe and you want to confirm bytes are actually
moving on the bulk endpoints.

## What it does

1.  Connects to the probe via WebUSB (`navigator.usb.requestDevice`)
    with a VID/PID filter of `2e8a:000c`.
2.  Claims interface 0 (the vendor CMSIS-DAP interface).
3.  Sends DAP commands on bulk OUT EP `0x01`, reads responses on bulk
    IN EP `0x81`.
4.  For SWO streaming: continuously reads bulk IN EP `0x82`.

Panels in the UI:

-   **Probe info** — fires every `DAP_Info` subcommand, decodes the
    capability bitmap, dumps it as a table.
-   **SWD bring-up** — individual buttons for `DAP_Connect`,
    `DAP_SWJ_Clock`, `DAP_SWJ_Sequence` (canonical line reset +
    JTAG→SWD switch), `DAP_TransferConfigure`, `DAP_SWD_Configure`.
-   **Read IDCODE** — one-click full sequence to read the target's
    `DP[0]` IDCODE register. The first thing that should work end-to-end
    if SWD is wired correctly.
-   **Single transfer** — arbitrary `DAP_Transfer` against any DP or
    AP register, read or write.
-   **SWJ_Pins direct drive** — manually drive nRESET, read pin states.
-   **SWO trace** — start/stop SWO at a chosen baud, see decoded bytes
    streaming live.
-   **Raw hex command** — type any byte sequence, see the response.
    Useful for poking edge cases the higher-level panels don't cover.
-   **Log** — every transferred packet, plus diagnostic notes.

## Running it

WebUSB requires HTTPS or `localhost`. Easiest path: serve the file
locally.

```sh
cd edev_dapv2/tools/web
python3 -m http.server 8080
# then open http://localhost:8080/tester.html
```

Click **Connect**, pick the probe from the browser's USB chooser
dialog. If you're on Linux you may need a udev rule so the browser
can claim the device without root — see below.

## Linux udev rule

If `requestDevice` returns the probe but `claimInterface(0)` fails
with `NetworkError: Unable to claim interface`, the kernel's
`cdc_acm` / `cmsis-dap` modules have probably grabbed it first. Add
this rule:

```
# /etc/udev/rules.d/99-uh-dapv2.rules
SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="000c", MODE="0664", GROUP="plugdev", TAG+="uaccess"
```

Then `sudo udevadm control --reload-rules && sudo udevadm trigger`.

You may also need to **unbind** any kernel driver that has claimed
the interface — `probe-rs` and `pyocd` will refuse to share the
device with the browser. The simplest fix is to make sure nothing
else has a handle open before you click Connect.

## macOS / Windows

Should Just Work — no kernel driver needs to be evicted. Chrome and
Edge both support WebUSB.

## Limitations

-   IDCODE decoder is rudimentary (JEP106 designer + part number only).
    If you want a richer ROM-table walk, layer on top of the **Single
    transfer** panel or use `probe-rs info`.
-   SWO display assumes ASCII text. If your target emits raw ITM
    framing (stim port headers, sync bytes), non-printable bytes show
    up as `\xNN`.
-   No support for `DAP_TransferBlock` in the UI yet — for bulk reads
    you'll want a host tool. The probe firmware supports it; the UI
    just doesn't expose a panel for it.
