# edev_dapv2 — Web Test GUI

Browser-based test harness for the edev_dapv2 probe. Wraps `probe-rs`
for routine operations and uses `pyocd`'s Python API for deep DAP
diagnostics. Runs locally; no host-side state survives a restart.

## What it does

- Lists connected probes (any CMSIS-DAP / J-Link / etc.)
- Connects to a chosen target chip at a chosen SWD clock
- Shows DPIDR / DLPIDR / AHB-AP IDR / CSW after connect
- Reads arbitrary memory ranges (hex + ASCII dump)
- Dumps full chip flash with live progress, saves `.bin`, shows MD5
- Compares against a reference `.bin` / Intel `.hex` byte-by-byte
- Streams the probe's USB-CDC log live (DAP traces, fault info)
- Runs pyocd-based diagnostics for deep transaction-level inspection

## Quick start

```sh
# from edev_dapv2/tools/webgui
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python server.py
# open http://127.0.0.1:8766
```

The probe must already be flashed with the edev_dapv2 firmware. The
host needs `probe-rs` on `$PATH` (Homebrew install works).

## Architecture

```
┌───────────────────────────────┐
│  Browser (one HTML file)      │ Tailwind CDN, vanilla JS, SSE
└──────────────┬────────────────┘
               │ HTTP / SSE
┌──────────────┴────────────────┐
│  FastAPI server :8766         │ server.py
│  ┌────────────────────────┐   │
│  │ probers.py             │   │ → subprocess  probe-rs list/info/read
│  │ pyocd_diag.py          │   │ → pyocd.core  for deep diagnostics
│  │ cdc_log.py             │   │ → pyserial    for live CDC tail
│  └────────────────────────┘   │
└──────────────┬────────────────┘
               │ USB
┌──────────────┴────────────────┐
│  edev_dapv2 probe (Pico 2 W)  │
└──────────────┬────────────────┘
               │ SWD (GP2 / GP3)
┌──────────────┴────────────────┐
│  Target chip                  │
└───────────────────────────────┘
```

## URLs / API

| Path | What |
| --- | --- |
| `GET /` | the page |
| `GET /api/probes` | enumerate connected probes |
| `POST /api/connect` | set chip + speed for the current session |
| `GET /api/info` | DPIDR / AP IDR / CSW snapshot |
| `POST /api/read` | read N words at address |
| `POST /api/dump` | start full-flash dump (returns job id) |
| `GET /api/dump/{id}/progress` | SSE: bytes-done updates |
| `GET /api/dump/{id}/data` | download the dump `.bin` |
| `POST /api/compare` | upload a reference, returns diff stats |
| `GET /api/cdc/log` | SSE: live CDC log lines |
| `POST /api/diag` | pyocd-based DAP diagnostics |
