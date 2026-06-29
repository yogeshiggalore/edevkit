/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Raspberry Pi Pico PIO-driven SWDP driver.
 *
 * Ports the PIO bit-bang from firmware/projects/edev_dapv2/src/hw/probe.c
 * (pico-sdk) to Zephyr's swdp_api (include/zephyr/drivers/swdp.h). The PIO
 * program (probe_swd.pio) is unchanged — same 6 PIO cycles per SWD bit,
 * deterministic timing, autopull threshold 32, side-set on SWCLK, set/out/in
 * pins all aliased to SWDIO with pindirs flipped between phases.
 *
 * Hardware loop:
 *   - PIO state machine runs the `start` loop, blocking on TX FIFO
 *   - The driver writes an opcode word: (function_offset) | (count-1)<<8 |
 *     (data<<13). PIO `out pc, 8` jumps to write_bits or read_bits.
 *   - write_bits drives SWDIO LSB first while toggling SWCLK
 *   - read_bits sets SWDIO Hi-Z, samples on rising SWCLK, pushes the result
 */

#define DT_DRV_COMPAT raspberrypi_pico_swdp_pio

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/drivers/swdp.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hardware/clocks.h>
#include <hardware/pio.h>

#include "probe_swd.pio.h"

LOG_MODULE_REGISTER(swdp_pio_rpi, CONFIG_DP_DRIVER_LOG_LEVEL);

/* ----------------------------------------------------------------------
 * Per-device state
 * ---------------------------------------------------------------------- */

struct swdp_pio_config {
	const struct device *piodev;
	struct gpio_dt_spec clk_gpio;
	struct gpio_dt_spec dio_gpio;
	struct gpio_dt_spec reset_gpio;	/* may be unbound */
	bool reset_present;
};

struct swdp_pio_data {
	size_t sm;		/* claimed state machine index */
	uint sm_offset;		/* PIO program load offset */
	uint32_t clock_hz;	/* SWD frequency in Hz */
	uint8_t turnaround;	/* line turnaround clocks (1..4) */
	uint8_t data_phase;	/* 0 = abort on WAIT/FAULT; 1 = full data phase */
	bool enabled;		/* port_on() called */
};

/* ----------------------------------------------------------------------
 * PIO primitives — direct ports of the corresponding helpers in probe.c
 * ---------------------------------------------------------------------- */

static inline uint32_t pio_opcode(uint sm_offset, uint8_t func_offset,
				  uint8_t count, uint32_t data)
{
	return (uint32_t)(func_offset + sm_offset)
	     | (((uint32_t)(count - 1u) & 0x1Fu) << 8)
	     | ((data & 0x7FFFFu) << 13);
}

static inline void drain_rx(PIO pio, uint sm)
{
	while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
		(void)pio_sm_get(pio, sm);
	}
}

static void swd_write_n(const struct device *dev, uint32_t data, uint8_t n)
{
	const struct swdp_pio_config *cfg = dev->config;
	struct swdp_pio_data *d = dev->data;
	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);
	uint32_t op = pio_opcode(d->sm_offset, probe_swd_offset_write_bits,
				 n, data);

	pio_sm_put_blocking(pio, d->sm, op);
	if (n > 19) {
		/* Bits 19..31 ride in a second word; PIO autopull fetches it. */
		pio_sm_put_blocking(pio, d->sm, data >> 19);
	}
}

static uint32_t swd_read_n(const struct device *dev, uint8_t n)
{
	const struct swdp_pio_config *cfg = dev->config;
	struct swdp_pio_data *d = dev->data;
	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);
	uint32_t op = pio_opcode(d->sm_offset, probe_swd_offset_read_bits,
				 n, 0);

	pio_sm_put_blocking(pio, d->sm, op);

	/* Bounded wait — matches the 10 ms-ish budget in probe.c. Wedged-SM
	 * recovery: clear FIFOs, restart, re-jump to start. */
	uint32_t guard = 1500000u;
	while (pio_sm_is_rx_fifo_empty(pio, d->sm)) {
		if (--guard == 0) {
			pio_sm_clear_fifos(pio, d->sm);
			pio_sm_restart(pio, d->sm);
			pio_sm_exec(pio, d->sm,
				    pio_encode_jmp(d->sm_offset +
						    probe_swd_offset_start));
			return 0xFFFFFFFFu;
		}
	}

	uint32_t v = pio_sm_get(pio, d->sm);
	if (n < 32) {
		v >>= (32 - n);
	}
	return v;
}

