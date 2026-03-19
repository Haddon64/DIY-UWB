/*
 * DIY-UWB — SS-TWR with bidirectional COBS serial bridge
 * Zephyr 2.7.1 / nRF52833 / DWM3001CDK
 *
 * Both units range continuously at ~50 Hz.
 * COBS packets received on UART are embedded in the next UWB frame.
 * UWB payloads received from the peer are output as COBS on UART.
 *
 * COBS output frame: [type:1][range_cm:4 LE i32][plen:1][payload:0-64]
 *   type 0x01 = initiator report, 0x02 = responder report
 *
 * Role (compile-time):
 *   -DUWB_INITIATOR  → drives ranging (sends polls)
 *   (default)        → responder (listens, replies)
 */

#include <kernel.h>
#include <device.h>
#include <drivers/gpio.h>
#include <drivers/spi.h>
#include <drivers/uart.h>
#include <string.h>

#include "deca_device_api.h"
#include "deca_interface.h"

/* ── Platform shims ─────────────────────────────────────────────────────────*/
void deca_usleep(unsigned long time_us) { k_usleep((int32_t)time_us); }
void deca_sleep(unsigned int time_ms)   { k_msleep((int32_t)time_ms); }
decaIrqStatus_t decamutexon(void)       { return (decaIrqStatus_t)irq_lock(); }
void decamutexoff(decaIrqStatus_t s)    { irq_unlock((unsigned int)s); }

/* ── DT nodes ───────────────────────────────────────────────────────────────*/
#define SPI_NODE  DT_NODELABEL(spi3)
#define UART_NODE DT_NODELABEL(uart0)

static const struct device *gpio0_dev;
static const struct device *gpio1_dev;

#define IRQ_PIN     2U   /* P1.02 */
#define RST_PIN    25U   /* P0.25 */
#define WAKEUP_PIN 19U   /* P1.19 */

/* ── LEDs (active-low) ─────────────────────────────────────────────────────*/
#define LED1_PIN    4U   /* D9:  TX */
#define LED2_PIN    5U   /* D10: RX */
#define LED3_PIN   22U   /* D11: OK */
#define LED4_PIN   14U   /* D12: ERR */

static void led_on(uint8_t pin)  { gpio_pin_set(gpio0_dev, pin, 1); }
static void led_off(uint8_t pin) { gpio_pin_set(gpio0_dev, pin, 0); }

/* ── SPI ────────────────────────────────────────────────────────────────────*/
static const struct device *spi_dev;
static const struct device *cs_gpio_dev;
#define CS_PIN 6U

static void cs_assert(void)   { gpio_pin_set_raw(cs_gpio_dev, CS_PIN, 0); }
static void cs_deassert(void) { gpio_pin_set_raw(cs_gpio_dev, CS_PIN, 1); }

#define SPI_OP (SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER)

static struct spi_config spi_cfg_slow = {
	.frequency = 4000000U, .operation = SPI_OP, .slave = 0, .cs = NULL,
};

static struct spi_config *spi_cfg = &spi_cfg_slow;

#define SPI_SCRATCH_LEN 512U
static uint8_t spi_tx_scratch[SPI_SCRATCH_LEN];
static uint8_t spi_rx_scratch[SPI_SCRATCH_LEN];

static int uwb_readfromspi(uint16_t hlen, uint8_t *hbuf,
			   uint16_t rlen, uint8_t *rbuf)
{
	uint16_t total = hlen + rlen;
	if (!hbuf || !rbuf || total > SPI_SCRATCH_LEN) { return -1; }

	memcpy(spi_tx_scratch, hbuf, hlen);
	memset(spi_tx_scratch + hlen, 0x00, rlen);

	const struct spi_buf tx_b = { .buf = spi_tx_scratch, .len = total };
	const struct spi_buf rx_b = { .buf = spi_rx_scratch, .len = total };
	const struct spi_buf_set tx_set = { .buffers = &tx_b, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx_b, .count = 1 };

	cs_assert();
	int ret = spi_transceive(spi_dev, spi_cfg, &tx_set, &rx_set);
	cs_deassert();

	if (ret == 0) { memcpy(rbuf, spi_rx_scratch + hlen, rlen); }
	return ret;
}

