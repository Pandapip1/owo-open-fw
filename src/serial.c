/*
 * The unit's serial number from its manufacturing record (devicetree compatible
 * "owo,serial-record": on the suit, twelve ASCII digits at USERDATA + 0x1C, docs/suit-board.md).
 * Read only; nothing in the firmware writes the record's page.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdint.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#define OWO_SERIAL_LEN 12

#define RECORD_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(owo_serial_record)

/*
 * Copies the record's twelve digits to out. Returns 0, -ENOENT when the board describes no record,
 * or -EINVAL when the record does not hold twelve ASCII digits (an erased page reads 0xFF).
 */
int owo_serial_read(uint8_t out[OWO_SERIAL_LEN])
{
#if DT_NODE_EXISTS(RECORD_NODE)
	BUILD_ASSERT(DT_REG_SIZE(RECORD_NODE) == OWO_SERIAL_LEN,
		     "owo,serial-record: the serial is twelve digits");
	const volatile uint8_t *record = (const volatile uint8_t *)DT_REG_ADDR(RECORD_NODE);

	for (size_t i = 0; i < OWO_SERIAL_LEN; i++) {
		uint8_t c = record[i];

		if (c < '0' || c > '9') {
			return -EINVAL;
		}
		out[i] = c;
	}
	return 0;
#else
	ARG_UNUSED(out);
	return -ENOENT;
#endif
}