static inline void swd_clock_idle_n(const struct device *dev, uint8_t n)
{
	(void)swd_read_n(dev, n);
}

/* ----------------------------------------------------------------------
 * SM configuration
 * ---------------------------------------------------------------------- */

static void sm_configure(const struct device *dev)
{
	const struct swdp_pio_config *cfg = dev->config;
	struct swdp_pio_data *d = dev->data;
	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);

	pio_sm_set_enabled(pio, d->sm, false);

	pio_sm_config c = probe_swd_program_get_default_config(d->sm_offset);

	sm_config_set_sideset_pins(&c, cfg->clk_gpio.pin);
	sm_config_set_out_pins(&c, cfg->dio_gpio.pin, 1);
	sm_config_set_in_pins(&c, cfg->dio_gpio.pin);
	sm_config_set_set_pins(&c, cfg->dio_gpio.pin, 1);

	sm_config_set_out_shift(&c, true /*right*/, true /*autopull*/, 32);
	sm_config_set_in_shift(&c, true /*right*/, false /*autopush*/, 32);

	/* PIO does 6 cycles per SWD bit. PIO clock = SWD freq * 6. */
	uint32_t sys_hz = clock_get_hz(clk_sys);
	float pio_hz = (float)d->clock_hz * 6.0f;
	float div = (float)sys_hz / pio_hz;
	if (div < 1.0f) {
		div = 1.0f;
	} else if (div > 65535.0f) {
		div = 65535.0f;
	}
	sm_config_set_clkdiv(&c, div);
	LOG_INF("sm_configure: swd=%u Hz sys=%u Hz div=%d.%03d",
		(unsigned)d->clock_hz, (unsigned)sys_hz,
		(int)div, (int)((div - (int)div) * 1000.0f));

	pio_gpio_init(pio, cfg->clk_gpio.pin);
	pio_gpio_init(pio, cfg->dio_gpio.pin);

	pio_sm_set_consecutive_pindirs(pio, d->sm, cfg->clk_gpio.pin, 1, true);

	pio_sm_init(pio, d->sm, d->sm_offset, &c);
	pio_sm_clear_fifos(pio, d->sm);
	pio_sm_set_enabled(pio, d->sm, true);
}

/* ----------------------------------------------------------------------
 * swdp_api implementation
 * ---------------------------------------------------------------------- */

static int api_output_sequence(const struct device *dev, uint32_t count,
			       const uint8_t *data)
{
	uint32_t bits_left = count;
	uint32_t byte_pos = 0;
	uint32_t chunks_sent = 0;
	uint32_t first_word = 0;

	while (bits_left) {
		uint8_t chunk = (bits_left > 32u) ? 32u : (uint8_t)bits_left;
		uint32_t word = 0;

		for (uint8_t i = 0; i < chunk; i++) {
			uint8_t bit = (data[byte_pos + (i >> 3)] >> (i & 7u)) & 1u;
			word |= (uint32_t)bit << i;
		}
		if (chunks_sent == 0) {
			first_word = word;
		}
		swd_write_n(dev, word, chunk);
		bits_left -= chunk;
		byte_pos += (chunk + 7u) / 8u;
		chunks_sent++;
	}
	LOG_INF("seq: count=%u chunks=%u first=0x%08x data[0]=0x%02x",
		(unsigned)count, (unsigned)chunks_sent,
		(unsigned)first_word, (unsigned)data[0]);
	return 0;
}