static int uwb_writetospi(uint16_t hlen, const uint8_t *hbuf,
			  uint16_t blen, const uint8_t *bbuf)
{
	uint16_t total = hlen + blen;
	if (!hbuf || total > SPI_SCRATCH_LEN) { return -1; }

	memcpy(spi_tx_scratch, hbuf, hlen);
	if (blen > 0 && bbuf) { memcpy(spi_tx_scratch + hlen, bbuf, blen); }

	const struct spi_buf tx_b = { .buf = spi_tx_scratch, .len = total };
	const struct spi_buf rx_b = { .buf = spi_rx_scratch, .len = total };
	const struct spi_buf_set tx_set = { .buffers = &tx_b, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx_b, .count = 1 };

	cs_assert();
	int ret = spi_transceive(spi_dev, spi_cfg, &tx_set, &rx_set);
	cs_deassert();
	return ret;
}

static int uwb_writetospi_crc(uint16_t hlen, const uint8_t *hbuf,
			      uint16_t blen, const uint8_t *bbuf, uint8_t crc8)
{
	ARG_UNUSED(crc8);
	return uwb_writetospi(hlen, hbuf, blen, bbuf);
}

static void uwb_spi_setfastrate(void) { spi_cfg = &spi_cfg_slow; }
static void uwb_spi_setslowrate(void) { spi_cfg = &spi_cfg_slow; }

/* ── DW3110 control ─────────────────────────────────────────────────────────*/
static void uwb_wakeup_fn(void)
{
	gpio_pin_set(gpio1_dev, WAKEUP_PIN, 1);
	k_usleep(500);
	gpio_pin_set(gpio1_dev, WAKEUP_PIN, 0);
	k_msleep(2);
}

static int waitforsysstatus(uint32_t *lo_result, uint32_t *hi_result,
			    uint32_t lo_mask, uint32_t hi_mask)
{
	uint32_t lo, hi = 0U;
	uint32_t cnt = 0U;
	do {
		lo = dwt_readsysstatuslo();
		if (hi_mask) { hi = dwt_readsysstatushi(); }
		if (++cnt > 500000U) {
			if (lo_result) { *lo_result = lo; }
			if (hi_result) { *hi_result = hi; }
			return -1;
		}
	} while (!(lo & lo_mask) && !(hi & hi_mask));
	if (lo_result) { *lo_result = lo; }
	if (hi_result) { *hi_result = hi; }
	return 0;
}

/* ── COBS encode/decode ─────────────────────────────────────────────────────*/
/*
 * COBS encoding: replaces all 0x00 bytes so the frame can be delimited by 0x00.
 * Output: [encoded bytes...] 0x00
 */
static uint16_t cobs_encode(const uint8_t *in, uint16_t in_len,
			    uint8_t *out, uint16_t out_max)
{
	uint16_t out_idx = 1;  /* leave room for first code byte */
	uint16_t code_idx = 0;
	uint8_t code = 1;

	for (uint16_t i = 0; i < in_len; i++) {
		if (out_idx >= out_max - 1U) { return 0; }  /* overflow */
		if (in[i] == 0x00) {
			out[code_idx] = code;
			code_idx = out_idx++;
			code = 1;
		} else {
			out[out_idx++] = in[i];
			code++;
			if (code == 0xFF) {
				out[code_idx] = code;
				code_idx = out_idx++;
				code = 1;
			}
		}
	}
	out[code_idx] = code;
	if (out_idx >= out_max) { return 0; }
	out[out_idx++] = 0x00;  /* frame delimiter */
	return out_idx;
}

static uint16_t cobs_decode(const uint8_t *in, uint16_t in_len,
			    uint8_t *out, uint16_t out_max)
{
	uint16_t out_idx = 0;
	uint16_t i = 0;

	while (i < in_len) {
		uint8_t code = in[i++];
		if (code == 0) { break; }  /* end of frame */
		for (uint8_t j = 1; j < code; j++) {
			if (i >= in_len || out_idx >= out_max) { return 0; }
			out[out_idx++] = in[i++];
		}
		if (code < 0xFF && i < in_len) {
			if (out_idx >= out_max) { return 0; }
			out[out_idx++] = 0x00;
		}
	}
	/* Remove trailing 0x00 added by the decode if present */
	if (out_idx > 0 && out[out_idx - 1] == 0x00) { out_idx--; }
	return out_idx;
}

