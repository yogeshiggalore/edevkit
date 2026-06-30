# edev_dapv2_zephyr — Web Test UI

Browser UI for the `edev_dapv2_zephyr` probe vendor commands.
Wraps the bench-validated Python primitives (`test_nrf53_vendor_cmds.py`,
`_program_and_verify.py`) behind a tiny FastAPI server.

Runs on `http://127.0.0.1:8766/`.

## What you can do from the browser

| Section | What it does |
|---|---|
| **Identify Target** | Calls `0x89 NRF53_TARGET_INFO` — DPIDR, AP_IDR, CPUID, family/core detection |
| **ERASE** | `0x85 NRF53_ERASE` — CTRL-AP ERASEALL on every core (1 on nRF52, 2 on nRF5340) |
| **RECOVER** | `0x84 NRF53_RECOVER` — full unlock chain (nRF5340 only) |
| **Flash & Verify** | Upload an Intel-hex; auto-erases, programs all segments, reads back, hash-compares per segment + whole-image. Live progress via Server-Sent Events. |
| **Read Memory** | Hexdump any address range OR download as `.bin` |
| **USB reset** | Hard-reset the probe's USB interface (recovers from wedged state) |

## Install

```bash
cd firmware/projects/edev_dapv2_zephyr/tools/webtest
pip install fastapi uvicorn python-multipart intelhex pyusb
```

(`pyusb` should already be installed from the existing test harness.)

## Run

```bash
python3 server.py
```

Then open <http://127.0.0.1:8766/> in any browser.

## Architecture

- **server.py** — FastAPI app. Imports `Probe`, `dap_connect_and_reset`,
  and the vendor-cmd constants from `../test_nrf53_vendor_cmds.py`.
  Single global `Probe` handle behind a `threading.Lock` — USB is
  exclusive so only one operation runs at a time.
- **index.html** — single-page vanilla JS (no build step, no framework).
  Talks to the API via `fetch`. Flash progress uses a streaming
  Server-Sent Events response from `/api/flash`.

## Endpoints

| Verb | Path | Purpose |
|---|---|---|
| GET | `/` | Main page |
| GET | `/api/info` | TARGET_INFO → JSON |
| POST | `/api/erase` | ERASEALL → JSON `{ap_count, elapsed_s}` |
| POST | `/api/recover` | Full RECOVER → JSON with all UICR fields |
| POST | `/api/flash` | multipart `file=<hex>` → SSE stream (`event: log`, `event: done`) |
| GET | `/api/read?addr=&length=&fmt=hex\|bin` | Read mem; hex JSON or raw .bin |
| POST | `/api/usb_reset` | Force USB-level device reset |

## Notes

- The probe must be running `edev_dapv2_zephyr` firmware that includes
  the Net flash auto-erase fix (Net page-0 dirty tracking). v0.1.7 +
  any commit ≥ the auto-erase fix.
- Long ops (flash + verify of a 700 KB image) take ~5 min total at
  ~4 KB/s write + ~10 KB/s read. The UI shows live progress.
- All destructive operations prompt for confirmation in the browser.
