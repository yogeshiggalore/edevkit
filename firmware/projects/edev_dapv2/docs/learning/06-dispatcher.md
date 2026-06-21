# Chapter 6 — Inside the Dispatcher

## Status

🚧 **Stub** — to be filled in. Outline below.

## What you'll learn

1. How a USB bulk OUT packet becomes a sequence of CMSIS-DAP command invocations.
2. The atomic-bundle commands (`DAP_QueueCommands` 0x7E, `DAP_ExecuteCommands` 0x7F) and why pyocd uses them aggressively.
3. The handler-function contract every command implementation follows.

## Outline

- **§6.1** USB ISR → ring buffer → main loop architecture (`src/usb/usb_dap_class.c`).
- **§6.2** `dap_dispatch` walks the OUT packet; one packet can contain back-to-back commands.
- **§6.3** Handler signature: `(req, req_avail, resp, resp_cap, *resp_used) → consumed_bytes`. Why this shape lets the dispatcher advance through atomic bundles.
- **§6.4** `DAP_ExecuteCommands` — wraps multiple commands so the host can submit them in one USB round-trip.
- **§6.5** Error handling — `DAP_Invalid` (0xFF) when an unknown cmd is seen.

## Source pointers

- `src/dap/dap.c::dap_dispatch` and `dispatch_one`.
- `src/dap/dap_internal.h` — the handler signature documented in the header comment.
- `src/dap/dap_atomic.c` — `DAP_ExecuteCommands` handler.