/* ── UART + COBS framing ───────────────────────────────────────────────────*/
#define PAYLOAD_MAX  100U
#define COBS_BUF_MAX (PAYLOAD_MAX + 10U)

static const struct device *uart_dev;

/* RX: message queue holds decoded COBS payloads (up to 4 pending) */
struct uart_msg {
	uint8_t data[PAYLOAD_MAX];
	uint8_t len;
};

K_MSGQ_DEFINE(uart_rx_msgq, sizeof(struct uart_msg), 2, 4);

/* ISR accumulates COBS bytes, decodes on 0x00, enqueues */
static uint8_t  uart_rx_cobs[COBS_BUF_MAX];
static uint16_t uart_rx_cobs_len;

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		uint8_t c;
		if (uart_fifo_read(dev, &c, 1) != 1) { break; }

		if (c == 0x00) {
			if (uart_rx_cobs_len > 0) {
				struct uart_msg msg;
				uint16_t dlen = cobs_decode(uart_rx_cobs,
							    uart_rx_cobs_len,
							    msg.data,
							    PAYLOAD_MAX);
				if (dlen > 0) {
					msg.len = (uint8_t)dlen;
					k_msgq_put(&uart_rx_msgq, &msg, K_NO_WAIT);
				}
			}
			uart_rx_cobs_len = 0;
		} else if (uart_rx_cobs_len < COBS_BUF_MAX) {
			uart_rx_cobs[uart_rx_cobs_len++] = c;
		}
	}
}

/* Grab the next pending UART payload (non-blocking). Returns len or 0. */
static uint8_t uart_rx_get(uint8_t *buf)
{
	struct uart_msg msg;
	if (k_msgq_get(&uart_rx_msgq, &msg, K_NO_WAIT) == 0) {
		memcpy(buf, msg.data, msg.len);
		return msg.len;
	}
	return 0;
}

/* TX: send COBS-encoded output frame */
static void uart_tx_bytes(const uint8_t *data, uint16_t len)
{
	for (uint16_t i = 0; i < len; i++) {
		uart_poll_out(uart_dev, data[i]);
	}
}

/*
 * Build and send a COBS output frame:
 * [type:1][range_cm:4 LE i32][plen:1][payload:0-64]
 */
static void cobs_output(uint8_t type, int32_t range_cm,
			const uint8_t *payload, uint8_t plen)
{
	uint8_t raw[6 + PAYLOAD_MAX];
	raw[0] = type;
	raw[1] = (uint8_t)(range_cm);
	raw[2] = (uint8_t)(range_cm >> 8);
	raw[3] = (uint8_t)(range_cm >> 16);
	raw[4] = (uint8_t)(range_cm >> 24);
	raw[5] = plen;
	if (plen > 0 && payload) {
		memcpy(&raw[6], payload, plen);
	}
	uint16_t raw_len = 6U + plen;

	uint8_t cobs_buf[COBS_BUF_MAX + 10];
	uint16_t enc_len = cobs_encode(raw, raw_len, cobs_buf, sizeof(cobs_buf));
	if (enc_len > 0) {
		uart_tx_bytes(cobs_buf, enc_len);
	}
}

/* ── UWB config ─────────────────────────────────────────────────────────────*/
static dwt_config_t uwb_config = {
	.chan            = 5,
	.txPreambLength  = DWT_PLEN_128,
	.rxPAC           = DWT_PAC8,
	.txCode          = 9,
	.rxCode          = 9,
	.sfdType         = DWT_SFD_DW_8,
	.dataRate        = DWT_BR_6M8,
	.phrMode         = DWT_PHRMODE_STD,
	.phrRate         = DWT_PHRRATE_STD,
	.sfdTO           = (128 + 1 + 8 - 8),
	.stsMode         = DWT_STS_MODE_OFF,
	.stsLength       = DWT_STS_LEN_64,
	.pdoaMode        = DWT_PDOA_M0,
};

