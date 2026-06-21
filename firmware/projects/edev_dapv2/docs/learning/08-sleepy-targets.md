# Chapter 8 — Sleepy Targets & Continuous SWCLK Keep-Alive

## What you'll learn

1. Why some chips (especially nRF5340 with low-power firmware) drop out of debug-reachable state within milliseconds.
2. J-Link's "continuous SWCLK" trick that keeps the chip's debug logic clocked between transactions.
3. How to implement this on the RP2350 using an idle PIO state machine, including the SM-handoff dance to the active SWD program for real transfers.

## §8.1 The failure mode

You connect, read `DPIDR` — perfect, `0x6BA02477` (Cortex-M33). You issue a second read milliseconds later — `DP_FAIL`. The chip went to sleep in between.

We see this on the Ring's nRF5340 in two distinct forms:

| Symptom | Cause | Recovery |
|---|---|---|
| `DPIDR` works, **AP** reads fail | DP is clocked, but the AHB-AP went to sleep | Re-handshake CTRL-AP keys |
| `DPIDR` fails on second poll | Whole DP fell into System OFF | nRESET pin (if wired) or VBUS cycle |

The deeper case ("System OFF") is described in `[[nrf5340-erased-chip-sleep]]`. This chapter focuses on the lighter case — the chip's debug logic is *gating its clock* on SWCLK activity.

## §8.2 Why chips do this

Modern low-power MCUs aggressively clock-gate everything that isn't strictly needed. The CoreSight DP and the Cortex-M33 debug fabric run off the **debug clock**, which is sourced from the active SWCLK pin when a debugger is attached. With no SWCLK edges arriving for a few hundred microseconds, the chip's clock-control logic decides:

> "Nobody's debugging. Power down DP. Save microamps."

When the next SWCLK edge finally arrives, DP has to spin back up, but in the meantime any pending transaction it had queued may be lost — hence the FAULT on the second read.

The nRF5340 datasheet doesn't document this explicitly. We inferred it from:

- The exact pattern: first read works, every subsequent read within the next ~10ms fails until we resync.
- J-Link's behavior — its "continuous SWCLK" feature exists for exactly this class of chips.
- Empirical: pulling SWCLK low and HOLDING it doesn't help; what helps is **keeping it toggling**.

📜 ARM Debug Interface Architecture (ADIv5) §B2.5 covers debug-clock gating but doesn't mandate it; whether the chip implements it is implementation-defined.

## §8.3 J-Link's solution

J-Link's firmware emits a low-frequency SWCLK pulse train (~1 kHz, configurable down to "as slow as the target allows without DP drop") whenever no real SWD transaction is in flight. Between back-to-back transactions, the gap is filled with idle clock. The chip never sees a quiet wire long enough to gate its DP.

From the host's perspective, J-Link looks faster than CMSIS-DAP probes for exactly this reason: many probes go quiet between transactions and pay a re-warm-up cost per access.

## §8.4 Our planned implementation

Two PIO programs sharing the SWCLK pin, with explicit handoff. PIO0 layout:

```
SM0  ← existing: probe.pio (active SWD bit-bang during real transactions)
SM1  ← new:      probe_keepalive.pio (idle SWCLK toggle at ~10 kHz when SM0 is idle)
```

### probe_keepalive.pio (sketch)

```pio
.program probe_keepalive
.side_set 1                    ; SWCLK on side-set pin

.wrap_target
    nop side 1 [31]            ; SWCLK high, hold 32 cycles
    nop side 0 [31]            ; SWCLK low, hold 32 cycles
.wrap
```

At sysclk = 125 MHz with these 32-cycle holds, each half-period is 256ns × 32 = ~256µs. Full cycle ~512µs → 1.95 kHz. Slow enough that the chip is satisfied, fast enough that the chip never sees more than ~256µs of quiet.

### Handoff protocol

The catch: only one SM can drive SWCLK at a time, or we get electrical contention. Solution:

```c
// hw/probe.c
void probe_swclk_keepalive_pause(void) {
    pio_sm_set_enabled(pio0, EDEV_PROBE_KEEPALIVE_SM, false);
    // Release the pin from SM1's side-set so SM0 can drive it.
    pio_sm_set_consecutive_pindirs(pio0, EDEV_PROBE_KEEPALIVE_SM,
                                   EDEV_PROBE_PIN_SWCLK, 1, false);
}

void probe_swclk_keepalive_resume(void) {
    // Take the pin back; SM1 drives.
    pio_sm_set_consecutive_pindirs(pio0, EDEV_PROBE_KEEPALIVE_SM,
                                   EDEV_PROBE_PIN_SWCLK, 1, true);
    pio_sm_set_enabled(pio0, EDEV_PROBE_KEEPALIVE_SM, true);
}
```

