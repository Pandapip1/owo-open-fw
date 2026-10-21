/* Host contract for the QEMU-only receive descriptor seam. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "rx_descriptor.h"

int main(void)
{
	struct efr32_radio_rx_descriptor descriptor;
	uint8_t raw[8] = { 0x2c, 0x01, 0x06, 0xa5, 0, 0, 0, 0 };
	uint8_t fifo[512] = { 0 };
	uint8_t target[16];
	uint8_t before[16];
	uint16_t bytes;

	assert(efr32_radio_rx_descriptor_parse(raw, &descriptor) == 0);
	assert(descriptor.endpoint == 0x12c);
	assert(descriptor.event_class == 6U);
	assert(descriptor.metadata == 0xa5U);

	raw[2] = 0x07U;
	assert(efr32_radio_rx_descriptor_parse(raw, &descriptor) == 0);
	raw[2] = 0x05U;
	assert(efr32_radio_rx_descriptor_parse(raw, &descriptor) == -EAGAIN);
	assert(efr32_radio_rx_descriptor_parse(NULL, &descriptor) == -EINVAL);

	/* A four-byte PDU ending across the 512-byte FIFO boundary. */
	fifo[510] = 0x01U;
	fifo[511] = 0x02U;
	fifo[0] = 0xa1U;
	fifo[1] = 0xa2U;
	memset(target, 0x5a, sizeof(target));
	assert(efr32_radio_rx_descriptor_normalize_pdu(target, fifo, 510U,
			514U, 2U, &bytes) == 0);
	assert(bytes == 4U);
	assert(memcmp(target, (uint8_t[]){ 0x01, 0x02, 0xa1, 0xa2 }, 4) == 0);
	assert(target[4] == 0x5aU);

	/* The interval and length field must agree, without partial writes. */
	memset(target, 0x5a, sizeof(target));
	memcpy(before, target, sizeof(target));
	fifo[511] = 0x03U;
	assert(efr32_radio_rx_descriptor_normalize_pdu(target, fifo, 510U,
			514U, 2U, &bytes) == -EMSGSIZE);
	assert(memcmp(target, before, sizeof(target)) == 0);
	assert(efr32_radio_rx_descriptor_normalize_pdu(target, fifo, 510U,
			0x20U, 2U, &bytes) == -EMSGSIZE);

	return 0;
}
