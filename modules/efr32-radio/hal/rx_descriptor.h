/*
 * QEMU-only receive descriptor and FIFO normalization.
 *
 * The vendor image consumes eight-byte BUFC channel-2 records whose first
 * word contains a channel-1 endpoint and whose byte two selects the receive
 * event class.  This header is intentionally a small, hardware-independent
 * parser: the real device's producer and record validity are not established.
 * The caller must therefore compile and enable this path only for the
 * simulator seam (CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_RX_DESCRIPTOR_H_
#define EFR32_RADIO_RX_DESCRIPTOR_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EFR32_RADIO_RX_DESCRIPTOR_BYTES 8U
#define EFR32_RADIO_RX_DESCRIPTOR_ENDPOINT_MASK 0x1FFFU
#define EFR32_RADIO_RX_DESCRIPTOR_CLASS_MASK 0x07U
#define EFR32_RADIO_RX_DESCRIPTOR_RECEIVED_CLASS 6U
#define EFR32_RADIO_RX_DESCRIPTOR_RECEIVED_ALT_CLASS 7U

/* The opt-in QEMU contract uses the same root observed in stock firmware. */
#define EFR32_RADIO_QEMU_RX_DESCRIPTOR_ROOT 0x20001178UL
#define EFR32_RADIO_QEMU_RX_DESCRIPTOR_RING_BYTES 512U
#define EFR32_RADIO_QEMU_RX_DESCRIPTOR_RING_MASK \
	(EFR32_RADIO_QEMU_RX_DESCRIPTOR_RING_BYTES - 1U)

struct efr32_radio_rx_descriptor {
	uint16_t endpoint;
	uint8_t event_class;
	uint8_t metadata;
};

static inline uint16_t efr32_radio_rx_descriptor_le16(const uint8_t *raw)
{
	return (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);
}

/*
 * Parse only packet-received classes.  Classes 1..5 describe abort, filter,
 * overflow, and full-queue conditions and must never be passed to the BLE
 * PDU copier as if they were successful receives.
 */
static inline int efr32_radio_rx_descriptor_parse(
	const uint8_t raw[EFR32_RADIO_RX_DESCRIPTOR_BYTES],
	struct efr32_radio_rx_descriptor *descriptor)
{
	uint8_t event_class;

	if (!raw || !descriptor) {
		return -EINVAL;
	}

	event_class = raw[2] & EFR32_RADIO_RX_DESCRIPTOR_CLASS_MASK;
	if (event_class != EFR32_RADIO_RX_DESCRIPTOR_RECEIVED_CLASS &&
	    event_class != EFR32_RADIO_RX_DESCRIPTOR_RECEIVED_ALT_CLASS) {
		return -EAGAIN;
	}

	descriptor->endpoint = efr32_radio_rx_descriptor_le16(raw) &
		EFR32_RADIO_RX_DESCRIPTOR_ENDPOINT_MASK;
	descriptor->event_class = event_class;
	descriptor->metadata = raw[3];
	return 0;
}

static inline uint16_t efr32_radio_rx_descriptor_distance(uint16_t read,
		uint16_t endpoint)
{
	return (endpoint - read) & EFR32_RADIO_RX_DESCRIPTOR_ENDPOINT_MASK;
}

static inline uint8_t efr32_radio_rx_descriptor_fifo_byte(
	const uint8_t *fifo, uint16_t offset)
{
	return fifo[offset & EFR32_RADIO_QEMU_RX_DESCRIPTOR_RING_MASK];
}

/*
 * Normalize a channel-1 circular FIFO interval into a BLE PDU.  The endpoint
 * is a 13-bit producer position; a descriptor is accepted only when its
 * interval is bounded by the configured FIFO and the PDU's own length byte.
 * The copy is transactional: target is untouched if any bound fails.
 */
static inline int efr32_radio_rx_descriptor_normalize_pdu(
	uint8_t *target, const uint8_t *fifo, uint16_t read, uint16_t endpoint,
	uint8_t max_len, uint16_t *bytes)
{
	uint16_t fifo_bytes;
	uint16_t pdu_bytes;
	uint8_t header[2];

	if (!target || !fifo || !bytes) {
		return -EINVAL;
	}

	read &= EFR32_RADIO_RX_DESCRIPTOR_ENDPOINT_MASK;
	endpoint &= EFR32_RADIO_RX_DESCRIPTOR_ENDPOINT_MASK;
	fifo_bytes = efr32_radio_rx_descriptor_distance(read, endpoint);
	if (fifo_bytes < 2U || fifo_bytes > (uint16_t)max_len + 2U ||
	    fifo_bytes > EFR32_RADIO_QEMU_RX_DESCRIPTOR_RING_BYTES) {
		return -EMSGSIZE;
	}

	header[0] = efr32_radio_rx_descriptor_fifo_byte(fifo, read);
	header[1] = efr32_radio_rx_descriptor_fifo_byte(fifo, read + 1U);
	pdu_bytes = (uint16_t)header[1] + 2U;
	if (pdu_bytes != fifo_bytes || header[1] > max_len) {
		return -EMSGSIZE;
	}

	for (uint16_t index = 0U; index < pdu_bytes; index++) {
		target[index] = efr32_radio_rx_descriptor_fifo_byte(fifo,
			read + index);
	}
	*bytes = pdu_bytes;
	return 0;
}

#endif /* EFR32_RADIO_RX_DESCRIPTOR_H_ */
