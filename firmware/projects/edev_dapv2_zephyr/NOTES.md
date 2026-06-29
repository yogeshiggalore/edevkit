# NOTES — Zephyr `subsys/dap` ↔ pico-sdk `dap_*` divergence audit

Performed 2026-06-29. Inputs:

- `zephyrproject/zephyr/subsys/dap/cmsis_dap.c` @ v4.4.1 (1039 LoC)
- `zephyrproject/zephyr/subsys/dap/dap_backend_usb.c` @ v4.4.1
- `firmware/projects/edev_dapv2/src/dap/` (our pico-sdk dispatcher)
- `firmware/projects/edev_dapv2/src/hw/probe.c` (PIO SWD)

The five critical SWD fixes from the v0.2 pico-sdk rebuild (commit `6694220`) — and where they land in the Zephyr port.

## Fix 1: SWDIO updates AFTER falling edge of SWCLK (hold-time fix)

**Where it lives:** PIO program (`pio/probe_swd.pio`). The `out pins, 1 side 0 [2]` instruction outputs the data bit on the same cycle SWCLK goes LOW (side-set 0), then `[2]` waits two cycles before letting `jmp ... side 1 [2]` rise the clock. The host's SWDIO transition happens during the SWCLK LOW phase, satisfying the target's hold-time spec.

**Port status:** ✅ Preserved verbatim — `pio/probe_swd.pio` is byte-identical to the pico-sdk source.

## Fix 2: WAIT/FAULT data-phase asymmetry

In ADIv5, if the target returns ACK=WAIT or FAULT on a read, the target keeps driving the line for 33 dummy cycles. On a write, the host turns the line around and drives 33 zeros. Getting this wrong (we did, on attempt N) wedges the bus.

**Where it lives:** `drivers/dp/swdp_pio_rpi_pico.c` `api_transfer()` — the `if (d->data_phase) { if (is_read) ... else ... }` block. Mirrors the same conditional in `firmware/projects/edev_dapv2/src/hw/probe.c::probe_swd_xact()`.

**Port status:** ✅ Preserved in the new SWDP-PIO driver.

## Fix 3: Posted-AP-read pipeline at the DAP dispatcher

When the host sends DAP_Transfer with a sequence of AP reads, the first read returns nothing useful (just posts the operation). The second read returns the first AP value. The final AP value comes from an explicit DP.RDBUFF read.

**Where it lives in Zephyr:** `subsys/dap/cmsis_dap.c::dap_swdp_transfer()`, lines 467–579. The `post_read` flag tracks whether an AP read is in flight. On the next AP read it returns the previous AP value first. On any DP/non-AP intervening op it issues DP.RDBUFF to flush. At end-of-batch it issues DP.RDBUFF as final flush. Matches what our `src/dap/dap_swd.c::dap_handle_transfer()` does.

**Port status:** ✅ No patch needed — upstream Zephyr already does this correctly. Carefully verified against our code line-by-line.

## Fix 4: Don't `++executed` on failure

If a transfer fails (WAIT or FAULT), the response `transfer_count` field should reflect ops *completed*, not ops *attempted*. The host infers the failing index from `executed`.

**Where it lives in Zephyr:** `subsys/dap/cmsis_dap.c`, the `rspns_cnt++` at line 557 runs **inside** the for loop **after** the inner blocks. On a `break` from any inner block, `rspns_cnt` is not incremented for the failing op. Identical to our dispatcher.

**Port status:** ✅ No patch needed.

## Fix 5: Parity-error retry in the wire wrapper

When the target's data-phase parity bit doesn't match the computed parity, we retry the whole transaction up to 8 times before returning a transfer error.

**Where it lives:** `drivers/dp/swdp_pio_rpi_pico.c::api_transfer()` — the `for (int attempt = 0; attempt < 8; attempt++)` loop. Identical retry budget to `probe_swd_transfer()` in the pico-sdk source.

**Port status:** ✅ Preserved in the new SWDP-PIO driver.

## SWCLK keep-alive (continuous SWCLK)

**Not in current pico-sdk firmware.** The keep-alive PIO program lived in the old `uh_dapv2` firmware (commit `0b89a48` ish, June 19 2026). The v0.2 PIO rebuild on June 25 (commit `6694220`) was from scratch and did not carry it over. Production has run without it ever since on Ring Pro/Air nRF chips.

**Port status:** Parity with current pico-sdk firmware — also not in the Zephyr port. If/when nRF debug-domain sleep becomes a regression again, add a second PIO state machine (PIO1 SM1, paired with our SWD SM0) running a 2-instruction toggle. The handoff design in `~/.claude/.../project_swclk_keepalive_pio_2026_06_19.md` memory describes the relinquish/takeover dance needed to share SWCLK without electrical contention.

## DAP_FW_VER

Zephyr hardcodes `"2.1.0"` in `subsys/dap/cmsis_dap.h` line 26. Our pico-sdk advertises `"2.2.0"`. The actual host-side gate (probe-rs / pyocd) checks **bcdDevice ≥ 0x0220** in the USB device descriptor, NOT the DAP_Info FW string. We force `bcdDevice = 0x0220` via `usbd_device_set_bcd_device()` in `src/main.c`, so the gate passes regardless of the string value.

**Port status:** ✅ Acceptable as-is. If a host tool ever gates on the string, patch `cmsis_dap.h` out-of-tree.

## Conclusion

**Zephyr's upstream `subsys/dap/cmsis_dap.c` is sufficient.** All five critical SWD fixes from the pico-sdk rebuild are either already implemented upstream (fixes 3, 4) or live below the DAP dispatcher in our PIO driver (fixes 1, 2, 5). The only port-time concern was the SWCLK keep-alive, which the current pico-sdk firmware doesn't carry either — so we're at parity. The bcdDevice gate is handled via the USBD framework setter at runtime.

No patches required. This file is the audit trail.
