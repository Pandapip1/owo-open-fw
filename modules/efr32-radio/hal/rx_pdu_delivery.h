/*
 * Controller-facing validation for a normalized Bluetooth Link Layer PDU.
 *
 * This helper deliberately knows nothing about FRC, BUFC, or an interrupt.
 * A caller may use it only after it has established that its input is the
 * normalized representation described by the controller contract: one header
 * octet, one payload-length octet, then precisely that many payload octets.
 * Raw RX FIFO memory is not such a representation.
 */
#ifndef EFR32_RADIO_RX_PDU_DELIVERY_H_
#define EFR32_RADIO_RX_PDU_DELIVERY_H_

#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

/*
 * A normalized PDU producer may deliver at most one frame for each RX stage.
 * The LLL owns the target until it samples the completion and calls
 * radio_status_reset(), so accepting a second frame here would silently
 * overwrite the PDU the callback is about to consume.
 */
struct efr32_radio_rx_pdu_delivery {
	bool delivered;
};

static inline void efr32_radio_rx_pdu_delivery_reset(
	struct efr32_radio_rx_pdu_delivery *delivery)
{
	delivery->delivered = false;
}

/*
 * Validate and copy one normalized PDU.  The current OpenISA contract uses
 * an octet-sized payload-length field, hence bits_len must be eight.  Reject
 * every invalid input before modifying target so an eventual hardware handoff
 * cannot expose a partially copied controller buffer.
 */
static inline int efr32_radio_rx_pdu_validate_and_copy(void *target,
						const uint8_t *pdu, uint16_t bytes,
						uint8_t bits_len, uint8_t max_len)
{
	uint16_t pdu_bytes;

	if (!target || !pdu) {
		return -EINVAL;
	}

	if (bits_len != 8U || bytes < 2U) {
		return -EMSGSIZE;
	}

	pdu_bytes = (uint16_t)pdu[1] + 2U;
	if (pdu_bytes != bytes || pdu[1] > max_len) {
		return -EMSGSIZE;
	}

	memcpy(target, pdu, bytes);

	return 0;
}

/*
 * Validate and copy the sole PDU for an RX stage.  A failed validation does
 * not consume the stage, but a successful copy does; its caller decides
 * whether an invalid producer result should make the later CRC outcome fail.
 */
static inline int efr32_radio_rx_pdu_deliver_once(
	struct efr32_radio_rx_pdu_delivery *delivery, void *target,
	const uint8_t *pdu, uint16_t bytes, uint8_t bits_len, uint8_t max_len)
{
	int err;

	if (!delivery) {
		return -EINVAL;
	}

	if (delivery->delivered) {
		return -EALREADY;
	}

	err = efr32_radio_rx_pdu_validate_and_copy(target, pdu, bytes,
						   bits_len, max_len);
	if (!err) {
		delivery->delivered = true;
	}

	return err;
}

#endif /* EFR32_RADIO_RX_PDU_DELIVERY_H_ */