static int api_input_sequence(const struct device *dev, uint32_t count,
			      uint8_t *data)
{
	const uint32_t total_bytes = (count + 7u) / 8u;

	for (uint32_t i = 0; i < total_bytes; i++) {
		data[i] = 0;
	}

	uint32_t bits_left = count;
	uint32_t bit_pos = 0;

	while (bits_left) {
		uint8_t chunk = (bits_left > 32u) ? 32u : (uint8_t)bits_left;
		uint32_t v = swd_read_n(dev, chunk);

		for (uint8_t i = 0; i < chunk; i++) {
			uint8_t bit = (v >> i) & 1u;

			if (bit) {
				data[(bit_pos + i) >> 3] |=
					1u << ((bit_pos + i) & 7u);
			}
		}
		bits_left -= chunk;
		bit_pos += chunk;
	}
	return 0;
}

static inline uint8_t parity32(uint32_t v)
{
	return (uint8_t)(__builtin_popcount(v) & 1u);
}

static inline uint8_t parity4(uint8_t v)
{
	return (uint8_t)(__builtin_popcount(v & 0x0Fu) & 1u);
}

static atomic_t s_xfer_count;

static int api_transfer(const struct device *dev, uint8_t request,
			uint32_t *data, uint8_t idle_cycles, uint8_t *response)
{
	struct swdp_pio_data *d = dev->data;
	const uint8_t turnaround = d->turnaround;
	const bool is_read = (request & SWDP_REQUEST_RnW) != 0;

	/* Build packet request byte: start, APnDP, RnW, A2, A3, parity, stop,
	 * park — LSB first. */
	uint8_t header = 0
		| (1u << 0)
		| ((request & 0x0Fu) << 1)
		| ((uint8_t)parity4(request) << 5)
		| (0u << 6)
		| (1u << 7);

	/* Parity-error retry loop (8 attempts) — kept from probe.c. */
	uint8_t ack;

	for (int attempt = 0; attempt < 8; attempt++) {
		swd_write_n(dev, header, 8);
		swd_clock_idle_n(dev, turnaround);

		ack = (uint8_t)swd_read_n(dev, 3);

		if (atomic_inc(&s_xfer_count) < 8) {
			LOG_INF("xfer req=0x%02x hdr=0x%02x ack=0x%x",
				request, header, ack);
		}

		if (ack == 0x1u) {
			if (is_read) {
				uint32_t v = swd_read_n(dev, 32);
				uint8_t target_par = (uint8_t)swd_read_n(dev, 1);

				swd_clock_idle_n(dev, turnaround);
				if (data) {
					*data = v;
				}
				if (parity32(v) != target_par) {
					ack = SWDP_ACK_OK | SWDP_TRANSFER_ERROR;
					continue;	/* retry */
				}
			} else {
				swd_clock_idle_n(dev, turnaround);
				swd_write_n(dev, *data, 32);
				swd_write_n(dev, parity32(*data), 1);
			}

			/* Trailing idle clocks — passed in via the swdp_api
			 * call (different from probe.c where it was a global
			 * pre-configured value). */
			if (idle_cycles) {
				uint16_t left = idle_cycles;

				while (left) {
					uint8_t chunk = (left > 32u) ? 32u :
						(uint8_t)left;
					swd_write_n(dev, 0u, chunk);
					left -= chunk;
				}
			}
			if (response) {
				*response = SWDP_ACK_OK;
			}
			return 0;
		}

		/* WAIT / FAULT path — emit the data phase if configured. */
		if (d->data_phase) {
			if (is_read) {
				(void)swd_read_n(dev, 32);
				(void)swd_read_n(dev, 1);
				swd_clock_idle_n(dev, turnaround);
			} else {
				swd_clock_idle_n(dev, turnaround);
				swd_write_n(dev, 0u, 32);
				swd_write_n(dev, 0u, 1);
			}
		} else {
			swd_clock_idle_n(dev, turnaround);
		}

		if (response) {
			*response = ack;
		}
		return 0;
	}

	if (response) {
		*response = ack;
	}
	return 0;
}