/* ── Frame layout ───────────────────────────────────────────────────────────
 * All frames: [FC:2][SN:1][PAN:2][DST:2][SRC:2][FUNC:1] = 10 bytes header
 * Poll:     [HDR:10][plen:1][payload:0-64]
 * Response: [HDR:10][reply_time:5][plen:1][payload:0-64]
 * FCS (2B) appended by hardware.
 */
#define HDR_LEN           10U
#define SN_IDX             2U
#define FUNC_CODE_IDX      9U
#define FUNC_POLL         0x21U
#define FUNC_RESP         0x10U
#define FCS_LEN            2U

#define POLL_PLEN_IDX     10U
#define POLL_PAYLOAD_IDX  11U

#define RESP_REPLY_IDX    10U
#define RESP_PLEN_IDX     15U
#define RESP_PAYLOAD_IDX  16U
#define RESP_HDR_LEN      16U  /* minimum response length (no payload) */

/* Timing */
#define CPU_PROC_UUS            5000U
#define POLL_RX_TO_RESP_DLY_UUS (6000U + CPU_PROC_UUS)
#define RESP_RX_TIMEOUT_UUS     20000U
#define UUS_TO_DWT              65536UL

#define TX_ANT_DLY  16390U
#define RX_ANT_DLY  16390U

static uint8_t frame_sn;
static int32_t last_range_cm = -1;  /* shared between roles */

/* ── Timestamp helpers ──────────────────────────────────────────────────────*/
static uint64_t get_tx_ts(void)
{
	uint8_t b[5];
	dwt_readtxtimestamp(b);
	return (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) |
	       ((uint64_t)b[3] << 24) | ((uint64_t)b[4] << 32);
}

static uint64_t get_rx_ts(void)
{
	uint8_t b[5];
	dwt_readrxtimestamp(b, DWT_COMPAT_NONE);
	return (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) |
	       ((uint64_t)b[3] << 24) | ((uint64_t)b[4] << 32);
}

static void pack_ts5(uint8_t *buf, uint64_t ts)
{
	buf[0] = (uint8_t)(ts);       buf[1] = (uint8_t)(ts >> 8);
	buf[2] = (uint8_t)(ts >> 16); buf[3] = (uint8_t)(ts >> 24);
	buf[4] = (uint8_t)(ts >> 32);
}

static uint64_t unpack_ts5(const uint8_t *buf)
{
	return (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) |
	       ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24) |
	       ((uint64_t)buf[4] << 32);
}

/* ── Initiator ──────────────────────────────────────────────────────────────*/
#ifdef UWB_INITIATOR

