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

#include <errno.h>
#include <stdint.h>
#include <string.h>

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

#endif /* EFR32_RADIO_RX_PDU_DELIVERY_H_ */
