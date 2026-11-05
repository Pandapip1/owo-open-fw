/*
 * Notification of the radio's safe shutdown (a missed or infeasible deadline, or a rule the radio
 * cannot keep): the radio is forced off and the controller stops for good, so no link is kept or
 * made again until reset. An application with outputs a link controls stops them here.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_SHUTDOWN_H_
#define EFR32_RADIO_SHUTDOWN_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*efr32_radio_shutdown_cb_t)(void);

/**
 * @brief Register the function called once when the radio shuts down.
 *
 * It runs in the context that detected the failure (an interrupt, or a radio job at meta-IRQ
 * priority), after the radio is forced off and before the controller's threads are stopped, so
 * it must not block. Registered after a shutdown, it is called at once from the caller. A later
 * call replaces the function; NULL removes it.
 *
 * @param cb Function to call.
 */
void efr32_radio_shutdown_cb_set(efr32_radio_shutdown_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* EFR32_RADIO_SHUTDOWN_H_ */