static void run_initiator(void)
{
	uint8_t rx_buf[HDR_LEN + 1U + PAYLOAD_MAX + 16U];
	uint32_t status;
	uint8_t tx_poll[HDR_LEN + 1U + PAYLOAD_MAX];

	static const uint8_t poll_hdr[] =
		{ 0x41, 0x88, 0, 0xCA, 0xDE, 'W', 'A', 'V', 'E', FUNC_POLL };
	memcpy(tx_poll, poll_hdr, sizeof(poll_hdr));

	/* Pending payload: held until successfully transmitted over UWB */
	uint8_t pending_payload[PAYLOAD_MAX];
	uint8_t pending_len = 0;
	bool    pending_sent = false;  /* true once UWB TX succeeded */

	while (1) {
		/* If previous payload was sent, grab next from queue */
		if (pending_len == 0 || pending_sent) {
			uint8_t new_len = uart_rx_get(pending_payload);
			if (new_len > 0) {
				pending_len = new_len;
				pending_sent = false;
			} else if (pending_sent) {
				pending_len = 0;  /* Nothing new, clear old */
			}
		}
		uint8_t plen = pending_len;
		uint8_t *payload = pending_payload;

		/* Build poll frame */
		tx_poll[SN_IDX]        = frame_sn;
		tx_poll[POLL_PLEN_IDX] = plen;
		if (plen > 0) { memcpy(&tx_poll[POLL_PAYLOAD_IDX], payload, plen); }
		uint16_t poll_len = (uint16_t)(HDR_LEN + 1U + plen);

		/* TX poll */
		dwt_forcetrxoff();
		dwt_writetxdata(poll_len, tx_poll, 0);
		dwt_writetxfctrl(poll_len + FCS_LEN, 0, 1);
		dwt_setrxaftertxdelay(0);
		dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);

		led_on(LED1_PIN);
		if (dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED)
		    != DWT_SUCCESS) {
			led_off(LED1_PIN);
			k_msleep(5);
			continue;
		}

		/* Wait TX done */
		if (waitforsysstatus(&status, NULL, DWT_INT_TXFRS_BIT_MASK, 0U) != 0) {
			led_off(LED1_PIN); dwt_forcetrxoff(); k_msleep(5); continue;
		}
		uint64_t poll_tx_ts = get_tx_ts();
		dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK);
		led_off(LED1_PIN);
		frame_sn++;

		/* Wait for response */
		if (waitforsysstatus(&status, NULL,
				DWT_INT_RXFCG_BIT_MASK |
				SYS_STATUS_ALL_RX_TO |
				SYS_STATUS_ALL_RX_ERR, 0U) != 0 ||
		    !(status & DWT_INT_RXFCG_BIT_MASK)) {
			dwt_writesysstatuslo(0xFFFFFFFFUL);
			dwt_forcetrxoff();
			k_msleep(5);
			continue;
		}
		uint64_t resp_rx_ts = get_rx_ts();
		dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK);

		uint8_t rng_bit;
		uint16_t flen = dwt_getframelength(&rng_bit);
		if (flen > sizeof(rx_buf) || flen < RESP_HDR_LEN) {
			dwt_forcetrxoff(); k_msleep(5); continue;
		}
		dwt_readrxdata(rx_buf, flen, 0);
		if (rx_buf[FUNC_CODE_IDX] != FUNC_RESP) {
			dwt_forcetrxoff(); k_msleep(5); continue;
		}

		led_on(LED2_PIN);

		/* Compute range */
		uint64_t reply_time = unpack_ts5(&rx_buf[RESP_REPLY_IDX]);
		if (reply_time > 0) {
			int64_t rtd_init = (int64_t)(resp_rx_ts - poll_tx_ts);
			int64_t rtd_resp = (int64_t)(reply_time & 0xFFFFFFFFFFULL);
			int64_t tof_ticks = (rtd_init - rtd_resp) / 2;
			double tof_sec = (double)tof_ticks * (1.0 / 499.2e6 / 128.0);
			double dist_m = tof_sec * 299702547.0;
			last_range_cm = (int32_t)(dist_m * 100.0);
		}

		/* Payload was successfully carried in the poll → mark as sent */
		if (plen > 0) {
			pending_sent = true;
		}

		/* Extract responder's payload from response */
		uint8_t resp_plen = rx_buf[RESP_PLEN_IDX];
		if (resp_plen > PAYLOAD_MAX) { resp_plen = 0; }

		/* Output COBS frame if responder sent data */
		if (resp_plen > 0) {
			cobs_output(0x01, last_range_cm,
				    &rx_buf[RESP_PAYLOAD_IDX], resp_plen);
		}

		led_off(LED2_PIN);
		led_on(LED3_PIN); k_usleep(200); led_off(LED3_PIN);

		k_msleep(5);  /* ~50 Hz with exchange overhead */
	}
}

/* ── Responder ──────────────────────────────────────────────────────────────*/
#else

