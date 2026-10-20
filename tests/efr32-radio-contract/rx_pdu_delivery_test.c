/* Hardware-independent validation for future normalized RX PDU delivery. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "rx_pdu_delivery.h"

static void expect_unchanged(const uint8_t *actual, const uint8_t *expected,
			     size_t bytes)
{
	assert(memcmp(actual, expected, bytes) == 0);
}

int main(void)
{
	struct efr32_radio_rx_pdu_delivery delivery;
	uint8_t target[257];
	uint8_t before[257];
	uint8_t short_pdu[] = { 0x01U, 0x02U, 0xa1U, 0xa2U };
	uint8_t replacement_pdu[] = { 0x01U, 0x02U, 0xb1U, 0xb2U };
	uint8_t max_pdu[257] = { 0 };

	memset(target, 0x5a, sizeof(target));
	assert(efr32_radio_rx_pdu_validate_and_copy(target, short_pdu,
						     sizeof(short_pdu), 8U, 2U) == 0);
	assert(memcmp(target, short_pdu, sizeof(short_pdu)) == 0);
	assert(target[sizeof(short_pdu)] == 0x5aU);

	memset(target, 0x5a, sizeof(target));
	memcpy(before, target, sizeof(target));
	assert(efr32_radio_rx_pdu_validate_and_copy(NULL, short_pdu,
						     sizeof(short_pdu), 8U, 2U) == -EINVAL);
	assert(efr32_radio_rx_pdu_validate_and_copy(target, NULL,
						     sizeof(short_pdu), 8U, 2U) == -EINVAL);
	expect_unchanged(target, before, sizeof(target));

	assert(efr32_radio_rx_pdu_validate_and_copy(target, short_pdu,
						     sizeof(short_pdu), 7U, 2U) == -EMSGSIZE);
	assert(efr32_radio_rx_pdu_validate_and_copy(target, short_pdu, 1U,
						     8U, 2U) == -EMSGSIZE);
	assert(efr32_radio_rx_pdu_validate_and_copy(target, short_pdu, 3U,
						     8U, 2U) == -EMSGSIZE);
	assert(efr32_radio_rx_pdu_validate_and_copy(target, short_pdu,
						     sizeof(short_pdu), 8U, 1U) == -EMSGSIZE);
	expect_unchanged(target, before, sizeof(target));

	/* The 8-bit length field permits the 255-octet payload boundary. */
	max_pdu[0] = 0x02U;
	max_pdu[1] = UINT8_MAX;
	for (uint16_t index = 2U; index < sizeof(max_pdu); index++) {
		max_pdu[index] = (uint8_t)index;
	}
	assert(efr32_radio_rx_pdu_validate_and_copy(target, max_pdu,
						     sizeof(max_pdu), 8U, UINT8_MAX) == 0);
	assert(memcmp(target, max_pdu, sizeof(max_pdu)) == 0);

	/* A valid frame owns the RX stage until its caller resets that stage. */
	memset(target, 0x5a, sizeof(target));
	efr32_radio_rx_pdu_delivery_reset(&delivery);
	assert(efr32_radio_rx_pdu_deliver_once(&delivery, target, short_pdu,
						  sizeof(short_pdu), 8U, 2U) == 0);
	memcpy(before, target, sizeof(target));
	assert(efr32_radio_rx_pdu_deliver_once(&delivery, target, replacement_pdu,
						  sizeof(replacement_pdu), 8U, 2U) == -EALREADY);
	expect_unchanged(target, before, sizeof(target));

	/* Rejection before a copy leaves the stage available to its real producer. */
	efr32_radio_rx_pdu_delivery_reset(&delivery);
	assert(efr32_radio_rx_pdu_deliver_once(&delivery, target, short_pdu, 3U,
						  8U, 2U) == -EMSGSIZE);
	assert(efr32_radio_rx_pdu_deliver_once(&delivery, target, short_pdu,
						  sizeof(short_pdu), 8U, 2U) == 0);

	return 0;
}