static int api_set_pins(const struct device *dev, uint8_t pins, uint8_t value)
{
	const struct swdp_pio_config *cfg = dev->config;
	struct swdp_pio_data *d = dev->data;
	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);

	/* If SWCLK or SWDIO is selected, yield from PIO back to SIO so we can
	 * drive the line directly. Restore PIO afterwards. */
	bool yield_pio = (pins & (BIT(SWDP_SWCLK_PIN) | BIT(SWDP_SWDIO_PIN)));

	if (yield_pio) {
		pio_sm_set_enabled(pio, d->sm, false);
		gpio_pin_configure_dt(&cfg->clk_gpio, GPIO_INPUT);
		gpio_pin_configure_dt(&cfg->dio_gpio, GPIO_INPUT);
	}

	if (pins & BIT(SWDP_SWCLK_PIN)) {
		gpio_pin_configure_dt(&cfg->clk_gpio, GPIO_OUTPUT);
		gpio_pin_set_dt(&cfg->clk_gpio,
				(value & BIT(SWDP_SWCLK_PIN)) ? 1 : 0);
	}
	if (pins & BIT(SWDP_SWDIO_PIN)) {
		gpio_pin_configure_dt(&cfg->dio_gpio, GPIO_OUTPUT);
		gpio_pin_set_dt(&cfg->dio_gpio,
				(value & BIT(SWDP_SWDIO_PIN)) ? 1 : 0);
	}
	if (cfg->reset_present && (pins & BIT(SWDP_nRESET_PIN))) {
		/* Open-drain — drive low to assert; release to deassert. */
		if (value & BIT(SWDP_nRESET_PIN)) {
			gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_INPUT);
		} else {
			gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT);
			gpio_pin_set_dt(&cfg->reset_gpio, 0);
		}
	}

	if (yield_pio) {
		sm_configure(dev);
	}
	return 0;
}

static int api_get_pins(const struct device *dev, uint8_t *state)
{
	const struct swdp_pio_config *cfg = dev->config;
	uint8_t v = 0;

	if (gpio_pin_get_dt(&cfg->clk_gpio)) {
		v |= BIT(SWDP_SWCLK_PIN);
	}
	if (gpio_pin_get_dt(&cfg->dio_gpio)) {
		v |= BIT(SWDP_SWDIO_PIN);
	}
	if (cfg->reset_present && gpio_pin_get_dt(&cfg->reset_gpio)) {
		v |= BIT(SWDP_nRESET_PIN);
	}
	*state = v;
	return 0;
}

static int api_set_clock(const struct device *dev, uint32_t clock)
{
	struct swdp_pio_data *d = dev->data;

	if (clock == 0) {
		clock = 1000000u;
	}
	/* RTOS difference vs bare-metal pico-sdk: in the upstream pico-sdk
	 * firmware the dispatcher reconfigures the PIO only at probe_init.
	 * Zephyr's cmsis_dap.c routes through swdp_set_clock at Connect time,
	 * which we used to honor unconditionally — calling sm_configure a
	 * second time right after port_on already did. That redundant
	 * pio_sm_init + clear_fifos churns the SM mid-bring-up and may leave
	 * the line in a state the target doesn't expect by the time the first
	 * SWJ_Sequence arrives. Skip the reconfigure when the divider would
	 * not actually change. */
	if (d->clock_hz == clock) {
		return 0;
	}
	d->clock_hz = clock;
	if (d->enabled) {
		sm_configure(dev);
	}
	return 0;
}

static int api_configure(const struct device *dev, uint8_t turnaround,
			 bool data_phase)
{
	struct swdp_pio_data *d = dev->data;

	d->turnaround = (turnaround & 0x3u) + 1u;
	d->data_phase = data_phase ? 1u : 0u;
	LOG_INF("configure: turnaround=%u data_phase=%u",
		(unsigned)d->turnaround, (unsigned)d->data_phase);
	return 0;
}