static void run_responder(void)
{
	uint8_t rx_buf[HDR_LEN + 1U + PAYLOAD_MAX + 16U];
	uint32_t status;
	uint8_t tx_resp[RESP_HDR_LEN + PAYLOAD_MAX];

	static const uint8_t resp_hdr[] =
		{ 0x41, 0x88, 0, 0xCA, 0xDE, 'V', 'E', 'W', 'A', FUNC_RESP };
	memcpy(tx_resp, resp_hdr, sizeof(resp_hdr));

	dwt_setrxtimeout(0U);
	dwt_setpreambledetecttimeout(0U);

	/* Pending payload: held until successfully sent in a response */
	uint8_t pending_payload[PAYLOAD_MAX];
	uint8_t pending_len = 0;
	bool    pending_sent = false;

	while (1) {
		/* If previous payload was sent, grab next from queue */
		if (pending_len == 0 || pending_sent) {
			uint8_t new_len = uart_rx_get(pending_payload);
			if (new_len > 0) {
				pending_len = new_len;
				pending_sent = false;
			} else if (pending_sent) {
				pending_len = 0;
			}
		}

		dwt_rxenable(DWT_START_RX_IMMEDIATE);

		waitforsysstatus(&status, NULL,
				 DWT_INT_RXFCG_BIT_MASK |
				 SYS_STATUS_ALL_RX_TO |
				 SYS_STATUS_ALL_RX_ERR, 0U);

		if (!(status & DWT_INT_RXFCG_BIT_MASK)) {
			dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
			continue;
		}

		uint64_t poll_rx_ts = get_rx_ts();
		led_on(LED2_PIN);
		dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK);

		uint8_t rng_bit;
		uint16_t flen = dwt_getframelength(&rng_bit);
		if (flen > sizeof(rx_buf)) { led_off(LED2_PIN); continue; }
		dwt_readrxdata(rx_buf, flen, 0);
		if (rx_buf[FUNC_CODE_IDX] != FUNC_POLL) { led_off(LED2_PIN); continue; }

		/* Extract initiator's payload */
		uint8_t poll_plen = rx_buf[POLL_PLEN_IDX];
		if (poll_plen > PAYLOAD_MAX) { poll_plen = 0; }

		uint8_t resp_plen = pending_len;
		uint8_t *resp_payload = pending_payload;

		/* Build response: [HDR][reply_time:5][plen:1][payload:0-N] */
		tx_resp[SN_IDX] = rx_buf[SN_IDX];

		/* Delayed TX for precise reply time */
		uint32_t resp_tx_time =
			(uint32_t)((poll_rx_ts +
				    (uint64_t)POLL_RX_TO_RESP_DLY_UUS * UUS_TO_DWT) >> 8);
		dwt_setdelayedtrxtime(resp_tx_time);

		uint64_t resp_tx_ts =
			(((uint64_t)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;
		uint64_t reply_time = resp_tx_ts - poll_rx_ts;
		pack_ts5(&tx_resp[RESP_REPLY_IDX], reply_time);

		tx_resp[RESP_PLEN_IDX] = resp_plen;
		if (resp_plen > 0) {
			memcpy(&tx_resp[RESP_PAYLOAD_IDX], resp_payload, resp_plen);
		}
		uint16_t resp_len = (uint16_t)(RESP_HDR_LEN + resp_plen);

		dwt_forcetrxoff();
		dwt_writetxdata(resp_len, tx_resp, 0);
		dwt_writetxfctrl(resp_len + FCS_LEN, 0, 1);

		led_on(LED1_PIN);
		if (dwt_starttx(DWT_START_TX_DELAYED) != DWT_SUCCESS) {
			led_off(LED1_PIN); led_off(LED2_PIN);
			dwt_forcetrxoff();
			continue;
		}

		if (waitforsysstatus(&status, NULL, DWT_INT_TXFRS_BIT_MASK, 0U) != 0) {
			led_off(LED1_PIN); led_off(LED2_PIN);
			dwt_forcetrxoff();
			continue;
		}
		dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK);
		led_off(LED1_PIN);

		/* Response sent successfully → mark payload as sent */
		if (resp_plen > 0) {
			pending_sent = true;
		}

		/* Range not available on responder (SS-TWR computed on initiator) */
		last_range_cm = -1;

		/* Output COBS frame if initiator sent data */
		if (poll_plen > 0) {
			cobs_output(0x02, last_range_cm,
				    &rx_buf[POLL_PAYLOAD_IDX], poll_plen);
		}

		led_off(LED2_PIN);
		led_on(LED3_PIN); k_usleep(200); led_off(LED3_PIN);
	}
}
#endif /* UWB_INITIATOR */