In `dap_swd.c::swd_transfer()`, wrap every transaction:

```c
probe_swclk_keepalive_pause();
// ... do the real SWD transfer (SM0) ...
probe_swclk_keepalive_resume();
```

Pause cost: a few PIO cycles. Less than the cost of one extra SWD bit. Free.

### Vendor command (optional)

For testing, expose enable/disable through `DAP_VENDOR 0x8B` (EDEV_KEEPALIVE_CTL):

```
Request:  [0x8B, enable:u8]
Response: [0x8B, status:u8]
```

`enable=0` lets us A/B compare with and without the keep-alive — important for validating it actually helps.

## §8.5 Caveats

- **System OFF defeats this.** Once the chip has fully cut VDD to its debug logic (an explicit firmware decision, e.g. `__WFE()` with deep-sleep modes), no amount of SWCLK toggling brings it back. Need nRESET assertion or VBUS cycle. See `[[nrf5340-erased-chip-sleep]]`.
- **Doesn't help against anti-extraction lock.** The Ring's NVMC throttle and APPROTECT re-engagement (`[[edevocd-validated-2026-06-19]]`) are independent mechanisms. SWCLK keep-alive helps with DP-level gating, not with the chip refusing to serve flash bytes.
- **Slight power cost on the target.** ~10 µA extra at 2 kHz toggling. Negligible during debug, but don't ship a fielded device with the probe attached running at full keep-alive 24/7.

## §8.6 What we've validated so far

In the 2026-06-19 cross-validation against J-Link/nrfjprog on twin Rings, **the handshake-and-halt protocol is correct** — every byte we capture matches the J-Link ground truth. What we lack is **endurance**: nrfjprog grinds through 36.6% of the flash by retrying obsessively for minutes; we currently capture small initial windows and then hit the chip's deep-lock state.

Continuous SWCLK keep-alive is one of the recovery-aggressiveness techniques. The full priority list (`[[jlink-ob-techniques-for-uh-dapv2]]`) includes:

1. **Dormant-state wakeup** (we already do this for first connect)
2. **Sticky-error auto-clear** between transactions (firmware-side improvement)
3. ⭐ **Continuous SWCLK keep-alive** (this chapter — biggest remaining win)
4. **Watchdog-reset escape** for chips fully in System OFF

Implementation order should be (2)→(3)→(4) — sticky-clear is cheapest, keep-alive is the biggest practical gain on sleepy chips, watchdog-reset is the heavy-lift fallback.

## §8.7 Try it (when implemented)

```bash
# Without keep-alive:
.venv/bin/pyocd dump-flash --unlock --family nRF5340-App \
    --start 0 --length 4096 -o /tmp/no-keepalive.bin -u <PROBE-UID>

# With keep-alive (once firmware lands):
# (currently controlled at firmware compile-time; will become a vendor cmd)
.venv/bin/pyocd dump-flash --unlock --family nRF5340-App \
    --start 0 --length 4096 -o /tmp/with-keepalive.bin -u <PROBE-UID> \
    --keepalive   # planned flag

# Expect: keep-alive version captures more bytes per session, especially
# on the Ring's nRF5340 where DP gating is the primary read-rate ceiling.
```

## Memory references

- `[[edevocd-validated-2026-06-19]]` — the cross-validation milestone (the recovery-rate gap this chapter aims to close)
- `[[nrf5340-erased-chip-sleep]]` — the System OFF case (keep-alive can't help here)
- `[[target-debug-power-pin]]` — full toolbox of staying-warm tricks
- `[[jlink-ob-techniques-for-uh-dapv2]]` — the concrete steal-list this chapter implements item ⭐ from
- `[[nrfjprog-read-flow-deep-dive]]` — what nrfjprog does that we don't (per-chunk retry tolerance)

## Source code map (when implemented)

- `src/pio/probe_keepalive.pio` — the 4-instruction SWCLK toggle
- `src/hw/probe.c` — `probe_swclk_keepalive_pause/_resume` + init
- `src/dap/dap_swd.c::swd_transfer()` — pause/resume wrap
- `src/dap/dap_vendor.c` — `do_edev_keepalive_ctl` (vendor cmd 0x8B)