static int api_port_on(const struct device *dev)
{
	struct swdp_pio_data *d = dev->data;

	sm_configure(dev);
	drain_rx(pio_rpi_pico_get_pio(((const struct swdp_pio_config *)
				       dev->config)->piodev),
		 d->sm);
	d->enabled = true;
	return 0;
}

static int api_port_off(const struct device *dev)
{
	const struct swdp_pio_config *cfg = dev->config;
	struct swdp_pio_data *d = dev->data;
	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);

	pio_sm_set_enabled(pio, d->sm, false);
	gpio_pin_configure_dt(&cfg->clk_gpio, GPIO_INPUT);
	gpio_pin_configure_dt(&cfg->dio_gpio, GPIO_INPUT);
	d->enabled = false;
	return 0;
}

/* Note: struct swdp_api, not <class>_driver_api — the swdp subsystem predates
 * the DEVICE_API() macro and uses the plain "<class>_api" naming. Match what
 * drivers/dp/swdp_bitbang.c does. */
static struct swdp_api swdp_pio_api = {
	.swdp_output_sequence = api_output_sequence,
	.swdp_input_sequence = api_input_sequence,
	.swdp_transfer = api_transfer,
	.swdp_set_pins = api_set_pins,
	.swdp_get_pins = api_get_pins,
	.swdp_set_clock = api_set_clock,
	.swdp_configure = api_configure,
	.swdp_port_on = api_port_on,
	.swdp_port_off = api_port_off,
};

/* ----------------------------------------------------------------------
 * Init
 * ---------------------------------------------------------------------- */

static int swdp_pio_init(const struct device *dev)
{
	const struct swdp_pio_config *cfg = dev->config;
	struct swdp_pio_data *d = dev->data;

	if (!device_is_ready(cfg->piodev)) {
		LOG_ERR("Parent PIO device not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&cfg->clk_gpio) ||
	    !gpio_is_ready_dt(&cfg->dio_gpio)) {
		LOG_ERR("SWCLK/SWDIO GPIOs not ready");
		return -ENODEV;
	}

	int ret = pio_rpi_pico_allocate_sm(cfg->piodev, &d->sm);

	if (ret) {
		LOG_ERR("No free PIO state machines (%d)", ret);
		return ret;
	}

	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);

	if (!pio_can_add_program(pio, &probe_swd_program)) {
		LOG_ERR("PIO program won't fit");
		return -EBUSY;
	}
	d->sm_offset = pio_add_program(pio, &probe_swd_program);

	d->clock_hz = 1000000u;
	d->turnaround = 1;
	d->data_phase = 0;
	d->enabled = false;

	sm_configure(dev);
	d->enabled = true;

	LOG_INF("SWDP-PIO ready on %s sm=%u offset=%u",
		cfg->piodev->name, (unsigned)d->sm, (unsigned)d->sm_offset);
	return 0;
}

#define SWDP_PIO_INIT(inst)							\
	static const struct swdp_pio_config swdp_pio_cfg_##inst = {		\
		.piodev = DEVICE_DT_GET(DT_INST_PARENT(inst)),			\
		.clk_gpio = GPIO_DT_SPEC_INST_GET(inst, clk_gpios),		\
		.dio_gpio = GPIO_DT_SPEC_INST_GET(inst, dio_gpios),		\
		.reset_gpio =							\
			GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),	\
		.reset_present = DT_INST_NODE_HAS_PROP(inst, reset_gpios),	\
	};									\
	static struct swdp_pio_data swdp_pio_data_##inst;			\
	DEVICE_DT_INST_DEFINE(inst, swdp_pio_init, NULL,			\
			      &swdp_pio_data_##inst, &swdp_pio_cfg_##inst,	\
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	\
			      &swdp_pio_api);

DT_INST_FOREACH_STATUS_OKAY(SWDP_PIO_INIT)