/* ── main ───────────────────────────────────────────────────────────────────*/
void main(void)
{
	/* GPIO init */
	gpio0_dev = device_get_binding("GPIO_0");
	gpio1_dev = device_get_binding("GPIO_1");
	if (!gpio0_dev || !gpio1_dev) { return; }

	gpio_pin_configure(gpio0_dev, RST_PIN,    GPIO_OUTPUT_HIGH);
	gpio_pin_configure(gpio1_dev, WAKEUP_PIN, GPIO_OUTPUT_LOW);
	gpio_pin_configure(gpio1_dev, IRQ_PIN,    GPIO_INPUT);

	gpio_pin_configure(gpio0_dev, LED1_PIN, GPIO_OUTPUT_INACTIVE | GPIO_ACTIVE_LOW);
	gpio_pin_configure(gpio0_dev, LED2_PIN, GPIO_OUTPUT_INACTIVE | GPIO_ACTIVE_LOW);
	gpio_pin_configure(gpio0_dev, LED3_PIN, GPIO_OUTPUT_INACTIVE | GPIO_ACTIVE_LOW);
	gpio_pin_configure(gpio0_dev, LED4_PIN, GPIO_OUTPUT_INACTIVE | GPIO_ACTIVE_LOW);

	/* Boot flash */
	led_on(LED1_PIN); led_on(LED2_PIN); led_on(LED3_PIN); led_on(LED4_PIN);
	k_msleep(300);
	led_off(LED1_PIN); led_off(LED2_PIN); led_off(LED3_PIN); led_off(LED4_PIN);

	/* CS GPIO */
	cs_gpio_dev = gpio1_dev;
	gpio_pin_configure(cs_gpio_dev, CS_PIN, GPIO_OUTPUT_HIGH);

	/* SPI init */
	spi_dev = DEVICE_DT_GET(SPI_NODE);
	if (!device_is_ready(spi_dev)) { return; }

	/* UART init */
	uart_dev = DEVICE_DT_GET(UART_NODE);
	if (!device_is_ready(uart_dev)) { return; }
	uart_irq_callback_set(uart_dev, uart_isr);
	uart_irq_rx_enable(uart_dev);

	/* DW3110: wakeup → reset → wait → probe → init → configure */
	cs_assert(); k_msleep(1); cs_deassert(); k_msleep(5);

	gpio_pin_configure(gpio0_dev, RST_PIN, GPIO_OUTPUT_LOW);
	k_msleep(2);
	gpio_pin_configure(gpio0_dev, RST_PIN, GPIO_INPUT);
	k_msleep(50);

	static struct dwt_spi_s dwt_spi = {
		.readfromspi       = uwb_readfromspi,
		.writetospi        = uwb_writetospi,
		.writetospiwithcrc = uwb_writetospi_crc,
		.setfastrate       = uwb_spi_setfastrate,
		.setslowrate       = uwb_spi_setslowrate,
	};
	static struct dwchip_s dw_chip;
	static struct dwt_probe_s probe = {
		.dw = &dw_chip, .spi = &dwt_spi,
		.wakeup_device_with_io = uwb_wakeup_fn,
	};
	extern const struct dwt_driver_s dw3000_driver;
	static const struct dwt_driver_s *driver_list[] = { &dw3000_driver };
	probe.driver_list   = (struct dwt_driver_s **)driver_list;
	probe.dw_driver_num = 1;

	if (dwt_probe(&probe) != DWT_SUCCESS) { led_on(LED4_PIN); return; }

	{ int t = 0; while (!dwt_checkidlerc()) { k_msleep(1); if (++t > 100) { led_on(LED4_PIN); return; } } }

	if (dwt_initialise(DWT_DW_INIT) != DWT_SUCCESS) { led_on(LED4_PIN); return; }
	if (dwt_configure(&uwb_config) != DWT_SUCCESS) { led_on(LED4_PIN); return; }

	dwt_setdwstate(DWT_DW_IDLE);
	k_msleep(1);

	dwt_setrxantennadelay(RX_ANT_DLY);
	dwt_settxantennadelay(TX_ANT_DLY);
	dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

	/* Send a startup COBS frame: type=0xFF (boot), range=-1, plen=4, "BOOT" */
	cobs_output(0xFF, -1, (const uint8_t *)"BOOT", 4);

#ifdef UWB_INITIATOR
	run_initiator();
#else
	run_responder();
#endif
}
